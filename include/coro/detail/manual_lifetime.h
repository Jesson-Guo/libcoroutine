//
// Created by Jesson on 2024/10/1.
//

#ifndef MANUAL_LIFETIME_H
#define MANUAL_LIFETIME_H

#include <type_traits>
#include <memory>

namespace coro::detail {
	template<typename T>
	struct manual_lifetime {
	    manual_lifetime() noexcept {}
		~manual_lifetime() noexcept {}

		manual_lifetime(const manual_lifetime&) = delete;
		manual_lifetime(manual_lifetime&&) = delete;
		manual_lifetime& operator=(const manual_lifetime&) = delete;
		manual_lifetime& operator=(manual_lifetime&&) = delete;

		template<typename... Args>
		auto construct(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>)
	    -> std::enable_if_t<std::is_constructible_v<T, Args&&...>> {
			::new (static_cast<void*>(std::addressof(m_value))) T(static_cast<Args&&>(args)...);
		}

		auto destruct() noexcept(std::is_nothrow_destructible_v<T>) -> void {
			m_value.~T();
		}

		std::add_pointer_t<T> operator->() noexcept { return std::addressof(**this); }
		std::add_pointer_t<const T> operator->() const noexcept { return std::addressof(**this); }

		T& operator*() & noexcept { return m_value; }
		const T& operator*() const & noexcept { return m_value; }
		T&& operator*() && noexcept { return static_cast<T&&>(m_value); }
		const T&& operator*() const && noexcept { return static_cast<const T&&>(m_value); }

	private:
		union {
			T m_value;
		};
	};

	template<typename T>
	struct manual_lifetime<T&> {
	    manual_lifetime() noexcept {}
		~manual_lifetime() noexcept {}

		manual_lifetime(const manual_lifetime&) = delete;
		manual_lifetime(manual_lifetime&&) = delete;
		manual_lifetime& operator=(const manual_lifetime&) = delete;
		manual_lifetime& operator=(manual_lifetime&&) = delete;

		auto construct(T& value) noexcept -> void {
			m_value = std::addressof(value);
		}

		auto destruct() noexcept -> void {}

		T* operator->() noexcept { return m_value; }
		const T* operator->() const noexcept { return m_value; }

		T& operator*() noexcept { return *m_value; }
		const T& operator*() const noexcept { return *m_value; }

	private:
		T* m_value;
	};

	template<typename T>
	struct manual_lifetime<T&&> {
	    manual_lifetime() noexcept {}
		~manual_lifetime() noexcept {}

		manual_lifetime(const manual_lifetime&) = delete;
		manual_lifetime(manual_lifetime&&) = delete;
		manual_lifetime& operator=(const manual_lifetime&) = delete;
		manual_lifetime& operator=(manual_lifetime&&) = delete;

		auto construct(T&& value) noexcept -> void {
			m_value = std::addressof(value);
		}

		auto destruct() noexcept -> void {}

		T* operator->() noexcept { return m_value; }
		const T* operator->() const noexcept { return m_value; }

		T& operator*() & noexcept { return *m_value; }
		const T& operator*() const & noexcept { return *m_value; }
		T&& operator*() && noexcept { return static_cast<T&&>(*m_value); }
		const T&& operator*() const && noexcept { return static_cast<const T&&>(*m_value); }

	private:
		T* m_value;
	};

	template<>
	struct manual_lifetime<void> {
		void construct() noexcept {}
		void destruct() noexcept {}
		void operator*() const noexcept {}
	};
}

#endif //MANUAL_LIFETIME_H
