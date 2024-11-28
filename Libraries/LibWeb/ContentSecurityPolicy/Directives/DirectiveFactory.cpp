/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ConnectSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>

namespace Web::ContentSecurityPolicy::Directives {

GC::Ref<Directive> create_directive(GC::Heap& heap, String name, Vector<String> value)
{
    if (name == Names::ConnectSrc)
        return heap.allocate<ConnectSourceDirective>(move(name), move(value));

    return heap.allocate<Directive>(move(name), move(value));
}

}
