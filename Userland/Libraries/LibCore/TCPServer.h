/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IPv4Address.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>

namespace Core {

class TCPServer : public EventReceiver {
    C_OBJECT_ABSTRACT(TCPServer)
public:
    static ErrorOr<NonnullRefPtr<TCPServer>> try_create(EventReceiver* parent = nullptr);
    virtual ~TCPServer() override;

    enum class AllowAddressReuse {
        Yes,
        No,
    };

    bool is_listening() const { return m_listening; }
    ErrorOr<void> listen(IPv4Address const& address, u16 port, AllowAddressReuse = AllowAddressReuse::No);
    ErrorOr<void> set_blocking(bool blocking);

    ErrorOr<NonnullOwnPtr<TCPSocket>> accept();

    Optional<IPv4Address> local_address() const;
    Optional<u16> local_port() const;

    Function<void()> on_ready_to_accept;

private:
    explicit TCPServer(int fd, EventReceiver* parent = nullptr);

    int m_fd { -1 };
    bool m_listening { false };
    RefPtr<Notifier> m_notifier;
};

}
