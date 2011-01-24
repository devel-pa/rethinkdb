#include "arch/linux/network.hpp"
#include "arch/linux/thread_pool.hpp"
#include "logger.hpp"
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

/* Network connection object */

linux_net_conn_t::linux_net_conn_t(const char *host, int port) {
    not_implemented();
}

linux_net_conn_t::linux_net_conn_t(fd_t sock)
    : sock(sock),
      registration_thread(-1), set_me_true_on_delete(NULL),
      read_mode(read_mode_none), in_read_buffered_cb(false),
      write_mode(write_mode_none),
      read_was_shut_down(false), write_was_shut_down(false),
      registered_for_writes(false)
{
    assert(sock != INVALID_FD);

    int res = fcntl(sock, F_SETFL, O_NONBLOCK);
    guarantee_err(res == 0, "Could not make socket non-blocking");
}

void linux_net_conn_t::register_with_event_loop() {
    /* Register ourself to receive notifications from the event loop if we have not
    already done so. */

    if (registration_thread == -1) {
        registration_thread = linux_thread_pool_t::thread_id;
        linux_thread_pool_t::thread->queue.watch_resource(sock, poll_event_in, this);

    } else if (registration_thread != linux_thread_pool_t::thread_id) {
        guarantee(registration_thread == linux_thread_pool_t::thread_id,
            "Must always use a net_conn_t on the same thread.");
    }
}

void linux_net_conn_t::read_external(void *buf, size_t size, linux_net_conn_read_external_callback_t *cb) {

    assert(!read_was_shut_down);
    register_with_event_loop();
    assert(sock != INVALID_FD);
    assert(read_mode == read_mode_none);

    read_mode = read_mode_external;
    external_read_buf = (char *)buf;
    external_read_size = size;
    read_external_cb = cb;
    assert(read_external_cb);

    // If we were reading in read_mode_buffered before this read, we might have
    // read more bytes than necessary, in which case the peek buffer will still
    // contain some data. Drain it out first.
    int peek_buffer_bytes = std::min(peek_buffer.size(), external_read_size);
    memcpy(external_read_buf, peek_buffer.data(), peek_buffer_bytes);
    peek_buffer.erase(peek_buffer.begin(), peek_buffer.begin() + peek_buffer_bytes);
    external_read_buf += peek_buffer_bytes;
    external_read_size -= peek_buffer_bytes;

    try_to_read_external_buf();
}

void linux_net_conn_t::try_to_read_external_buf() {
    assert(read_mode == read_mode_external);

    while (external_read_size > 0) {
        assert(external_read_buf);
        int res = ::read(sock, external_read_buf, external_read_size);

        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // We'll get called again via on_event() when there is more data
                return;
            } else if (errno == ECONNRESET || errno == ENOTCONN) {
                // Socket was closed
                on_shutdown_read();
                return;
            } else {
                // This is not expected, but it will probably happen sometime so we shouldn't crash
                logERR("Could not read from socket: %s", strerror(errno));
                on_shutdown_read();
                return;
            }

        } else if (res == 0) {
            // Socket was closed
            on_shutdown_read();
            return;

        } else {
            external_read_size -= res;
            external_read_buf += res;
        }
    }

    if (external_read_size == 0) {
        // The request has been fulfilled
        read_mode = read_mode_none;
        read_external_cb->on_net_conn_read_external();
    }
}

void linux_net_conn_t::read_buffered(linux_net_conn_read_buffered_callback_t *cb) {

    assert(!read_was_shut_down);
    register_with_event_loop();
    assert(sock != INVALID_FD);
    assert(read_mode == read_mode_none);

    read_mode = read_mode_buffered;
    read_buffered_cb = cb;

    // We call see_if_callback_is_satisfied() first because there might be data
    // already in the peek buffer, or the callback might be satisfied with an empty
    // peek buffer.

    if (!see_if_callback_is_satisfied()) put_more_data_in_peek_buffer();
}

