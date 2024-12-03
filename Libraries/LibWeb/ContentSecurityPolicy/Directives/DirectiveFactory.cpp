/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ConnectSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FontSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FrameSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ImageSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ManifestSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/MediaSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ObjectSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ScriptSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ScriptSourceElementDirective.h>

namespace Web::ContentSecurityPolicy::Directives {

GC::Ref<Directive> create_directive(JS::Realm& realm, String name, Vector<String> value)
{
    if (name == Names::ConnectSrc)
        return realm.create<ConnectSourceDirective>(move(name), move(value));

    if (name == Names::FontSrc)
        return realm.create<FontSourceDirective>(move(name), move(value));

    if (name == Names::FrameSrc)
        return realm.create<FrameSourceDirective>(move(name), move(value));

    if (name == Names::ImgSrc)
        return realm.create<ImageSourceDirective>(move(name), move(value));

    if (name == Names::ManifestSrc)
        return realm.create<ManifestSourceDirective>(move(name), move(value));

    if (name == Names::MediaSrc)
        return realm.create<MediaSourceDirective>(move(name), move(value));

    if (name == Names::ObjectSrc)
        return realm.create<ObjectSourceDirective>(move(name), move(value));

    if (name == Names::ScriptSrc)
        return realm.create<ScriptSourceDirective>(move(name), move(value));

    if (name == Names::ScriptSrcElem)
        return realm.create<ScriptSourceElementDirective>(move(name), move(value));

    dbgln("Potential FIXME: Creating unknown Content Security Policy directive: {}", name);
    return realm.create<Directive>(move(name), move(value));
}

}
