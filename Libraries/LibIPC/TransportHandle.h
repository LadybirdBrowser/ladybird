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

namespace IPC {

#if !defined(AK_OS_WINDOWS)
class TransportSocket;
using Transport = TransportSocket;
#else
class TransportSocketWindows;
using Transport = TransportSocketWindows;
#endif

class TransportHandle {
    AK_MAKE_NONCOPYABLE(TransportHandle);

public:
    TransportHandle() = default;
    TransportHandle(TransportHandle&&) = default;
    TransportHandle& operator=(TransportHandle&&) = default;

    static ErrorOr<TransportHandle> from_transport(Transport& transport);

    ErrorOr<NonnullOwnPtr<Transport>> create_transport() const;

private:
    explicit TransportHandle(File);

    template<typename U>
    friend ErrorOr<void> encode(Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> decode(Decoder&);

    mutable File m_file;
};

}