void linux_net_conn_t::put_more_data_in_peek_buffer() {
    assert(read_mode == read_mode_buffered);

    // Grow the peek buffer so we have some space to put what we're about to insert
    int old_size = peek_buffer.size();
    peek_buffer.resize(old_size + IO_BUFFER_SIZE);

    int res = ::read(sock, peek_buffer.data() + old_size, IO_BUFFER_SIZE);

    if (res == -1) {
        // Reset the temporary increase in peek buffer size
        peek_buffer.resize(old_size);

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // We will get a callback via on_event() at a later date
            return;
        } else if (errno == ECONNRESET || errno == ENOTCONN) {
            // Socket was closed
            on_shutdown_read();
        } else {
            // This is not expected, but it will probably happen sometime so we shouldn't crash
            logERR("Could not read from socket: %s", strerror(errno));
            on_shutdown_read();
        }

    } else if (res == 0) {
        // Reset the temporary increase in peek buffer size
        peek_buffer.resize(old_size);

        // Socket was closed
        on_shutdown_read();

    } else {
        // Shrink the peek buffer so that its 'size' member is only how many bytes are
        // actually in it. Its internal array will probably not shrink.
        peek_buffer.resize(old_size + res);

        if (!see_if_callback_is_satisfied()) {
            // There might be more data in the kernel buffer
            put_more_data_in_peek_buffer();
        }
    }
}

bool linux_net_conn_t::see_if_callback_is_satisfied() {
    assert(read_mode == read_mode_buffered);
    assert(!in_read_buffered_cb);

    in_read_buffered_cb = true;   // Make it legal to call accept_buffer()

    /* Weird dance to figure out if we got deleted by the callback, while also making it possible
    for on_event() to know if we got deleted */
    bool deleted = false;
    bool *prev_set_me_true_on_delete = set_me_true_on_delete;
    set_me_true_on_delete = &deleted;

    read_buffered_cb->on_net_conn_read_buffered(peek_buffer.data(), peek_buffer.size());

    if (deleted) {
        if (prev_set_me_true_on_delete) {
            *prev_set_me_true_on_delete = true;   // Tell on_event()
        }
        return true;
    } else {
        set_me_true_on_delete = prev_set_me_true_on_delete;
    }

    if (in_read_buffered_cb) {
        // accept_buffer() was not called; our offer was rejected.
        in_read_buffered_cb = false;
        return false;

    } else {
        // accept_buffer() was called and it set in_read_buffered_cb to false. It also
        // already removed the appropriate amount of data from the peek buffer and set
        // the read mode to read_mode_none. The read_buffered_cb might have gone on to
        // start another read, though, so there's no guarantee that the read mode is
        // still read_mode_none.
        return true;

    }
}

void linux_net_conn_t::accept_buffer(size_t bytes) {
    assert(read_mode == read_mode_buffered);
    assert(in_read_buffered_cb);

    assert(bytes <= peek_buffer.size());
    peek_buffer.erase(peek_buffer.begin(), peek_buffer.begin() + bytes);

    // So that the callback can start another read after calling accept_buffer()
    in_read_buffered_cb = false;
    read_mode = read_mode_none;
}

void linux_net_conn_t::write_external(const void *buf, size_t size, linux_net_conn_write_external_callback_t *cb) {

    assert(!write_was_shut_down);
    register_with_event_loop();
    assert(sock != INVALID_FD);
    assert(write_mode == write_mode_none);

    write_mode = write_mode_external;
    external_write_buf = (const char *)buf;
    external_write_size = size;
    write_external_cb = cb;
    assert(write_external_cb);
    try_to_write_external_buf();
}

void linux_net_conn_t::try_to_write_external_buf() {
    assert(write_mode == write_mode_external);

    while (external_write_size > 0) {
        assert(external_write_buf);
        int res = ::write(sock, external_write_buf, external_write_size);
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Register ourselves so we'll get called on_event()
                 * when we're readable again. We can't do this during
                 * construction because on level-triggered systems
                 * on_event will spin the cpu, and starve out
                 * signals. Plenty of legacy systems do this, and I
                 * had to work this weekend to fix this because
                 * whoever refactored this last time fucked over
                 * customers with legacy systems. Please don't do this
                 * again. */
                linux_thread_pool_t::thread->queue.adjust_resource(sock, poll_event_in|poll_event_out, this);
                registered_for_writes = true;
                return;
            } else if (errno == EPIPE || errno == ENOTCONN || errno == EHOSTUNREACH ||
                    errno == ENETDOWN || errno == EHOSTDOWN || errno == ECONNRESET) {
                /* We expect that some of these errors could happen in practice, so just
                shut down nicely */
                on_shutdown_write();
                return;
            } else {
                /* In theory this should never happen, but in practice it probably will because
                I probably didn't account for all possible error messages. So instead of crashing,
                we write a log error message and then shut down gracefully. */
                logERR("Could not write to socket: %s", strerror(errno));
                on_shutdown_write();
                return;
            }
        } else if (res == 0) {
            /* This should also probably never happen, but it's better to write an error message
            than to crash completely. */
            logERR("Didn't expect write() to return 0");
            on_shutdown_write();
            return;
        } else {
            external_write_size -= res;
            external_write_buf += res;
        }
    }

    if (external_write_size == 0) {
        // Deregister our write notification so we don't get flooded
        // on legacy systems that use level-triggered IO.
        if(registered_for_writes) {
            linux_thread_pool_t::thread->queue.adjust_resource(sock, poll_event_in, this);
            registered_for_writes = false;
        }
        
        // The request has been fulfilled
        write_mode = write_mode_none;
        write_external_cb->on_net_conn_write_external();
    }
}

