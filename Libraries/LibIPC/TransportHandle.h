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

#if defined(AK_OS_MACOS)
#    include <LibCore/MachPort.h>
#endif

namespace IPC {

class TransportHandle {
    AK_MAKE_NONCOPYABLE(TransportHandle);

public:
    TransportHandle() = default;
    TransportHandle(TransportHandle&&) = default;
    TransportHandle& operator=(TransportHandle&&) = default;

#if defined(AK_OS_MACOS)
    TransportHandle(Core::MachPort receive_right, Core::MachPort send_right);
#else
    explicit TransportHandle(File);
#endif

    ErrorOr<NonnullOwnPtr<Transport>> create_transport() const;

private:
#if defined(AK_OS_MACOS)
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

}
