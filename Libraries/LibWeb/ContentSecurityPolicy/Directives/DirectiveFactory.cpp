/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ConnectSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>

namespace Web::ContentSecurityPolicy::Directives {

GC::Ref<Directive> create_directive(JS::Realm& realm, String name, Vector<String> value)
{
    if (name == Names::ConnectSrc)
        return realm.create<ConnectSourceDirective>(move(name), move(value));

    dbgln("Potential FIXME: Creating unknown Content Security Policy directive: {}", name);
    return realm.create<Directive>(move(name), move(value));
}

}
