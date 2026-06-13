/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AddressInfoVector.h>

#if defined(AK_OS_WINDOWS)
#    include <ws2tcpip.h>
#else
#    include <netdb.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#endif

namespace Core::System {

AddressInfoVector::AddressInfoVector(Vector<struct addrinfo> addresses, struct addrinfo* ptr)
    : m_addresses(move(addresses))
    , m_ptr(ptr)
{
}

AddressInfoVector::AddressInfoVector(AddressInfoVector&& other)
    : m_addresses(move(other.m_addresses))
    , m_ptr(exchange(other.m_ptr, nullptr))
{
}

AddressInfoVector::~AddressInfoVector()
{
    if (m_ptr)
        ::freeaddrinfo(m_ptr);
}

void AddressInfoVector::swap(AddressInfoVector& other)
{
    AK::swap(m_addresses, other.m_addresses);
    AK::swap(m_ptr, other.m_ptr);
}

}
