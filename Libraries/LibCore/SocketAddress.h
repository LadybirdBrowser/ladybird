/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>

#ifndef AK_OS_WINDOWS
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#else
#    include "SocketAddressWindows.h"
#endif

namespace Core {

class SocketAddress {
public:
    enum class Type {
        Invalid,
        IPv4,
        IPv6,
        Local
    };

    SocketAddress() = default;
    SocketAddress(IPv4Address const& address)
        : m_type(Type::IPv4)
        , m_ip_address { address }
    {
    }

    SocketAddress(IPv6Address const& address)
        : m_type(Type::IPv6)
        , m_ip_address { address }
    {
    }

    SocketAddress(IPv4Address const& address, u16 port)
        : m_type(Type::IPv4)
        , m_ip_address { address }
        , m_port(port)
    {
    }

    SocketAddress(IPv6Address const& address, u16 port)
        : m_type(Type::IPv6)
        , m_ip_address { address }
        , m_port(port)
    {
    }

    static SocketAddress local(ByteString const& address)
    {
        SocketAddress addr;
        addr.m_type = Type::Local;
        addr.m_local_address = address;
        return addr;
    }

    Type type() const { return m_type; }
    bool is_valid() const { return m_type != Type::Invalid; }

    IPv4Address ipv4_address() const { return m_ip_address.get<IPv4Address>(); }
    IPv6Address ipv6_address() const { return m_ip_address.get<IPv6Address>(); }
    u16 port() const { return m_port; }

    ByteString to_byte_string() const
    {
        switch (m_type) {
        case Type::IPv4:
            return ByteString::formatted("{}:{}", m_ip_address.get<IPv4Address>(), m_port);
        case Type::IPv6:
            return ByteString::formatted("[{}]:{}", m_ip_address.get<IPv6Address>(), m_port);
        case Type::Local:
            return m_local_address;
        default:
            return "[SocketAddress]";
        }
    }

    Optional<sockaddr_un> to_sockaddr_un() const
    {
        VERIFY(type() == Type::Local);
        sockaddr_un address;
        address.sun_family = AF_LOCAL;
        bool fits = m_local_address.copy_characters_to_buffer(address.sun_path, sizeof(address.sun_path));
        if (!fits)
            return {};
        return address;
    }

    sockaddr_in6 to_sockaddr_in6() const
    {
        VERIFY(type() == Type::IPv6);
        sockaddr_in6 address {};
        memset(&address, 0, sizeof(address));
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port());
        auto ipv6_addr = ipv6_address();
        memcpy(&address.sin6_addr, &ipv6_addr.to_in6_addr_t(), sizeof(address.sin6_addr));
        return address;
    }

    sockaddr_in to_sockaddr_in() const
    {
        VERIFY(type() == Type::IPv4);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port());
        address.sin_addr.s_addr = ipv4_address().to_in_addr_t();
        return address;
    }

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
