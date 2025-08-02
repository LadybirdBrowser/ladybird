/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/Types.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>

namespace Core {

// FIXME: Implement on Windows

ErrorOr<NonnullRefPtr<TCPServer>> TCPServer::try_create(EventReceiver*)
{
    return Error::from_string_literal("TCPServer::try_create() is not implemented on Windows");
}

TCPServer::TCPServer(int fd, EventReceiver* parent)
    : EventReceiver(parent)
    , m_fd(fd)
{
    VERIFY(m_fd >= 0);
}

TCPServer::~TCPServer()
{
    MUST(Core::System::close(m_fd));
}

ErrorOr<void> TCPServer::listen(IPv4Address const&, u16, AllowAddressReuse)
{

    return Error::from_string_literal("TCPServer::listen() is not implemented on Windows");
}

ErrorOr<void> TCPServer::set_blocking(bool)
{
    return Error::from_string_literal("TCPServer::set_blocking() is not implemented on Windows");
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPServer::accept()
{
    return Error::from_string_literal("TCPServer::accept() is not implemented on Windows");
}

Optional<IPv4Address> TCPServer::local_address() const
{
    return {};
}

Optional<u16> TCPServer::local_port() const
{
    return {};
}

}
