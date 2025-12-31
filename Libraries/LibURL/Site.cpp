/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Site.h>

namespace URL {

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-a-site
Site Site::obtain(Origin const& origin)
{
    // 1. If origin is an opaque origin, then return origin.
    if (origin.is_opaque())
        return Site { origin };

    // 2. If origin's host's registrable domain is null, then return (origin's scheme, origin's host).
    auto origin_scheme = origin.scheme().value_or_lazy_evaluated([]() { return String {}; });

    auto registrable_domain = origin.host().registrable_domain();
    if (!registrable_domain.has_value())
        return Site { SchemeAndHost { origin_scheme, origin.host() } };

    // 3. Return (origin's scheme, origin's host's registrable domain).
    return Site { SchemeAndHost { origin_scheme, Host { *registrable_domain } } };
}

Site::Site(Variant<Origin, SchemeAndHost> value)
    : m_value(move(value))
{
}

// https://html.spec.whatwg.org/multipage/browsers.html#concept-site-same-site
bool Site::is_same_site(Site const& other) const
{
    // 1. If A and B are the same opaque origin, then return true.
    // NOTE: Origins in sites are always opaque.
    if (m_value.has<Origin>() && other.m_value.has<Origin>())
        return m_value.get<Origin>().opaque_data().nonce == other.m_value.get<Origin>().opaque_data().nonce;

    // 2. If A or B is an opaque origin, then return false.
    if (m_value.has<Origin>() || other.m_value.has<Origin>())
        return false;

    // 3. If A's and B's scheme values are different, then return false.
    auto& a = m_value.get<SchemeAndHost>();
    auto& b = other.m_value.get<SchemeAndHost>();
    if (a.scheme != b.scheme)
        return false;

    // 4. If A's and B's host values are not equal, then return false.
    if (a.host != b.host)
        return false;

    // 5. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/browsers.html#serialization-of-a-site
String Site::serialize() const
{
    // 1. If site is an opaque origin, then return "null".
    if (m_value.has<Origin>())
        return "null"_string;

    // 2. Let result be site[0].
    auto& [scheme, host] = m_value.get<SchemeAndHost>();
    StringBuilder result;
    result.append(scheme);

    // 3. Append "://" to result.
    result.append("://"sv);

    // 4. Append site[1], serialized, to result.
    result.append(host.serialize());

    // 5. Return result.
    return result.to_string_without_validation();
}

}
