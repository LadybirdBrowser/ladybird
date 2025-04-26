/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>

namespace Web::ContentSecurityPolicy::Directives {

GC::Ref<Directive> create_directive(GC::Heap& heap, String name, Vector<String> value)
{
    dbgln("Potential FIXME: Creating unknown Content Security Policy directive: {}", name);
    return heap.allocate<Directive>(move(name), move(value));
}

}
