/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>

namespace RequestServer {

class RequestPipe {
    AK_MAKE_NONCOPYABLE(RequestPipe);

public:
    RequestPipe(RequestPipe&& other);
    RequestPipe& operator=(RequestPipe&& other);
    ~RequestPipe();

    static ErrorOr<RequestPipe> create();

    int reader_fd() const { return m_reader_fd; }
    int writer_fd() const { return m_writer_fd; }

    ErrorOr<ssize_t> write(ReadonlyBytes bytes);

private:
    RequestPipe(int reader_fd, int writer_fd);

    int m_reader_fd { -1 };
    int m_writer_fd { -1 };
};

}
