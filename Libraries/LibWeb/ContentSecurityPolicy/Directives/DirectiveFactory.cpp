/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
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

namespace Web::ContentSecurityPolicy::Directives {

GC::Ref<Directive> create_directive(GC::Heap& heap, String name, Vector<String> value)
{
    if (name == Names::ConnectSrc)
        return heap.allocate<ConnectSourceDirective>(move(name), move(value));

    if (name == Names::FontSrc)
        return heap.allocate<FontSourceDirective>(move(name), move(value));

    if (name == Names::FrameSrc)
        return heap.allocate<FrameSourceDirective>(move(name), move(value));

    if (name == Names::ImgSrc)
        return heap.allocate<ImageSourceDirective>(move(name), move(value));

    if (name == Names::ManifestSrc)
        return heap.allocate<ManifestSourceDirective>(move(name), move(value));

    if (name == Names::MediaSrc)
        return heap.allocate<MediaSourceDirective>(move(name), move(value));

    return heap.allocate<Directive>(move(name), move(value));
}

}
