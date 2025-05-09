/*
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only or BSD-2-Clause
 */

#ifndef _IO_MUX_CALL_H
#define _IO_MUX_CALL_H

#include <exception>
#include <sys/time.h>

#include <util/vtypes.h>
#include <util/xlio_stats.h>
#include <sock/sockinfo.h>
#include <sock/sockinfo.h>

// from sigset.h
#ifndef sigandnset
#define sigandnset(dest, left, right)                                                              \
    (__extension__({                                                                               \
        int __cnt = _SIGSET_NWORDS;                                                                \
        sigset_t *__dest = (dest);                                                                 \
        const sigset_t *__left = (left);                                                           \
        const sigset_t *__right = (right);                                                         \
        while (--__cnt >= 0)                                                                       \
            __dest->__val[__cnt] = (__left->__val[__cnt] & ~(__right->__val[__cnt]));              \
        0;                                                                                         \
    }))
#endif

#define CHECK_INTERRUPT_RATIO 0

extern timeval g_last_zero_polling_time; // the last time g_polling_time_usec was zeroed

/**
 * @class mux_call
 * Base class for IO multiplexing system calls - select,poll,epoll_wait
 */
class io_mux_call {
public:
    enum offloaded_mode_t {
        OFF_NONE = 0x0,
        OFF_READ = 0x1,
        OFF_WRITE = 0x2,
        OFF_RDWR = OFF_READ | OFF_WRITE // offloaded for both read & write
    };

    /**
     * Create a multiplexing call.
     * @param fds_buffer Pointer to a buffer large enough to hold all fds.
     */
    io_mux_call(int *off_fds_buffer, offloaded_mode_t *off_modes_buffer, int num_fds = 0,
                const sigset_t *sigmask = nullptr); // = 0 is only temp
    virtual ~io_mux_call() {};

    /**
     * Sets an offloaded file descriptor as ready.
     * @param fd_index Index in offloaded_fds array.
     * @return Whether fd was added.
     *         Also updates m_n_ready_rfds
     */
    virtual void set_offloaded_rfd_ready(int fd_index) = 0;
    virtual void set_offloaded_wfd_ready(int fd_index) = 0;

    /**
     * Sets a file descriptor as ready.
     * @param fd_index Index in offloaded_fds array.
     * @return Whether fd was added.
     *         Also updates m_n_ready_rfds
     */
    virtual void set_rfd_ready(int fd) = 0;
    virtual void set_wfd_ready(int fd) = 0;
    virtual void set_efd_ready(int fd, int errors) = 0;
    /**
     * Prepare to poll on fds
     */
    virtual void prepare_to_poll() {};

    /**
     * Prepare to block on fds.
     * Set m_cq_epfd.
     */
    virtual void prepare_to_block() = 0;

    /**
     * Waits on original file descriptors only.
     * Updates m_n_all_ready_fds.
     * @param zero_timeout If true, wait with zero timeout.
     *                     If false, wait with original timeout.
     * @throws io_mux_call::io_error
     */
    virtual bool wait_os(bool zero_timeout) = 0;

    /**
     * Blocks until any fd (or cq_epfd) are ready, or timeout expires.
     * Updates the timeout with time remaining.
     * Updates m_n_all_ready_fds.
     *
     * @param elapsed Time elapsed since the call start.
     *   Should wait at most (timeout - elapsed).
     * @return true if cq_epfd is ready.
     * @throws io_mux_call::io_error
     */
    virtual bool wait(const timeval &elapsed) = 0;

    /**
     * Checks if there is a timeout (used in polling loops).
     * @param elapsed Time elapsed since the call start.
     * @return true if elapsed > timeout, false otherwise.
     */
    virtual bool is_timeout(const timeval &elapsed) = 0;

