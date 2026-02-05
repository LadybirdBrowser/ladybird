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
    , m_ptr(adopt_own_if_nonnull(ptr))
{
}

AddressInfoVector::~AddressInfoVector() = default;

void AddressInfoVector::AddrInfoDeleter::operator()(struct addrinfo* ptr)
{
    if (ptr)
        ::freeaddrinfo(ptr);
}

}
