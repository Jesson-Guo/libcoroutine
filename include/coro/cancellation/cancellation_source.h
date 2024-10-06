//
// Created by Jesson on 2024/10/3.
//

#ifndef CANCELLATION_SOURCE_H
#define CANCELLATION_SOURCE_H

#include "cancellation_state.h"

namespace coro {

class cancellation_token;

namespace detail {

class cancellation_state;

}

class cancellation_source {
public:
    /// Construct to a new cancellation source.
    cancellation_source();

    /// Create a new reference to the same underlying cancellation
    /// source as \p other.
    cancellation_source(const cancellation_source& other) noexcept;

    cancellation_source(cancellation_source&& other) noexcept;

    ~cancellation_source();

    auto operator=(const cancellation_source& other) noexcept -> cancellation_source&;

    auto operator=(cancellation_source&& other) noexcept -> cancellation_source&;

    /// Query if this cancellation source can be cancelled.
    ///
    /// A cancellation source object will not be cancellable if it has
    /// previously been moved into another cancellation_source instance
    /// or was copied from a cancellation_source that was not cancellable.
    auto can_be_cancelled() const noexcept -> bool;

    /// Obtain a cancellation token that can be used to query if
    /// cancellation has been requested on this source.
    ///
    /// The cancellation token can be passed into functions that you
    /// may want to later be able to request cancellation.
    auto token() const noexcept -> cancellation_token;

    /// Request cancellation of operations that were passed an associated
    /// cancellation token.
    ///
    /// Any cancellation callback registered via a cancellation_registration
    /// object will be called inside this function by the first thread to
    /// call this method.
    ///
    /// This operation is a no-op if can_be_cancelled() returns false.
    auto request_cancellation() -> void;

    /// Query if some thread has called 'request_cancellation()' on this
    /// cancellation_source.
    auto is_cancellation_requested() const noexcept -> bool;

private:
    detail::cancellation_state* m_state;
};

}

coro::cancellation_source::cancellation_source() : m_state(detail::cancellation_state::create()) {}

coro::cancellation_source::cancellation_source(const cancellation_source& other) noexcept
    : m_state(other.m_state) {
    if (m_state) {
        m_state->add_source_ref();
    }
}

coro::cancellation_source::cancellation_source(cancellation_source&& other) noexcept
    : m_state(other.m_state) {
    other.m_state = nullptr;
}

coro::cancellation_source::~cancellation_source() {
    if (m_state) {
        m_state->release_source_ref();
    }
}

coro::cancellation_source& coro::cancellation_source::operator=(const cancellation_source& other) noexcept {
    if (m_state != other.m_state) {
        if (m_state) {
            m_state->release_source_ref();
        }
        m_state = other.m_state;
        if (m_state) {
            m_state->add_source_ref();
        }
    }

    return *this;
}

coro::cancellation_source& coro::cancellation_source::operator=(cancellation_source&& other) noexcept {
    if (this != &other) {
        if (m_state) {
            m_state->release_source_ref();
        }
        m_state = other.m_state;
        other.m_state = nullptr;
    }

    return *this;
}

bool coro::cancellation_source::can_be_cancelled() const noexcept {
    return m_state != nullptr;
}

coro::cancellation_token coro::cancellation_source::token() const noexcept {
    return cancellation_token(m_state);
}

void coro::cancellation_source::request_cancellation() {
    if (m_state) {
        m_state->request_cancellation();
    }
}

bool coro::cancellation_source::is_cancellation_requested() const noexcept {
    return m_state && m_state->is_cancellation_requested();
}

#endif //CANCELLATION_SOURCE_H
