/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

TransportHandle::TransportHandle(File file)
    : m_file(move(file))
{
}

ErrorOr<NonnullOwnPtr<Transport>> TransportHandle::create_transport() const
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(m_file.take_fd()));
    TRY(socket->set_blocking(false));
    return make<Transport>(move(socket));
}

template<>
ErrorOr<void> encode(Encoder& encoder, TransportHandle const& handle)
{
    return encoder.encode(handle.m_file);
}

template<>
ErrorOr<TransportHandle> decode(Decoder& decoder)
{
    auto file = TRY(decoder.decode<File>());
    return TransportHandle { move(file) };
}

}
