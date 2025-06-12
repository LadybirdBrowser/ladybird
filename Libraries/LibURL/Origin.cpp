/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>

namespace URL {

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
    if (origin.is_opaque())
        return 0;

    unsigned hash = origin.scheme().value_or(String {}).hash();

    if (origin.port().has_value())
        hash = pair_int_hash(hash, *origin.port());

    hash = pair_int_hash(hash, origin.host().serialize().hash());

    return hash;
}

} // namespace AK