void linux_net_conn_t::shutdown_read() {
    assert(!in_read_buffered_cb, "Please don't call net_conn_t::shutdown() from within "
        "on_net_conn_read_buffered() without calling accept_buffer(). The net_conn_t is "
        "sort of stupid and you just broke its fragile little mind.");

    int res = ::shutdown(sock, SHUT_RD);
    if (res != 0 && errno != ENOTCONN) {
        logERR("Could not shutdown socket for reading: %s", strerror(errno));
    }

    on_shutdown_read();
}

void linux_net_conn_t::on_shutdown_read() {
    assert(!read_was_shut_down);
    assert(sock != INVALID_FD);
    read_was_shut_down = true;

    // Deregister ourself with the event loop. If the write half of the connection
    // is still open, the make sure we stay registered for write.

    if (registration_thread != -1) {
        assert(registration_thread == linux_thread_pool_t::thread_id);
        if (write_was_shut_down) {
            linux_thread_pool_t::thread->queue.forget_resource(sock, this);
        } else {
            linux_thread_pool_t::thread->queue.adjust_resource(sock, poll_event_out, this);
        }
    }

    // Inform any readers that were waiting that the socket has been closed.

    // If there are no readers, nothing gets informed until an attempt is made to read. Is this a
    // problem?

    switch (read_mode) {
        case read_mode_none: break;
        case read_mode_external: read_external_cb->on_net_conn_close(); break;
        case read_mode_buffered: read_buffered_cb->on_net_conn_close(); break;
        default: unreachable();
    }
}

bool linux_net_conn_t::is_read_open() {
    return !read_was_shut_down;
}

void linux_net_conn_t::shutdown_write() {

    int res = ::shutdown(sock, SHUT_WR);
    if (res != 0 && errno != ENOTCONN) {
        logERR("Could not shutdown socket for writing: %s", strerror(errno));
    }

    on_shutdown_write();
}

void linux_net_conn_t::on_shutdown_write() {
    assert(!write_was_shut_down);
    assert(sock != INVALID_FD);
    write_was_shut_down = true;

    // Deregister ourself with the event loop. If the read half of the connection
    // is still open, the make sure we stay registered for read.

    if (registration_thread != -1) {
        assert(registration_thread == linux_thread_pool_t::thread_id);
        if (read_was_shut_down) {
            linux_thread_pool_t::thread->queue.forget_resource(sock, this);
        } else {
            linux_thread_pool_t::thread->queue.adjust_resource(sock, poll_event_in, this);
        }
    }

    // Inform any writers that were waiting that the socket has been closed.

    // If there are no writers, nothing gets informed until an attempt is made to write. Is this a
    // problem?

    switch (write_mode) {
        case write_mode_none: break;
        case write_mode_external: write_external_cb->on_net_conn_close(); break;
        default: unreachable();
    }
}

bool linux_net_conn_t::is_write_open() {
    return !write_was_shut_down;
}

linux_net_conn_t::~linux_net_conn_t() {
    // sock would be INVALID_FD if our sock was stolen by a
    // linux_oldstyle_net_conn_t.
    if (sock != INVALID_FD) {
        // So on_event() doesn't call us after we got deleted
        if (set_me_true_on_delete) *set_me_true_on_delete = true;

        assert(read_was_shut_down);
        assert(write_was_shut_down);

        int res = ::close(sock);
        if (res != 0) {
            logERR("close() failed: %s", strerror(errno));
        }
    }
}

