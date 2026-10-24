/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    include <LibCore/MachPort.h>
#endif

namespace IPC {

struct TransportHandlePair;

class TransportHandle {
    AK_MAKE_NONCOPYABLE(TransportHandle);

public:
    TransportHandle() = default;
    TransportHandle(TransportHandle&&) = default;
    TransportHandle& operator=(TransportHandle&&) = default;

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    TransportHandle(Core::MachPort receive_right, Core::MachPort send_right);
#else
    explicit TransportHandle(File);
#endif

    ErrorOr<NonnullOwnPtr<Transport>> create_transport() const;

    // Gives up this end of the channel, for a transport made outside LibIPC, such as by Rust.
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    struct MachRights {
        Core::MachPort receive_right;
        Core::MachPort send_right;
    };
    MachRights release_mach_rights();
#else
    // On Windows, the descriptor is the handle of a pipe, as IPC::File holds it.
    int release_fd();
#endif

    // Both ends of a new channel, for a process that connects two others without touching the channel itself.
    static ErrorOr<TransportHandlePair> create_pair();

private:
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    mutable Core::MachPort m_receive_right;
    mutable Core::MachPort m_send_right;
#else
    mutable File m_file;
#endif

    template<typename U>
    friend ErrorOr<void> encode(Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> decode(Decoder&);
};

struct TransportHandlePair {
    TransportHandle first;
    TransportHandle second;
};

}
