//
// Created by Jesson on 2024/10/17.
//

#include "../../include/coro/thread_pool.h"

namespace coro {

class thread_pool::thread_state {
public:
    thread_state()
        : m_local_queue(std::make_unique<std::atomic<schedule_operation*>[]>(local_queue_size))
        , m_mask(local_queue_size-1)
        , m_head(0)
        , m_tail(0)
        , m_is_sleep(false) {}

    auto try_wakeup() -> bool {
        if (m_is_sleep.load(std::memory_order_seq_cst)) {
            if (m_is_sleep.exchange(false, std::memory_order_seq_cst)) {
                m_wakeup_event.set();
                return true;
            }
        }
        return false;
    }

    auto notify_intent_to_sleep() noexcept -> void {
        m_is_sleep.store(true, std::memory_order_relaxed);
    }

    auto sleep_until_woken() noexcept -> void {
        try {
            m_wakeup_event.wait();
        }
        catch (...) {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1ms);
        }
    }

    auto has_queued_work() noexcept -> bool {
        std::scoped_lock lock(m_remote_mutex);
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_seq_cst);
        return diff(head, tail) > 0;
    }

    auto approx_has_queued_work() noexcept -> bool {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_relaxed);
        return diff(head, tail) > 0;
    }

    auto queue_size() noexcept -> size_t {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_relaxed);
        return static_cast<size_t>(diff(head, tail));
    }

    auto try_local_enqueue(schedule_operation* op) noexcept -> bool {
        auto head = m_head.load(std::memory_order_relaxed);
        auto tail = m_tail.load(std::memory_order_relaxed);
        if (diff(head, tail) < static_cast<offset_t>(m_mask)) {
            m_local_queue[head & m_mask].store(op, std::memory_order_relaxed);
            m_head.store(head + 1, std::memory_order_seq_cst);
            return true;
        }

        if (m_mask + 1 >= max_local_queue_size) {
            return false;
        }

        const size_t new_size = (m_mask + 1) * 2;
        std::unique_ptr<std::atomic<schedule_operation*>[]> new_local_queue{
            new std::atomic<schedule_operation*>[new_size]
        };
        if (!new_local_queue) {
            return false;
        }

        if (!m_remote_mutex.try_lock()) {
            return false;
        }

        std::scoped_lock lock{ std::adopt_lock, m_remote_mutex };
        tail = m_tail.load(std::memory_order_relaxed);

        const size_t new_mask = new_size - 1;
        for (auto i = tail; i != head; ++i) {
            new_local_queue[i & new_mask].store(
                m_local_queue[i & m_mask].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        new_local_queue[head & new_mask].store(op, std::memory_order_relaxed);
        m_head.store(head + 1, std::memory_order_relaxed);
        m_local_queue = std::move(new_local_queue);
        m_mask = new_mask;
        return true;
    }

    auto try_local_pop() noexcept -> schedule_operation* {
        auto head = m_head.load(std::memory_order_relaxed);
        auto tail = m_tail.load(std::memory_order_relaxed);
        if (diff(head, tail) <= 0) {
            return nullptr;
        }

        auto new_head = head - 1;
        m_head.store(new_head, std::memory_order_seq_cst);
        tail = m_tail.load(std::memory_order_seq_cst);
        if (diff(new_head, tail) < 0) {
            std::lock_guard lock{ m_remote_mutex };
            tail = m_tail.load(std::memory_order_relaxed);
            if (diff(new_head, tail) < 0) {
                m_head.store(head, std::memory_order_relaxed);
                return nullptr;
            }
        }

        return m_local_queue[new_head & m_mask].load(std::memory_order_relaxed);
    }

    auto try_steal() noexcept -> schedule_operation* {
        if (!m_remote_mutex.try_lock()) {
            return nullptr;
        }

        std::scoped_lock lock{ std::adopt_lock, m_remote_mutex };

        auto head = m_head.load(std::memory_order_seq_cst);
        auto tail = m_tail.load(std::memory_order_seq_cst);
        if (diff(head, tail) <= 0) {
            return nullptr;
        }

        m_tail.store(tail + 1, std::memory_order_seq_cst);
        head = m_head.load(std::memory_order_seq_cst);
        if (diff(head, tail + 1) >= 0) {
            return m_local_queue[tail & m_mask].load(std::memory_order_relaxed);
        }
        else {
            m_tail.store(tail, std::memory_order_relaxed);
            return nullptr;
        }
    }

private:
    using offset_t = std::make_signed_t<std::size_t>;

    // Keep each thread's local queue under 1MB
    static constexpr std::size_t max_local_queue_size = 1024 * 1024 / sizeof(void*);
    static constexpr std::size_t local_queue_size = 256;

    static constexpr offset_t diff(size_t a, size_t b) {
        return static_cast<offset_t>(a - b);
    }

    std::unique_ptr<std::atomic<schedule_operation*>[]> m_local_queue;

    std::size_t m_mask;

    std::atomic<std::size_t> m_head;
    std::atomic<std::size_t> m_tail;

    std::atomic<bool> m_is_sleep;

    spin_mutex m_remote_mutex;

    auto_reset_event m_wakeup_event;
};

thread_pool::thread_pool() : thread_pool(std::thread::hardware_concurrency()) {}

thread_pool::thread_pool(std::uint32_t thread_count)
    : m_thread_count(thread_count > 0 ? thread_count : 1)
    , m_thread_states(std::make_unique<thread_state[]>(m_thread_count))
    , m_stop(false)
    , m_global_queue(std::make_unique<std::atomic<schedule_operation*>[]>(global_queue_size))
    , m_global_mask(global_queue_size - 1)
    , m_global_head(0)
    , m_global_tail(0)
    , m_queued_work_count(0)
    , m_sleep_thread_count(0) {
    m_threads.reserve(m_thread_count);
    try {
        for (auto i = 0u; i < m_thread_count; ++i) {
            m_threads.emplace_back([this, i]() { this->run_worker_thread(i); });
        }
    }
    catch (...) {
        try {
            shutdown();
        }
        catch (...) {
            std::terminate();
        }
        throw;
    }
}

thread_pool::~thread_pool() {
    shutdown();
}

void thread_pool::schedule_operation::await_suspend(std::coroutine_handle<> handle) noexcept {
    m_awaiting_handle = handle;
    m_thread_pool->schedule_impl(this);
}

void thread_pool::schedule(std::function<void()> func) noexcept {
    auto op = std::make_unique<schedule_operation>(this, std::move(func));
    schedule_impl(std::move(op).get()); // 将所有权转移给 schedule_impl
}

void thread_pool::run_worker_thread(std::uint32_t thread_id) noexcept {
    s_cur_thread_pool = this;
    s_cur_state = &m_thread_states[thread_id];

    auto& state = *s_cur_state;
    spin_wait spinner;

    while (!m_stop.load(std::memory_order_relaxed)) {
        schedule_operation* op = nullptr;

        // Try to get a task from the local queue
        op = state.try_local_pop();

        if (!op) {
            // Try to get a task from the global queue
            op = try_global_dequeue();

            if (!op) {
                // Try to steal from other threads
                op = try_steal(thread_id);
            }
        }

        if (op) {
            m_queued_work_count.fetch_sub(1, std::memory_order_relaxed);
            try_clear_intent_to_sleep(thread_id);
            {
                // op 离开作用域后会自动销毁
                op->execute();
            }
            spinner.reset();
        }
        else {
            if (approx_has_queued_work(thread_id)) {
                spinner.spin_one();
            }
            else {
                notify_intent_to_sleep(thread_id);
                if (approx_has_queued_work(thread_id)) {
                    try_clear_intent_to_sleep(thread_id);
                    continue;
                }
                state.sleep_until_woken();
                spinner.reset();
            }
        }
    }
}

auto thread_pool::shutdown() -> void {
    m_stop.store(true, std::memory_order_relaxed);
    wake_threads(m_thread_count);

    for (auto& thread : m_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

auto thread_pool::schedule_impl(schedule_operation* op) noexcept -> void {
    m_queued_work_count.fetch_add(1, std::memory_order_relaxed);
    if (s_cur_state && s_cur_thread_pool == this) {
        if (s_cur_state->try_local_enqueue(op)) {
            return;
        }
    }
    if (!try_global_enqueue(op)) {
        // Global queue is full, drop the task or handle overflow
        // For simplicity, we'll block until we can enqueue
        std::unique_lock lock(m_global_queue_mutex);
        while (!try_global_enqueue(op)) {
            // Wait or handle overflow
            std::this_thread::yield();
        }
    }

    auto work_count = approx_total_work();
    auto sleep_threads_count = m_sleep_thread_count.load(std::memory_order_relaxed);
    auto num_threads = std::min(work_count, sleep_threads_count);
    wake_threads(num_threads);
}

auto thread_pool::try_global_enqueue(schedule_operation* op) noexcept -> bool {
    auto head = m_global_head.load(std::memory_order_relaxed);
    auto tail = m_global_tail.load(std::memory_order_acquire);
    if (diff(head, tail) < static_cast<std::make_signed_t<std::size_t>>(m_global_mask)) {
        m_global_queue[head & m_global_mask].store(op, std::memory_order_relaxed);
        m_global_head.store(head + 1, std::memory_order_release);
        return true;
    }
    return false;
}

auto thread_pool::try_global_dequeue() noexcept -> schedule_operation* {
    auto tail = m_global_tail.load(std::memory_order_relaxed);
    auto head = m_global_head.load(std::memory_order_acquire);
    if (diff(head, tail) <= 0) {
        return nullptr;
    }
    schedule_operation* op = m_global_queue[tail & m_global_mask].load(std::memory_order_relaxed);
    if (op) {
        m_global_tail.store(tail + 1, std::memory_order_release);
        return op;
    }
    return nullptr;
}

auto thread_pool::has_queued_work(std::uint32_t thread_id) noexcept -> bool {
    std::scoped_lock lock(m_global_queue_mutex);
    const auto head = m_global_head.load(std::memory_order_seq_cst);
    const auto tail = m_global_tail.load(std::memory_order_seq_cst);
    if (diff(head, tail) > 0) {
        return true;
    }

    for (auto i = 0u; i < m_thread_count; ++i) {
        if (i == thread_id) {
            continue;
        }
        if (m_thread_states[i].has_queued_work()) {
            return true;
        }
    }
    return false;
}

auto thread_pool::approx_has_queued_work(std::uint32_t thread_id) const noexcept -> bool {
    const auto head = m_global_head.load(std::memory_order_relaxed);
    const auto tail = m_global_tail.load(std::memory_order_relaxed);
    if (diff(head, tail) > 0) {
        return true;
    }

    for (auto i = 0u; i < m_thread_count; ++i) {
        if (i == thread_id) {
            continue;
        }
        if (m_thread_states[i].approx_has_queued_work()) {
            return true;
        }
    }
    return false;
}

auto thread_pool::approx_total_work() const noexcept -> std::uint32_t {
    return m_queued_work_count.load(std::memory_order_relaxed);
}

auto thread_pool::notify_intent_to_sleep(std::uint32_t thread_id) noexcept -> void {
    m_sleep_thread_count.fetch_add(1, std::memory_order_relaxed);
    m_thread_states[thread_id].notify_intent_to_sleep();
}

auto thread_pool::try_clear_intent_to_sleep(std::uint32_t thread_id) noexcept -> void {
    std::uint32_t old_sleeping_count = m_sleep_thread_count.load(std::memory_order_relaxed);
    do {
        if (old_sleeping_count == 0) {
            // No more sleeping threads.
            // Someone must have woken us up.
            return;
        }
    } while (!m_sleep_thread_count.compare_exchange_weak(
        old_sleeping_count,
        old_sleeping_count - 1,
        std::memory_order_acquire,
        std::memory_order_relaxed));

    // Then preferentially try to wake up our thread.
    // If some other thread has already requested that this thread wake up
    // then we will wake up another thread - the one that should have been woken
    // up by the thread that woke this thread up.
    if (!m_thread_states[thread_id].try_wakeup()) {
        for (auto i = 0u; i < m_thread_count; ++i) {
            if (i == thread_id) {
                continue;
            }
            if (m_thread_states[i].try_wakeup()) {
                return;
            }
        }
    }
}

auto thread_pool::try_steal(std::uint32_t cur_thread_id) noexcept -> schedule_operation* {
    schedule_operation* op = nullptr;
    std::size_t max_queue_size = 0;
    auto target_thread_id = cur_thread_id;

    // Find the thread with the maximum queue size
    for (auto i = 0u; i < m_thread_count; ++i) {
        if (i == cur_thread_id) continue;
        auto queue_size = m_thread_states[i].queue_size();
        if (queue_size > max_queue_size) {
            max_queue_size = queue_size;
            target_thread_id = i;
        }
    }

    if (target_thread_id != cur_thread_id) {
        op = m_thread_states[target_thread_id].try_steal();
    }

    return op;
}

auto thread_pool::wake_threads(std::uint32_t num_threads) noexcept -> void {
    std::scoped_lock lock{m_global_queue_mutex};

    // 尽可能地唤醒指定数量的线程
    while (num_threads > 0) {
        std::uint32_t old_sleep_count = m_sleep_thread_count.load(std::memory_order_seq_cst);
        if (old_sleep_count == 0) {
            // 没有更多的线程可以唤醒
            break;
        }
        // 尝试减少睡眠线程计数
        if (m_sleep_thread_count.compare_exchange_weak(
            old_sleep_count,
            old_sleep_count - 1,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
            // 成功减少睡眠线程计数，唤醒一个线程
            for (std::uint32_t i = 0; i < m_thread_count; ++i) {
                if (m_thread_states[i].try_wakeup()) {
                    break;
                }
            }
            --num_threads;
        }
    }
}

}


