//
// Created by Jesson on 2024/10/5.
//

#ifndef LINUX_H
#define LINUX_H

#include <mqueue.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <uuid/uuid.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>
#include <system_error>
#include <unistd.h>

#define UUID_STRING_SIZE 36

namespacecoro::detail::linux {

using fd_t = int;

enum message_type {
	CALLBACK_TYPE,
	RESUME_TYPE
};

class safe_fd {
public:
	safe_fd() : m_fd(-1) {}
	explicit safe_fd(fd_t fd) : m_fd(fd) {}
	safe_fd(const safe_fd& other) = delete;
	safe_fd(safe_fd&& other) noexcept : m_fd(other.m_fd) {other.m_fd = -1;}
	~safe_fd() {close();}

	safe_fd& operator=(safe_fd fd) noexcept {
		swap(fd);
		return *this;
	}

	constexpr fd_t fd() const { return m_fd; }

	/// Calls close() and sets the fd to -1.
	void close() noexcept;

	void swap(safe_fd& other) noexcept {std::swap(m_fd, other.m_fd);}
	bool operator==(const safe_fd& other) const {return m_fd == other.m_fd;}
	bool operator!=(const safe_fd& other) const {return m_fd != other.m_fd;}
	bool operator==(fd_t fd) const {return m_fd == fd;}
	bool operator!=(fd_t fd) const {return m_fd != fd;}

private:
	fd_t m_fd;
};

struct message {
	enum message_type m_type;
	void* m_ptr;
};

struct io_state : linux::message {
	using callback_type = void(io_state* state);
	callback_type* m_callback;
};

class message_queue {
private:
	mqd_t m_mqdt;
	char m_qname[NAME_MAX];
	safe_fd m_epollfd;
	struct epoll_event m_ev;
	message_queue();

public:
	message_queue(size_t queue_length);
	~message_queue();
	bool enqueue_message(void* message, message_type type);
	bool dequeue_message(void*& message, message_type& type, bool wait);
};

safe_fd create_event_fd();
safe_fd create_timer_fd();
safe_fd create_epoll_fd();

}

namespacecoro::detail::linux
{
	message_queue::message_queue(size_t queue_length)
	{
		m_mqdt = -1;
		uuid_t unique_name;
		const char* cppcoro_qname_prefix = "/coro-";

		if(NAME_MAX < UUID_STRING_SIZE + strlen(cppcoro_qname_prefix) + 1)
		{
			throw std::system_error
			{
				static_cast<int>(EINVAL),
					std::system_category(),
					"Error creating message queue: system name max length too small"
					};
		}

		strncpy(m_qname, cppcoro_qname_prefix, NAME_MAX);

		for(;;)
		{
			uuid_generate(unique_name);
			uuid_unparse(unique_name, m_qname + sizeof(cppcoro_qname_prefix));

			struct mq_attr attr;
			attr.mq_flags = O_NONBLOCK;
			attr.mq_maxmsg = queue_length;
			attr.mq_msgsize = sizeof(coro::detail::linux::message);
			attr.mq_curmsgs = 0;

			m_mqdt = mq_open(m_qname, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, S_IRWXU, &attr);

			if( m_mqdt == -1 && errno == EEXIST)
			{
				continue;
			}

			if( m_mqdt == -1)
			{
				throw std::system_error
				{
					static_cast<int>(errno),
						std::system_category(),
						"Error creating io_service: message queue open"
						};
			}

			break;
		}

		m_epollfd = safe_fd{create_epoll_fd()};
		m_ev.data.fd = m_mqdt;
		m_ev.events = EPOLLIN;

		if(epoll_ctl(m_epollfd.fd(), EPOLL_CTL_ADD, m_mqdt, &m_ev) == -1)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error creating io_service: epoll ctl mqdt"
					};
		}
	}

	message_queue::~message_queue()
	{
		assert(mq_close(m_mqdt) == 0);
		assert(mq_unlink(m_qname) == 0);
	}

	bool message_queue::enqueue_message(void* msg, message_type type)
	{
		message qmsg;
		qmsg.m_type = type;
		qmsg.m_ptr = msg;
		int status = mq_send(m_mqdt, (const char*)&qmsg, sizeof(message), 0);
		return status==-1?false:true;
	}

	bool message_queue::dequeue_message(void*& msg, message_type& type, bool wait)
	{
		struct epoll_event ev = {0};
		int nfds = epoll_wait(m_epollfd.fd(), &ev, 1, wait?-1:0);

		if(nfds == -1)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error in epoll_wait run loop"
					};
		}

		if(nfds == 0 && !wait)
		{
			return false;
		}

		if(nfds == 0 && wait)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error in epoll_wait run loop"
					};
		}

		message qmsg;
		ssize_t status = mq_receive(m_mqdt, (char*)&qmsg, sizeof(message), NULL);

		if(status == -1)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error retrieving message from message queue: mq_receive"
					};
		}

		msg = qmsg.m_ptr;
		type = qmsg.m_type;
		return true;
	}

	safe_fd create_event_fd()
	{
		int fd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC);

		if(fd == -1)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error creating io_service: event fd create"
					};
		}

		return safe_fd{fd};
	}

	safe_fd create_timer_fd()
	{
		int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

		if(fd == -1)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error creating io_service: timer fd create"
					};
		}

		return safe_fd{fd};
	}

	safe_fd create_epoll_fd()
	{
		int fd = epoll_create1(EPOLL_CLOEXEC);

		if(fd == -1)
		{
			throw std::system_error
			{
				static_cast<int>(errno),
					std::system_category(),
					"Error creating timer thread: epoll create"
					};
		}

		return safe_fd{fd};
	}

	void safe_fd::close() noexcept
	{
		if(m_fd != -1)
		{
			::close(m_fd);
			m_fd = -1;
		}
	}
}

#endif //LINUX_H