void linux_net_conn_t::on_event(int events) {

    assert(sock != INVALID_FD);

    // So we get notified if 'this' gets deleted and don't try to do any more
    // operations with it.
    bool deleted = false;
    set_me_true_on_delete = &deleted;

    if (events & poll_event_in) {
        assert(!read_was_shut_down);
        switch (read_mode) {
            case read_mode_none: break;
            case read_mode_external: try_to_read_external_buf(); break;
            case read_mode_buffered: put_more_data_in_peek_buffer(); break;
            default: unreachable();
        }
        if (deleted) return;
    }

    // Check write_was_shut_down in case a read callback called shutdown_write()
    if ((events & poll_event_out) && !write_was_shut_down) {
        assert(!write_was_shut_down);
        switch (write_mode) {
            case write_mode_none: break;
            case write_mode_external: try_to_write_external_buf(); break;
            default: unreachable();
        }
        if (deleted) return;
    }

    if ((events & poll_event_err) && (events & poll_event_hup)) {
        /* We get this when the socket is closed but there is still data we are trying to send.
        For example, it can sometimes be reproduced by sending "nonsense\r\n" and then sending
        "set [key] 0 0 [length] noreply\r\n[value]\r\n" a hundred times then immediately closing the
        socket.
        
        I speculate that the "error" part comes from the fact that there is undelivered data
        in the socket send buffer, and the "hup" part comes from the fact that the remote end
        has hung up.
        
        Ignore it; the other logic will handle it properly. */
        
    } else if (events & poll_event_err) {
        /* We don't know why we got this, so shut the hell down. */
        logERR("Unexpected poll_event_err. Events: %d\n", events);
        if (!read_was_shut_down) shutdown_read();
        if (deleted) return;   // read callback could call shutdown_write() then delete us
        if (!write_was_shut_down) shutdown_write();
        set_me_true_on_delete = NULL;
        return;
    }

    set_me_true_on_delete = NULL;
}

/* Network listener object */

linux_net_listener_t::linux_net_listener_t(int port)
    : callback(NULL)
{
    defunct = false;

    int res;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    guarantee_err(sock != INVALID_FD, "Couldn't create socket");

    int sockoptval = 1;
    res = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &sockoptval, sizeof(sockoptval));
    guarantee_err(res != -1, "Could not set REUSEADDR option");

    /* XXX Making our socket NODELAY prevents the problem where responses to
     * pipelined requests are delayed, since the TCP Nagle algorithm will
     * notice when we send mulitple small packets and try to coalesce them. But
     * if we are only sending a few of these small packets quickly, like during
     * pipeline request responses, then Nagle delays for around 40 ms before
     * sending out those coalesced packets if they don't reach the max window
     * size. So for latency's sake we want to disable Nagle.
     *
     * This might decrease our throughput, so perhaps we should add a
     * runtime option for it.
     *
     * - Jordan 12/22/10
     */
    res = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockoptval, sizeof(sockoptval));
    guarantee_err(res != -1, "Could not set TCP_NODELAY option");

    // Bind the socket
    sockaddr_in serv_addr;
    bzero((char*)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    res = bind(sock, (sockaddr*)&serv_addr, sizeof(serv_addr));
    if (res != 0) {
        fprintf(stderr, "Couldn't bind socket: %s\n", strerror(errno));
        // We cannot simply terminate here, since this may lead to corrupted database files.
        // defunct myself and rely on server to handle this condition and shutdown gracefully...
        defunct = true;
        return;
    }

    // Start listening to connections
    res = listen(sock, 5);
    guarantee_err(res == 0, "Couldn't listen to the socket");

    res = fcntl(sock, F_SETFL, O_NONBLOCK);
    guarantee_err(res == 0, "Could not make socket non-blocking");
}

void linux_net_listener_t::set_callback(linux_net_listener_callback_t *cb) {
    if (defunct)
        return;

    assert(!callback);
    assert(cb);
    callback = cb;

    linux_thread_pool_t::thread->queue.watch_resource(sock, poll_event_in, this);
}

void linux_net_listener_t::on_event(int events) {
    if (defunct)
        return;

    if (events != poll_event_in) {
        logERR("Unexpected event mask: %d\n", events);
    }

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int new_sock = accept(sock, (sockaddr*)&client_addr, &client_addr_len);

        if (new_sock == INVALID_FD) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else {
                switch (errno) {
                    case EPROTO:
                    case ENOPROTOOPT:
                    case ENETDOWN:
                    case ENONET:
                    case ENETUNREACH:
                    case EINTR:
                        break;
                    default:
                        // We can't do anything about failing accept, but we still
                        // must continue processing current connections' request.
                        // Thus, we can't bring down the server, and must ignore
                        // the error.
                        logERR("Cannot accept new connection: %s\n", strerror(errno));
                        break;
                }
            }
        } else {
            callback->on_net_listener_accept(new linux_net_conn_t(new_sock));
        }
    }
}

linux_net_listener_t::~linux_net_listener_t() {
    if (defunct)
        return;

    int res;

    if (callback) linux_thread_pool_t::thread->queue.forget_resource(sock, this);

    res = shutdown(sock, SHUT_RDWR);
    guarantee_err(res == 0, "Could not shutdown main socket");

    res = close(sock);
    guarantee_err(res == 0, "Could not close main socket");
}

