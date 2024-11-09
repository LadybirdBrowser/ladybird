/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
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
using Host = Variant<IPv4Address, IPv6Address, String, Empty>;

}
