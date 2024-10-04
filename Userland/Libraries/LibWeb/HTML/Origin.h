/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

namespace Web::HTML {

class Origin {
public:
    Origin() = default;
    Origin(Optional<ByteString> const& scheme, URL::Host const& host, u16 port)
        : m_scheme(scheme)
        , m_host(host)
        , m_port(port)
    {
    }

    // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-opaque
    bool is_opaque() const { return !m_scheme.has_value() && m_host.has<Empty>() && m_port == 0; }

    StringView scheme() const
    {
        return m_scheme.map([](auto& str) { return str.view(); }).value_or(StringView {});
    }
    URL::Host const& host() const { return m_host; }
    u16 port() const { return m_port; }

    // https://html.spec.whatwg.org/multipage/origin.html#same-origin
    bool is_same_origin(Origin const& other) const
    {
        // 1. If A and B are the same opaque origin, then return true.
        if (is_opaque() && other.is_opaque())
            return true;

        // 2. If A and B are both tuple origins and their schemes, hosts, and port are identical, then return true.
        // 3. Return false.
        return scheme() == other.scheme()
            && host() == other.host()
            && port() == other.port();
    }

    // https://html.spec.whatwg.org/multipage/origin.html#same-origin-domain
    bool is_same_origin_domain(Origin const& other) const
    {
        // 1. If A and B are the same opaque origin, then return true.
        if (is_opaque() && other.is_opaque())
            return true;

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

    // https://html.spec.whatwg.org/multipage/origin.html#ascii-serialisation-of-an-origin
    ByteString serialize() const
    {
        // 1. If origin is an opaque origin, then return "null"
        if (is_opaque())
            return "null";

        // 2. Otherwise, let result be origin's scheme.
        StringBuilder result;
        result.append(scheme());

        // 3. Append "://" to result.
        result.append("://"sv);

        // 4. Append origin's host, serialized, to result.
        result.append(URL::Parser::serialize_host(host()).release_value_but_fixme_should_propagate_errors().to_byte_string());

        // 5. If origin's port is non-null, append a U+003A COLON character (:), and origin's port, serialized, to result.
        if (port() != 0) {
            result.append(':');
            result.append(ByteString::number(port()));
        }
        // 6. Return result
        return result.to_byte_string();
    }

    // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-effective-domain
    Optional<URL::Host> effective_domain() const
    {
        // 1. If origin is an opaque origin, then return null.
        if (is_opaque())
            return {};

        // FIXME: 2. If origin's domain is non-null, then return origin's domain.

        // 3. Return origin's host.
        return m_host;
    }

    bool operator==(Origin const& other) const { return is_same_origin(other); }

private:
    Optional<ByteString> m_scheme;
    URL::Host m_host;
    u16 m_port { 0 };
};

}

namespace AK {
template<>
struct Traits<Web::HTML::Origin> : public DefaultTraits<Web::HTML::Origin> {
    static unsigned hash(Web::HTML::Origin const& origin)
    {
        auto hash_without_host = pair_int_hash(origin.scheme().hash(), origin.port());
        if (origin.host().has<Empty>())
            return hash_without_host;
        return pair_int_hash(hash_without_host, URL::Parser::serialize_host(origin.host()).release_value_but_fixme_should_propagate_errors().hash());
    }
};
} // namespace AK

namespace IPC {
template<>
ErrorOr<void> encode(Encoder&, Web::HTML::Origin const&);

template<>
ErrorOr<Web::HTML::Origin> decode(Decoder&);
}