    /**
     * Call the function.
     * @return Number of ready fds.
     * @throws io_mux_call::io_error
     *
     * This is how it works:
     * No offloaded sockets - redirect the call to OS.
     * Otherwise:
     *  Loop N times until found or timeout: Poll all offloaded sockets, if
     *    nothing is found poll OS.
     * If nothing is found yet:
     *  Loop until found or timeout: Arm the CQ and block on offloaded sockets
     *  plus CQ epfd. If CQ is found, poll offloaded sockets. If something else
     *  is found, return it.
     */
    int call();

    static inline void update_fd_array(fd_array_t *p_fd_array, int fd)
    {
        if (p_fd_array && (p_fd_array->fd_count < p_fd_array->fd_max)) {
            // Check that fd doesn't exist in the array
            for (int i = (p_fd_array->fd_count - 1); i >= 0; i--) {
                if (p_fd_array->fd_list[i] == fd) {
                    return;
                }
            }
            p_fd_array->fd_list[p_fd_array->fd_count] = fd;
            p_fd_array->fd_count++;
        }
    }

    virtual bool immidiate_return(int &poll_os_countdown);
    /**
     * @class io_error
     * Exception by OS IO functions.
     */

    class io_error : public xlio_exception {
    public:
        io_error(const char *_message, const char *_function, const char *_filename, int _lineno,
                 int _errnum) throw()
            : xlio_exception(_message, _function, _filename, _lineno, _errnum)
        {
        }
    };

private:
    /**
     * Go over offloaded fd's and check if their sockinfo is ready.
     * If ready, calls set_offloaded_rfd_ready() & set_offloaded_wfd_ready() on that fd.
     * @return If all attached CQs were darined.
     */
    virtual bool check_all_offloaded_sockets();
    inline void check_offloaded_rsockets();
    inline void check_offloaded_wsockets();
    inline void check_offloaded_esockets();

    /**
     * Loop: Poll the CQ and check for ready fds
     */
    void polling_loops();

    /**
     * Loop: Block on CQ and check for ready fds
     */
    void blocking_loops();

    /**
     * Internal timer update
     * Used to update the elapsed time for is_timeout() calls
     */
    inline void timer_update();

    /**
     * Check if the polling CPU var needs to be zeroed
     * (internal for the statistics)
     */
    inline void zero_polling_cpu(timeval current);

    /**
     * Go over fd_ready_array and set all fd's in it as ready.
     * @return Whether anything was found in the array.
     */
    inline void check_rfd_ready_array(fd_array_t *fd_ready_array);

    /**
     * check if we have signal pending and the call need to be interrupted
     */
    inline bool is_sig_pending();

    /// counts the number times os poll was skipped
    static int m_n_skip_os_count;

    int m_check_sig_pending_ratio;

public:
protected:
    virtual bool ring_poll_and_process_element();

    virtual int ring_request_notification();

    virtual void ring_wait_for_notification_and_process_element(void *pv_fd_ready_array);

    bool handle_os_countdown(int &poll_os_countdown);

    /// Pointer to an array of all offloaded fd's
    int *m_p_all_offloaded_fds;
    offloaded_mode_t *m_p_offloaded_modes;

    //---  read handling
    /// Number of offloaded fd's
    int m_num_all_offloaded_fds;

    /// Pointer to the number of offloaded fd's
    int *m_p_num_all_offloaded_fds;

    //--
    /// CQ epoll file descriptor (wrapper)
    int m_cqepfd;

    /// poll sn
    uint64_t m_poll_sn_rx;
    uint64_t m_poll_sn_tx;

    /// xlio statistics. each implementation must initialize this.
    iomux_func_stats_t *m_p_stats;

    /// timer managment
    timeval m_start, m_elapsed;

    /// number of total ready fds (r + w + x)
    int m_n_all_ready_fds;

    // TODO: consider removing m_n_ready_rfds & m_n_ready_wfds
    /// number of ready r fds
    int m_n_ready_rfds;

    /// number of ready w fds
    int m_n_ready_wfds;

    /// number of ready e fds
    int m_n_ready_efds;

    /// collect the ready fds in the begining of the call
    fd_array_t m_fd_ready_array;

    const sigset_t *m_sigmask;
};

#endif
