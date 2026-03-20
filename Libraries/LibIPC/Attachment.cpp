/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibIPC/Attachment.h>

namespace IPC {

Attachment::Attachment(Attachment&& other)
    : m_fd(exchange(other.m_fd, -1))
{
}

Attachment& Attachment::operator=(Attachment&& other)
{
    if (this != &other) {
        if (m_fd != -1)
            (void)Core::System::close(m_fd);
        m_fd = exchange(other.m_fd, -1);
    }
    return *this;
}

Attachment::~Attachment()
{
    if (m_fd != -1)
        (void)Core::System::close(m_fd);
}

Attachment Attachment::from_fd(int fd)
{
    Attachment attachment;
    attachment.m_fd = fd;
    return attachment;
}

ErrorOr<Attachment> Attachment::clone() const
{
    VERIFY(m_fd != -1);
    return from_fd(TRY(Core::System::dup(m_fd)));
}

int Attachment::to_fd()
{
    return exchange(m_fd, -1);
}

}
