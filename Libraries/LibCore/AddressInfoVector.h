/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

struct addrinfo;

namespace Core::System {

class AddressInfoVector {
    AK_MAKE_NONCOPYABLE(AddressInfoVector);

public:
    AddressInfoVector(Vector<struct addrinfo> addresses, struct addrinfo* ptr);
    AddressInfoVector(AddressInfoVector&& other);
    ~AddressInfoVector();

    AddressInfoVector& operator=(AddressInfoVector&& other)
    {
        AddressInfoVector temporary { move(other) };
        swap(temporary);
        return *this;
    }

    ReadonlySpan<struct addrinfo> addresses() const { return m_addresses; }

private:
    void swap(AddressInfoVector& other);

    Vector<struct addrinfo> m_addresses;
    struct addrinfo* m_ptr { nullptr };
};

}
