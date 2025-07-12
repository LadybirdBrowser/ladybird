/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibURL/Forward.h>
#include <LibURL/Host.h>

namespace URL {

class Origin {
public:
    using Nonce = Array<u8, 16>;

    explicit Origin(Nonce nonce)
        : m_state(move(nonce))
    {
    }

    static Origin create_opaque();

    Origin(Optional<String> const& scheme, Host const& host, Optional<u16> port)
        : m_state(Tuple {
              .scheme = scheme,
              .host = host,
              .port = move(port),
          })
    {
    }

    // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-opaque
    bool is_opaque() const { return m_state.has<Nonce>(); }

    Optional<String> const& scheme() const { return m_state.get<Tuple>().scheme; }
    Host const& host() const { return m_state.get<Tuple>().host; }
    Optional<u16> port() const { return m_state.get<Tuple>().port; }

    Nonce const& nonce() const { return m_state.get<Nonce>(); }

    // https://html.spec.whatwg.org/multipage/origin.html#same-origin
    bool is_same_origin(Origin const& other) const
    {
        // 1. If A and B are the same opaque origin, then return true.
        if (is_opaque() && other.is_opaque())
            return nonce() == other.nonce();

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
    bool is_same_origin_domain(Origin const& other) const
    {
        // 1. If A and B are the same opaque origin, then return true.
        if (is_opaque() && other.is_opaque())
            return nonce() == other.nonce();

        // 2. If A and B are both tuple origins, run these substeps:
        if (!is_opaque() && !other.is_opaque()) {
            // 1. If A and B's schemes are identical, and their domains are identical and non-null, then return true.
            // FIXME: Check domains once supported.
            if (scheme() == other.scheme())
                return true;

            // 2. Otherwise, if A and B are same origin and their domains are identical and null, then return true.
            // FIXME: Check domains once supported.
            if (is_same_origin(other))
                return true;
        }

        // 3. Return false.
        return false;
    }

    // https://html.spec.whatwg.org/multipage/browsers.html#same-site
    bool is_same_site(Origin const&) const;

    // https://html.spec.whatwg.org/multipage/origin.html#ascii-serialisation-of-an-origin
    String serialize() const;

    // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-effective-domain
    Optional<Host> effective_domain() const
    {
        // 1. If origin is an opaque origin, then return null.
        if (is_opaque())
            return {};

        // FIXME: 2. If origin's domain is non-null, then return origin's domain.

        // 3. Return origin's host.
        return m_state.get<Tuple>().host;
    }

    bool operator==(Origin const& other) const { return is_same_origin(other); }

private:
    struct Tuple {
        Optional<String> scheme;
        Host host;
        Optional<u16> port;
    };

    Variant<Tuple, Nonce> m_state;
};

}

namespace AK {

template<>
struct Traits<URL::Origin> : public DefaultTraits<URL::Origin> {
    static unsigned hash(URL::Origin const& origin);
};

} // namespace AK
