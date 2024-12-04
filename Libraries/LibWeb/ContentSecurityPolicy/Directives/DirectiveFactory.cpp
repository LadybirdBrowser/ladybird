/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/Directives/BaseUriDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ChildSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ConnectSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DefaultSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FontSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FormActionDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FrameAncestorsDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/FrameSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ImageSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ManifestSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/MediaSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ObjectSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ReportToDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ReportUriDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ScriptSourceAttributeDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ScriptSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/ScriptSourceElementDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/StyleSourceAttributeDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/StyleSourceDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/StyleSourceElementDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/WebRTCDirective.h>
#include <LibWeb/ContentSecurityPolicy/Directives/WorkerSourceDirective.h>

namespace Web::ContentSecurityPolicy::Directives {

GC::Ref<Directive> create_directive(JS::Realm& realm, String name, Vector<String> value)
{
    if (name == Names::BaseUri)
        return realm.create<BaseUriDirective>(move(name), move(value));

    if (name == Names::ChildSrc)
        return realm.create<ChildSourceDirective>(move(name), move(value));

    if (name == Names::ConnectSrc)
        return realm.create<ConnectSourceDirective>(move(name), move(value));

    if (name == Names::DefaultSrc)
        return realm.create<DefaultSourceDirective>(move(name), move(value));

    if (name == Names::FontSrc)
        return realm.create<FontSourceDirective>(move(name), move(value));

    if (name == Names::FormAction)
        return realm.create<FormActionDirective>(move(name), move(value));

    if (name == Names::FrameAncestors)
        return realm.create<FrameAncestorsDirective>(move(name), move(value));

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

    if (name == Names::ReportTo)
        return realm.create<ReportToDirective>(move(name), move(value));

    if (name == Names::ReportUri)
        return realm.create<ReportUriDirective>(move(name), move(value));

    if (name == Names::ScriptSrcAttr)
        return realm.create<ScriptSourceAttributeDirective>(move(name), move(value));

    if (name == Names::ScriptSrc)
        return realm.create<ScriptSourceDirective>(move(name), move(value));

    if (name == Names::ScriptSrcElem)
        return realm.create<ScriptSourceElementDirective>(move(name), move(value));

    if (name == Names::StyleSrcAttr)
        return realm.create<StyleSourceAttributeDirective>(move(name), move(value));

    if (name == Names::StyleSrc)
        return realm.create<StyleSourceDirective>(move(name), move(value));

    if (name == Names::StyleSrcElem)
        return realm.create<StyleSourceElementDirective>(move(name), move(value));

    if (name == Names::WebRTC)
        return realm.create<WebRTCDirective>(move(name), move(value));

    if (name == Names::WorkerSrc)
        return realm.create<WorkerSourceDirective>(move(name), move(value));

    dbgln("Potential FIXME: Creating unknown Content Security Policy directive: {}", name);
    return realm.create<Directive>(move(name), move(value));
}

}
