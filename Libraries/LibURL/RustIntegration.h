/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <LibURL/Parser.h>
#include <LibURL/RustFFI.h>
#include <LibURL/URL.h>

namespace URL::RustIntegration {

Optional<URL> parse_basic_url(StringView input, Optional<URL const&> base_url = {}, URL* url = nullptr, Optional<Parser::State> state_override = {}, Optional<StringView> encoding = {});
Optional<Host> parse_host(StringView input, bool is_opaque = false);

class URLPattern {
public:
    // NOTE: All exceptions which are thrown by the URLPattern spec are TypeErrors which web-based callers are expected to assume.
    //       If this ever does not become the case, this should change to also include the error type.
    struct ErrorInfo {
        String message;
    };

    template<typename ValueT>
    using ErrorOr = AK::ErrorOr<ValueT, ErrorInfo>;

    struct Component {
        String pattern_string;
        struct Result {
            String input;
            OrderedHashMap<String, Variant<String, Empty>> groups;
        };
    };

    struct Init {
        Optional<String> protocol;
        Optional<String> username;
        Optional<String> password;
        Optional<String> hostname;
        Optional<String> port;
        Optional<String> pathname;
        Optional<String> search;
        Optional<String> hash;
        Optional<String> base_url;
    };

    using Input = Variant<String, Init>;

    struct Result {
        Vector<Input> inputs;

        Component::Result protocol;
        Component::Result username;
        Component::Result password;
        Component::Result hostname;
        Component::Result port;
        Component::Result pathname;
        Component::Result search;
        Component::Result hash;
    };

    static ErrorOr<URLPattern> create(Input const&, Optional<String> const& base_url = {}, FFI::IgnoreCase = FFI::IgnoreCase::No);

    ErrorOr<Optional<Result>> match(Input const&, Optional<String> const& base_url_string) const;

    bool has_regexp_groups() const;

    Component const& protocol_component() const { return m_protocol_component; }
    Component const& username_component() const { return m_username_component; }
    Component const& password_component() const { return m_password_component; }
    Component const& hostname_component() const { return m_hostname_component; }
    Component const& port_component() const { return m_port_component; }
    Component const& pathname_component() const { return m_pathname_component; }
    Component const& search_component() const { return m_search_component; }
    Component const& hash_component() const { return m_hash_component; }

private:
    struct Impl {
        FFI::RustUrlPattern* rust_url_pattern { nullptr };
        ~Impl();
    };
    OwnPtr<Impl> m_impl;
    Component m_protocol_component;
    Component m_username_component;
    Component m_password_component;
    Component m_hostname_component;
    Component m_port_component;
    Component m_pathname_component;
    Component m_search_component;
    Component m_hash_component;
    bool m_has_regexp_groups { false };
};

}
