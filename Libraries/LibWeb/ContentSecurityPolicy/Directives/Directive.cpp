/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SerializedDirective.h>

namespace Web::ContentSecurityPolicy::Directives {

GC_DEFINE_ALLOCATOR(Directive);

Directive::Directive(String name, Vector<String> value)
    : m_name(move(name))
    , m_value(move(value))
{
}

GC::Ref<Directive> Directive::clone(JS::Realm& realm) const
{
    return create_directive(realm, m_name, m_value);
}

SerializedDirective Directive::serialize() const
{
    return SerializedDirective {
        .name = m_name,
        .value = m_value,
    };
}

}
