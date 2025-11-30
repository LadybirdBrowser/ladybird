/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>

struct addrinfo;

namespace Core::System {

class AddressInfoVector {
    AK_MAKE_NONCOPYABLE(AddressInfoVector);
    AK_MAKE_DEFAULT_MOVABLE(AddressInfoVector);

public:
    AddressInfoVector(Vector<struct addrinfo> addresses, struct addrinfo* ptr);
    ~AddressInfoVector();

    ReadonlySpan<struct addrinfo> addresses() const { return m_addresses; }

private:
    struct AddrInfoDeleter {
        void operator()(struct addrinfo*);
    };

    Vector<struct addrinfo> m_addresses;
    OwnPtr<struct addrinfo, AddrInfoDeleter> m_ptr;
};

}
