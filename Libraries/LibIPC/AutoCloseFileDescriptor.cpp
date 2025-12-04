/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibIPC/AutoCloseFileDescriptor.h>

namespace IPC {

AutoCloseFileDescriptor::AutoCloseFileDescriptor(int fd)
    : m_fd(fd)
{
}

AutoCloseFileDescriptor::~AutoCloseFileDescriptor()
{
    if (m_fd != -1)
        (void)Core::System::close(m_fd);
}

}
