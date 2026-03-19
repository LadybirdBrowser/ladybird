/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>

#if defined(AK_OS_MACOS)
#    include <LibCore/MachPort.h>
#endif

namespace IPC {

class Attachment {
    AK_MAKE_NONCOPYABLE(Attachment);

public:
    Attachment() = default;
    Attachment(Attachment&&);
    Attachment& operator=(Attachment&&);
    ~Attachment();

    static Attachment from_fd(int fd);
    int to_fd();

#if defined(AK_OS_MACOS)
    static Attachment from_mach_port(Core::MachPort, Core::MachPort::MessageRight);
    Core::MachPort const& mach_port() const { return m_port; }
    Core::MachPort::MessageRight message_right() const { return m_message_right; }
    Core::MachPort release_mach_port();
#endif

private:
#if defined(AK_OS_MACOS)
    Core::MachPort m_port;
    Core::MachPort::MessageRight m_message_right { Core::MachPort::MessageRight::MoveSend };
#else
    int m_fd { -1 };
#endif
};

}
