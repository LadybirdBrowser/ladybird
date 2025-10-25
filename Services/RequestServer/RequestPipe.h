/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>

namespace RequestServer {

class RequestPipe : public RefCounted<RequestPipe> {
public:
    static ErrorOr<NonnullRefPtr<RequestPipe>> try_create();

    ~RequestPipe();

    int reader_fd() const { return m_reader_fd; }
    int writer_fd() const { return m_writer_fd; }

    ErrorOr<ssize_t> write(Vector<u8> const& bytes);

private:
    explicit RequestPipe(int reader_fd, int writer_fd)
        : m_reader_fd(reader_fd)
        , m_writer_fd(writer_fd)
    {
        VERIFY(m_reader_fd >= 0);
        VERIFY(m_writer_fd >= 0);
    }

    int m_reader_fd { -1 };
    int m_writer_fd { -1 };
};

}
