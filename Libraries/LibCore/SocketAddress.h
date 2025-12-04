/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <LibCore/Export.h>

#if defined(AK_OS_WINDOWS)
constexpr inline int SOCK_STREAM = 1;
constexpr inline int SOCK_DGRAM = 2;

using ADDRESS_FAMILY = unsigned short;
constexpr inline ADDRESS_FAMILY AF_LOCAL = 1;

#    include <afunix.h>
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#endif

struct sockaddr_in;
struct sockaddr_in6;
struct sockaddr_un;

namespace Core {

class CORE_API SocketAddress {
public:
    enum class Type {
        Invalid,
        IPv4,
        IPv6,
        Local
    };

    SocketAddress();
    SocketAddress(IPv4Address const& address);
    SocketAddress(IPv6Address const& address);
    SocketAddress(IPv4Address const& address, u16 port);
    SocketAddress(IPv6Address const& address, u16 port);

    static SocketAddress local(ByteString const& address);

    Type type() const { return m_type; }
    bool is_valid() const { return m_type != Type::Invalid; }

    IPv4Address ipv4_address() const { return m_ip_address.get<IPv4Address>(); }
    IPv6Address ipv6_address() const { return m_ip_address.get<IPv6Address>(); }
    u16 port() const { return m_port; }

    ByteString to_byte_string() const;

    Optional<sockaddr_un> to_sockaddr_un() const;
    sockaddr_in6 to_sockaddr_in6() const;
    sockaddr_in to_sockaddr_in() const;

    bool operator==(SocketAddress const& other) const = default;
    bool operator!=(SocketAddress const& other) const = default;

private:
    Type m_type { Type::Invalid };

    Variant<IPv4Address, IPv6Address> m_ip_address = IPv4Address();

    u16 m_port { 0 };
    ByteString m_local_address;
};

}

template<>
struct AK::Formatter<Core::SocketAddress> : Formatter<ByteString> {
    ErrorOr<void> format(FormatBuilder& builder, Core::SocketAddress const& value)
    {
        return Formatter<ByteString>::format(builder, value.to_byte_string());
    }
};
