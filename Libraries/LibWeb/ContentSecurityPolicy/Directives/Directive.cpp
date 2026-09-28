/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWebCommon/ContentSecurityPolicy/Directives/SerializedDirective.h>

namespace Web::ContentSecurityPolicy::Directives {

static Directive::Kind kind_for_name(Utf16FlyString const& name)
{
#define __ENUMERATE_DIRECTIVE_NAME(directive_name, value) \
    if (name == Names::directive_name)                    \
        return Directive::Kind::directive_name;
    ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME
    return Directive::Kind::Unrecognized;
}

Directive Directive::create(Utf16FlyString name, Vector<Utf16String> value)
{
    auto kind = kind_for_name(name);
    return Directive { kind, move(name), move(value) };
}

Directive::Directive(Kind kind, Utf16FlyString name, Vector<Utf16String> value)
    : m_kind(kind)
    , m_name(move(name))
    , m_value(move(value))
{
}

SerializedDirective Directive::serialize() const
{
    return SerializedDirective {
        .name = m_name,
        .value = m_value,
    };
}

}
