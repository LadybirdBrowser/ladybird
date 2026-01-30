/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <errno.h>

namespace Web::WebAudio::Render {

// Callers are expected to use nonblocking fds.

inline bool try_signal_eventfd(int fd)
{
    if (fd < 0)
        return false;

    // eventfd expects an 8-byte counter increment. This also works for pipes.
    u64 one = 1;
    ssize_t nwritten = ::write(fd, &one, sizeof(one));
    if (nwritten == static_cast<ssize_t>(sizeof(one)))
        return true;

    // Nonblocking: treat EAGAIN/EWOULDBLOCK as a coalesced signal.
    if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return true;

    return false;
}

// Signal a stream notification fd (eventfd on linux, pipe elsewhere)
//
// For portability we always attempt an 8-byte write; pipes happily accept this.
inline bool try_signal_stream_notify_fd(int fd)
{
    return try_signal_eventfd(fd);
}

inline bool try_signal_pipe(int fd)
{
    if (fd < 0)
        return false;

    u8 one = 1;
    ssize_t nwritten = ::write(fd, &one, sizeof(one));
    if (nwritten == static_cast<ssize_t>(sizeof(one)))
        return true;

    if (nwritten < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return true;

    return false;
}

inline void drain_eventfd(int fd)
{
    if (fd < 0)
        return;

    u64 value = 0;
    while (true) {
        ssize_t nread = ::read(fd, &value, sizeof(value));
        if (nread == static_cast<ssize_t>(sizeof(value)))
            continue;
        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        break;
    }
}

inline void drain_pipe(int fd)
{
    if (fd < 0)
        return;

    u8 buffer[64];
    while (true) {
        ssize_t nread = ::read(fd, buffer, sizeof(buffer));
        if (nread > 0)
            continue;
        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        break;
    }
}

inline void drain_stream_notify_fd(int fd)
{
    if (fd < 0)
        return;

    // Try eventfd-style draining first.
    u64 value = 0;
    while (true) {
        ssize_t nread = ::read(fd, &value, sizeof(value));
        if (nread == static_cast<ssize_t>(sizeof(value)))
            continue;
        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;

        // If this isn't an eventfd (e.g. pipe read end), fall back to byte draining.
        if (nread > 0 || (nread < 0 && errno == EINVAL))
            drain_pipe(fd);
        return;
    }
}

}
