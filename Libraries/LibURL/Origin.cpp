/*
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>

namespace URL {

Origin Origin::create_opaque(OpaqueData::Type type)
{
    return Origin { OpaqueData { get_random<OpaqueData::Nonce>(), type } };
}

Origin::Origin(Optional<String> const& scheme, Host const& host, Optional<u16> port, Optional<String> domain)
    : m_state(Tuple {
          .scheme = scheme,
          .host = host,
          .port = move(port),
          .domain = move(domain),
      })
{
}
// https://html.spec.whatwg.org/multipage/origin.html#same-origin
bool Origin::is_same_origin(Origin const& other) const
{
    // 1. If A and B are the same opaque origin, then return true.
    if (is_opaque() && other.is_opaque())
        return opaque_data().nonce == other.opaque_data().nonce;

    // 2. If A and B are both tuple origins and their schemes, hosts, and port are identical, then return true.
    if (!is_opaque() && !other.is_opaque()
        && scheme() == other.scheme()
        && host() == other.host()
        && port() == other.port()) {
        return true;
    }

    // 3. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/origin.html#same-origin-domain
bool Origin::is_same_origin_domain(Origin const& other) const
{
    // 1. If A and B are the same opaque origin, then return true.
    if (is_opaque() && other.is_opaque())
        return opaque_data().nonce == other.opaque_data().nonce;

    // 2. If A and B are both tuple origins, run these substeps:
    if (!is_opaque() && !other.is_opaque()) {
        // 1. If A and B's schemes are identical, and their domains are identical and non-null, then return true.
        if (domain().has_value()
            && domain() == other.domain()
            && scheme() == other.scheme())
            return true;

        // 2. Otherwise, if A and B are same origin and their domains are identical and null, then return true.
        if (!domain().has_value()
            && !other.domain().has_value()
            && is_same_origin(other))
            return true;
    }

    // 3. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/origin.html#concept-origin-effective-domain
Optional<Host> Origin::effective_domain() const
{
    // 1. If origin is an opaque origin, then return null.
    if (is_opaque())
        return {};

    // 2. If origin's domain is non-null, then return origin's domain.
    auto const& tuple = m_state.get<Tuple>();
    if (tuple.domain.has_value())
        return Host { tuple.domain.value() };

    // 3. Return origin's host.
    return tuple.host;
}

// https://html.spec.whatwg.org/multipage/browsers.html#same-site
bool Origin::is_same_site(Origin const& other) const
{
    // 1. Let siteA be the result of obtaining a site given A.
    auto site_a = Site::obtain(*this);

    // 2. Let siteB be the result of obtaining a site given B.
    auto site_b = Site::obtain(other);

    // 3. If siteA is same site with siteB, then return true.
    if (site_a.is_same_site(site_b))
        return true;

    // 4. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/origin.html#ascii-serialisation-of-an-origin
String Origin::serialize() const
{
    // 1. If origin is an opaque origin, then return "null"
    if (is_opaque())
        return "null"_string;

    // 2. Otherwise, let result be origin's scheme.
    StringBuilder result;
    result.append(scheme().value_or(String {}));

    // 3. Append "://" to result.
    result.append("://"sv);

    // 4. Append origin's host, serialized, to result.
    result.append(host().serialize());

    // 5. If origin's port is non-null, append a U+003A COLON character (:), and origin's port, serialized, to result.
    if (port().has_value()) {
        result.append(':');
        result.append(String::number(*port()));
    }

    // 6. Return result
    return result.to_string_without_validation();
}

}

namespace AK {

unsigned Traits<URL::Origin>::hash(URL::Origin const& origin)
{
    if (origin.is_opaque()) {
        auto const& nonce = origin.opaque_data().nonce;
        // Random data, so the first u32 of the nonce is as good as hashing the entire thing.
        return (static_cast<u32>(nonce[0]) << 24)
            | (static_cast<u32>(nonce[1]) << 16)
            | (static_cast<u32>(nonce[2]) << 8)
            | (static_cast<u32>(nonce[3]));
    }

    unsigned hash = origin.scheme().value_or(String {}).hash();

    if (origin.port().has_value())
        hash = pair_int_hash(hash, *origin.port());

    hash = pair_int_hash(hash, origin.host().serialize().hash());

    if (origin.domain().has_value())
        hash = pair_int_hash(hash, origin.domain()->hash());

    return hash;
}

} // namespace AK
