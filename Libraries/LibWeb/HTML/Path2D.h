/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/Canvas/CanvasPath.h>

namespace Web::Bindings {

struct DOMMatrix2DInit;

}

namespace Web::Geometry {

class DOMMatrix;

}

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#path2d
class Path2D final
    : public Bindings::Wrappable
    , public CanvasPath {

    WEB_WRAPPABLE(Path2D, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Path2D);

public:
    static GC::Ref<Path2D> create(Optional<Variant<GC::Ref<Path2D>, String>> const& path);

    virtual ~Path2D() override;

    WebIDL::ExceptionOr<void> add_path(GC::Ref<Path2D> path, GC::Ref<Geometry::DOMMatrix> transform);
    WebIDL::ExceptionOr<void> add_path(GC::Ref<Path2D> path, Bindings::DOMMatrix2DInit const& transform);

private:
    explicit Path2D(Optional<Variant<GC::Ref<Path2D>, String>> const&);
};

}
