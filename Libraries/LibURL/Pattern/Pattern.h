/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/Pattern/Component.h>
#include <LibURL/Pattern/Init.h>
#include <LibURL/Pattern/PatternError.h>
#include <LibURL/URL.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#typedefdef-urlpatterninput
using Input = Variant<String, Init>;

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternresult
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

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternoptions
enum class IgnoreCase {
    Yes,
    No,
};

// https://urlpattern.spec.whatwg.org/#url-pattern
class Pattern {
public:
    static PatternErrorOr<Pattern> create(Input const&, Optional<String> const& base_url = {}, IgnoreCase = IgnoreCase::No);

    PatternErrorOr<Optional<Result>> match(Variant<String, Init, URL> const&, Optional<String> const& base_url_string) const;

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
    // https://urlpattern.spec.whatwg.org/#url-pattern-protocol-component
    // protocol component, a component
    Component m_protocol_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-username-component
    // username component, a component
    Component m_username_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-password-component
    // password component, a component
    Component m_password_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-hostname-component
    // hostname component, a component
    Component m_hostname_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-port-component
    // port component, a component
    Component m_port_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-pathname-component
    // pathname component, a component
    Component m_pathname_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-search-component
    // search component, a component
    Component m_search_component;

    // https://urlpattern.spec.whatwg.org/#url-pattern-hash-component
    // hash component, a component
    Component m_hash_component;
};

}
