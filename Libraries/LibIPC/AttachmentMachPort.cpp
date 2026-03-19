/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/MachPort.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>

// fileport_makeport() and fileport_makefd() are private macOS APIs that convert
// between file descriptors and Mach port rights. Since Mach messages can only
// carry port rights (not file descriptors), we convert FDs to "file ports" for
// transmission and convert them back on the receiving side. These APIs are stable
// and used by Apple's own frameworks (including XPC).
extern "C" {
int fileport_makeport(int fd, mach_port_t* port);
int fileport_makefd(mach_port_t port);
}

namespace IPC {

Attachment::Attachment(Attachment&& other)
    : m_port(move(other.m_port))
    , m_message_right(other.m_message_right)
{
}

Attachment& Attachment::operator=(Attachment&& other)
{
    if (this != &other) {
        m_port = move(other.m_port);
        m_message_right = other.m_message_right;
    }
    return *this;
}

Attachment::~Attachment() = default;

Attachment Attachment::from_fd(int fd)
{
    mach_port_t port = MACH_PORT_NULL;
    auto result = fileport_makeport(fd, &port);
    (void)Core::System::close(fd);
    VERIFY(result == 0);
    return from_mach_port(Core::MachPort::adopt_right(port, Core::MachPort::PortRight::Send), Core::MachPort::MessageRight::MoveSend);
}

int Attachment::to_fd()
{
    VERIFY(MACH_PORT_VALID(m_port.port()));
    int fd = fileport_makefd(m_port.port());
    VERIFY(fd >= 0);
    mach_port_deallocate(mach_task_self(), m_port.release());
    return fd;
}

Attachment Attachment::from_mach_port(Core::MachPort port, Core::MachPort::MessageRight right)
{
    VERIFY(MACH_PORT_VALID(port.port()));
    Attachment attachment;
    attachment.m_port = move(port);
    attachment.m_message_right = right;
    return attachment;
}

Core::MachPort Attachment::release_mach_port()
{
    VERIFY(MACH_PORT_VALID(m_port.port()));
    return move(m_port);
}

}
