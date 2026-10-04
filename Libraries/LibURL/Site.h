/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibURL/Host.h>
#include <LibURL/Origin.h>

namespace URL {

// https://html.spec.whatwg.org/multipage/browsers.html#scheme-and-host
// A scheme-and-host is a tuple of a scheme (an ASCII string) and a host (a host).
struct SchemeAndHost {
    String scheme;
    Host host;
};

// https://html.spec.whatwg.org/multipage/browsers.html#site
// A site is an opaque origin or a scheme-and-host.
class Site {
public:
    static Site obtain(Origin const&);

    bool operator==(Site const& other) const { return is_same_site(other); }
    bool is_same_site(Site const& other) const;

    String serialize() const;

    // AD-HOC: The serialized site of origin that partitioned state is keyed by, or nothing if origin is opaque. An opaque
    //         origin is a site of its own that nothing else can address, so state keyed by it is never shared. Like other
    //         browsers, we treat file: URLs, whose origins are otherwise opaque, as one site.
    static Optional<Utf16String> serialize_for_partitioning(Origin const&);

private:
    Site(Variant<Origin, SchemeAndHost>);
    Variant<Origin, SchemeAndHost> m_value;
};

}
