/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>

namespace Web::ContentSecurityPolicy::Directives {

static bool is_known_directive(String const& name)
{
#define __ENUMERATE_DIRECTIVE_NAME(enum_name, string_value) \
    if (name == Names::enum_name) {                        \
        return true;                                       \
    }
    ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME
    return false;
}

GC::Ref<Directive> create_directive(GC::Heap& heap, String name, Vector<String> value)
{
    if (!is_known_directive(name))
        dbgln("Potential FIXME: Creating unknown Content Security Policy directive: {}", name);

    return heap.allocate<Directive>(move(name), move(value));
}

}
