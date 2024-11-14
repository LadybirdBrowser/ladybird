/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGfx/DeprecatedPath.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>
#include <LibWeb/HTML/Canvas/CanvasPath.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#path2d
class Path2D final
    : public Bindings::PlatformObject
    , public CanvasPath {

    WEB_PLATFORM_OBJECT(Path2D, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Path2D);

public:
    static WebIDL::ExceptionOr<GC::Ref<Path2D>> construct_impl(JS::Realm&, Optional<Variant<GC::Root<Path2D>, String>> const& path);

    virtual ~Path2D() override;

    WebIDL::ExceptionOr<void> add_path(GC::Ref<Path2D> path, Geometry::DOMMatrix2DInit& transform);

private:
    Path2D(JS::Realm&, Optional<Variant<GC::Root<Path2D>, String>> const&);

    virtual void initialize(JS::Realm&) override;
};

}
