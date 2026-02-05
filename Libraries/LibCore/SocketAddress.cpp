/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SocketAddress.h>

#if defined(AK_OS_WINDOWS)
// SOCK_STREAM and SOCK_DGRAM are defined with #define. Let's cache our custom definitions before including Windows
// headers to ensure we defined them correctly.
static constexpr auto LADYBIRD_SOCK_STREAM = SOCK_STREAM;
static constexpr auto LADYBIRD_SOCK_DGRAM = SOCK_DGRAM;

#    include <AK/Windows.h>
#    include <ws2tcpip.h>

static_assert(LADYBIRD_SOCK_STREAM == SOCK_STREAM);
static_assert(LADYBIRD_SOCK_DGRAM == SOCK_DGRAM);
static_assert(AF_LOCAL == AF_UNIX);
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#endif

namespace Core {

SocketAddress::SocketAddress() = default;

SocketAddress::SocketAddress(IPv4Address const& address)
    : m_type(Type::IPv4)
    , m_ip_address { address }
{
}

SocketAddress::SocketAddress(IPv6Address const& address)
    : m_type(Type::IPv6)
    , m_ip_address { address }
{
}

SocketAddress::SocketAddress(IPv4Address const& address, u16 port)
    : m_type(Type::IPv4)
    , m_ip_address { address }
    , m_port(port)
{
}

SocketAddress::SocketAddress(IPv6Address const& address, u16 port)
    : m_type(Type::IPv6)
    , m_ip_address { address }
    , m_port(port)
{
}

SocketAddress SocketAddress::local(ByteString const& address)
{
    SocketAddress addr;
    addr.m_type = Type::Local;
    addr.m_local_address = address;
    return addr;
}

ByteString SocketAddress::to_byte_string() const
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

Optional<sockaddr_un> SocketAddress::to_sockaddr_un() const
{
    VERIFY(type() == Type::Local);
    sockaddr_un address;
    address.sun_family = AF_LOCAL;
    bool fits = m_local_address.copy_characters_to_buffer(address.sun_path, sizeof(address.sun_path));
    if (!fits)
        return {};
    return address;
}

sockaddr_in6 SocketAddress::to_sockaddr_in6() const
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

sockaddr_in SocketAddress::to_sockaddr_in() const
{
    VERIFY(type() == Type::IPv4);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port());
    address.sin_addr.s_addr = ipv4_address().to_in_addr_t();
    return address;
}

}
