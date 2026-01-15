/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/Types.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>

#include <AK/Windows.h>

namespace Core {

LocalServer::LocalServer() = default;

LocalServer::~LocalServer()
{
    if (static_cast<SOCKET>(m_fd) != INVALID_SOCKET)
        MUST(Core::System::close(m_fd));
}

ErrorOr<void> LocalServer::take_over_fd(int socket_fd)
{
    if (m_listening)
        return Error::from_string_literal("Core::LocalServer: Can't perform socket takeover when already listening");

    m_fd = socket_fd;
    m_listening = true;
    setup_notifier();
    return {};
}

void LocalServer::setup_notifier()
{
    m_notifier = Notifier::construct(m_fd, Notifier::Type::Read);
    m_notifier->on_activation = [this] {
        if (on_accept) {
            auto maybe_client_socket = accept();
            if (maybe_client_socket.is_error()) {
                dbgln("LocalServer::on_ready_to_read: Error accepting a connection: {}", maybe_client_socket.error());
                if (on_accept_error)
                    on_accept_error(maybe_client_socket.release_error());
                return;
            }

            on_accept(maybe_client_socket.release_value());
        }
    };
}

bool LocalServer::listen(ByteString const& address)
{
    if (m_listening)
        return false;

    m_fd = MUST(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
    int option = 1;
    MUST(Core::System::ioctl(m_fd, FIONBIO, &option));
    auto const ret = SetHandleInformation(to_handle(m_fd), HANDLE_FLAG_INHERIT, 0);
    VERIFY(ret != 0);

    auto socket_address = SocketAddress::local(address);
    auto un_optional = socket_address.to_sockaddr_un();
    if (!un_optional.has_value()) {
        perror("bind");
        return false;
    }

    auto un = un_optional.value();
    if (Core::System::bind(m_fd, (sockaddr const*)&un, sizeof(un)).is_error()) {
        perror("bind");
        return false;
    }
    if (Core::System::listen(m_fd, 5).is_error()) {
        perror("listen");
        return false;
    }

    m_listening = true;
    setup_notifier();
    return true;
}

ErrorOr<NonnullOwnPtr<LocalSocket>> LocalServer::accept()
{
    VERIFY(m_listening);
    sockaddr_un un;
    socklen_t un_size = sizeof(un);
    int accepted_fd = TRY(Core::System::accept(m_fd, (sockaddr*)&un, &un_size));
    return LocalSocket::adopt_fd(accepted_fd);
}

}
