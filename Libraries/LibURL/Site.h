/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    bool is_same_site(Site const& other) const;

    String serialize() const;

private:
    Site(Variant<Origin, SchemeAndHost>);
    Variant<Origin, SchemeAndHost> m_value;
};

}
