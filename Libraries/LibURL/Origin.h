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

// https://html.spec.whatwg.org/multipage/browsers.html#concept-origin
class Origin {
public:
    struct OpaqueData {
        enum class Type : u8 {
            Standard,
            File
        };
        using Nonce = Array<u8, 16>;

        Nonce nonce;
        Type type;
    };

    explicit Origin(OpaqueData opaque_data)
        : m_state(opaque_data)
    {
    }

    static Origin create_opaque(OpaqueData::Type = OpaqueData::Type::Standard);

    Origin(Optional<String> const& scheme, Host const&, Optional<u16> port, Optional<String> domain = {});

    // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-opaque
    bool is_opaque() const { return m_state.has<OpaqueData>(); }

    Optional<String> const& scheme() const { return m_state.get<Tuple>().scheme; }
    Host const& host() const { return m_state.get<Tuple>().host; }
    Optional<u16> port() const { return m_state.get<Tuple>().port; }
    Optional<String> domain() const { return m_state.get<Tuple>().domain; }

    OpaqueData const& opaque_data() const { return m_state.get<OpaqueData>(); }

    // https://html.spec.whatwg.org/multipage/origin.html#same-origin
    bool is_same_origin(Origin const& other) const;

    // https://html.spec.whatwg.org/multipage/origin.html#same-origin-domain
    bool is_same_origin_domain(Origin const& other) const;

    // https://html.spec.whatwg.org/multipage/browsers.html#same-site
    bool is_same_site(Origin const&) const;

    // https://html.spec.whatwg.org/multipage/origin.html#ascii-serialisation-of-an-origin
    String serialize() const;

    // https://html.spec.whatwg.org/multipage/origin.html#concept-origin-effective-domain
    Optional<Host> effective_domain() const;

    bool operator==(Origin const& other) const { return is_same_origin(other); }

    bool is_opaque_file_origin() const { return is_opaque() && opaque_data().type == OpaqueData::Type::File; }

private:
    struct Tuple {
        Optional<String> scheme;
        Host host;
        Optional<u16> port;
        Optional<String> domain;
    };

    Variant<Tuple, OpaqueData> m_state;
};

}

namespace AK {

template<>
struct Traits<URL::Origin> : public DefaultTraits<URL::Origin> {
    static unsigned hash(URL::Origin const& origin);
};

} // namespace AK
