/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/String.h>
#include <AK/Types.h>

namespace URL {

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

    Optional<String> public_suffix() const;
    Optional<String> registrable_domain() const;

    String serialize() const;

private:
    VariantType m_value;
};

}
