/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifndef AK_OS_MACH
#    error "MachPort is only available on Mach platforms"
#endif

#include <AK/Error.h>
#include <AK/Noncopyable.h>

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    include <mach/mach.h>
#else
extern "C" {
#    include <mach/mach.h>
}
#endif

namespace Core {

// https://www.gnu.org/software/hurd/gnumach-doc/Major-Concepts.html#Major-Concepts
class MachPort {
    AK_MAKE_NONCOPYABLE(MachPort);

public:
    // https://www.gnu.org/software/hurd/gnumach-doc/Exchanging-Port-Rights.html#Exchanging-Port-Rights
    enum class PortRight : mach_port_right_t {
        Send = MACH_PORT_RIGHT_SEND,
        Receive = MACH_PORT_RIGHT_RECEIVE,
        SendOnce = MACH_PORT_RIGHT_SEND_ONCE,
        PortSet = MACH_PORT_RIGHT_PORT_SET,
        DeadName = MACH_PORT_RIGHT_DEAD_NAME,
    };

    enum class MessageRight : mach_msg_type_name_t {
        MoveReceive = MACH_MSG_TYPE_MOVE_RECEIVE,
        MoveSend = MACH_MSG_TYPE_MOVE_SEND,
        MoveSendOnce = MACH_MSG_TYPE_MOVE_SEND_ONCE,
        CopySend = MACH_MSG_TYPE_COPY_SEND,
        MakeSend = MACH_MSG_TYPE_MAKE_SEND,
        MakeSendOnce = MACH_MSG_TYPE_MAKE_SEND_ONCE,
#if defined(AK_OS_MACOS)
        CopyReceive = MACH_MSG_TYPE_COPY_RECEIVE,
        DisposeReceive = MACH_MSG_TYPE_DISPOSE_RECEIVE,
        DisposeSend = MACH_MSG_TYPE_DISPOSE_SEND,
        DisposeSendOnce = MACH_MSG_TYPE_DISPOSE_SEND_ONCE,
#endif
    };

    MachPort() = default;
    MachPort(MachPort&& other);
    MachPort& operator=(MachPort&& other);
    ~MachPort();

    mach_port_t release();

    static ErrorOr<MachPort> create_with_right(PortRight);
    static MachPort adopt_right(mach_port_t, PortRight);

    ErrorOr<MachPort> insert_right(MessageRight);

#if defined(AK_OS_MACOS)
    // https://opensource.apple.com/source/launchd/launchd-842.92.1/liblaunch/bootstrap.h.auto.html
    static ErrorOr<MachPort> look_up_from_bootstrap_server(ByteString const& service_name);
    ErrorOr<void> register_with_bootstrap_server(ByteString const& service_name);
#endif

    // FIXME: mach_msg wrapper? For now just let the owner poke into the internals
    mach_port_t port() const { return m_port; }

private:
    MachPort(PortRight, mach_port_t);

    void unref_port();

    PortRight m_right { PortRight::DeadName };
    mach_port_t m_port { MACH_PORT_NULL };
};

Error mach_error_to_error(kern_return_t error);

}
