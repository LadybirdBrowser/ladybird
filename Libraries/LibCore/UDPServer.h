/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Alexander Narsudinov <a.narsudinov@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Forward.h>
#include <LibCore/SocketAddress.h>

namespace Core {

class UDPServer : public EventReceiver {
    C_OBJECT(UDPServer)
public:
    virtual ~UDPServer() override;

    bool is_bound() const { return m_bound; }

    bool bind(IPv4Address const& address, u16 port);
    ErrorOr<ByteBuffer> receive(size_t size, sockaddr_in& from);
    ErrorOr<ByteBuffer> receive(size_t size)
    {
        struct sockaddr_in saddr;
        return receive(size, saddr);
    }

    ErrorOr<size_t> send(ReadonlyBytes, sockaddr_in const& to);

    Optional<IPv4Address> local_address() const;
    Optional<u16> local_port() const;

    int fd() const { return m_fd; }

    Function<void()> on_ready_to_receive;

protected:
    explicit UDPServer(EventReceiver* parent = nullptr);

private:
    int m_fd { -1 };
    bool m_bound { false };
    RefPtr<Notifier> m_notifier;
};

}
