/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/String.h>
#include <AK/Types.h>

namespace URL {

// https://url.spec.whatwg.org/#concept-ipv4
// An IPv4 address is a 32-bit unsigned integer that identifies a network address. [RFC791]
// FIXME: It would be nice if this were an AK::IPv4Address
using IPv4Address = u32;

// https://url.spec.whatwg.org/#concept-ipv6
// An IPv6 address is a 128-bit unsigned integer that identifies a network address. For the purposes of this standard
// it is represented as a list of eight 16-bit unsigned integers, also known as IPv6 pieces. [RFC4291]
// FIXME: It would be nice if this were an AK::IPv6Address
using IPv6Address = Array<u16, 8>;

// https://url.spec.whatwg.org/#concept-host
// A host is a domain, an IP address, an opaque host, or an empty host. Typically a host serves as a network address,
// but it is sometimes used as opaque identifier in URLs where a network address is not necessary.
class Host {
public:
    using VariantType = Variant<IPv4Address, IPv6Address, String>;
    Host(VariantType&&);
    Host(String&&);

    bool is_domain() const;
    bool is_empty_host() const;

    template<typename T>
    bool has() const { return m_value.template has<T>(); }

    template<typename T>
    T const& get() const { return m_value.template get<T>(); }

    bool operator==(Host const& other) const = default;

    VariantType const& value() const { return m_value; }

    String serialize() const;

private:
    VariantType m_value;
};

}
