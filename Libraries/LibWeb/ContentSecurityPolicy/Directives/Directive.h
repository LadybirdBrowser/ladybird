/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/ContentSecurityPolicy/Directives/NavigationType.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directives
// Each policy contains an ordered set of directives (its directive set), each of which controls a specific behavior.
// The directives defined in this document are described in detail in § 6 Content Security Policy Directives.
class Directive {
public:
    using NavigationType = Directives::NavigationType;

    enum class Kind : u8 {
#define __ENUMERATE_DIRECTIVE_NAME(name, value) name,
        ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME
            Unrecognized,
    };

    enum class [[nodiscard]] Result {
        Blocked,
        Allowed,
    };

    enum class CheckType {
        Source,
        Response,
    };

    enum class InlineType {
        Navigation,
        Script,
        ScriptAttribute,
        Style,
        StyleAttribute,
    };

    [[nodiscard]] static Directive create(Utf16FlyString name, Vector<Utf16String> value);

    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] Utf16FlyString const& name() const { return m_name; }
    [[nodiscard]] Vector<Utf16String> const& value() const { return m_value; }

    [[nodiscard]] SerializedDirective serialize() const;

private:
    Directive(Kind, Utf16FlyString name, Vector<Utf16String> value);

    Kind m_kind { Kind::Unrecognized };

    // https://w3c.github.io/webappsec-csp/#directive-name
    // https://w3c.github.io/webappsec-csp/#directive-value
    // Each directive is a name / value pair. The name is a non-empty string, and the value is a set of non-empty strings.
    // The value MAY be empty.
    Utf16FlyString m_name;
    Vector<Utf16String> m_value;
};

}
