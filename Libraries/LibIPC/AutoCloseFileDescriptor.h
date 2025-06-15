/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibCore/System.h>

namespace IPC {

class AutoCloseFileDescriptor : public RefCounted<AutoCloseFileDescriptor> {
public:
    AutoCloseFileDescriptor(int fd)
        : m_fd(fd)
    {
    }

    ~AutoCloseFileDescriptor()
    {
        if (m_fd != -1)
            (void)Core::System::close(m_fd);
    }

    int value() const { return m_fd; }

    int take_fd()
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

private:
    int m_fd;
};

}
