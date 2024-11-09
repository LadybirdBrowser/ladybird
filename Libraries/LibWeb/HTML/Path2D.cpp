/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Path2DPrototype.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/HTML/Path2D.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGPathElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(Path2D);

WebIDL::ExceptionOr<JS::NonnullGCPtr<Path2D>> Path2D::construct_impl(JS::Realm& realm, Optional<Variant<JS::Handle<Path2D>, String>> const& path)
{
    return realm.heap().allocate<Path2D>(realm, realm, path);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-path2d
Path2D::Path2D(JS::Realm& realm, Optional<Variant<JS::Handle<Path2D>, String>> const& path)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this))
{
    // 1. Let output be a new Path2D object.
    // 2. If path is not given, then return output.
    if (!path.has_value())
        return;

    // 3. If path is a Path2D object, then add all subpaths of path to output and return output.
    //    (In other words, it returns a copy of the argument.)
    if (path->has<JS::Handle<Path2D>>()) {
        this->path() = path->get<JS::Handle<Path2D>>()->path();
        return;
    }

    // 4. Let svgPath be the result of parsing and interpreting path according to SVG 2's rules for path data. [SVG]
    auto path_instructions = SVG::AttributeParser::parse_path_data(path->get<String>());
    auto svg_path = SVG::path_from_path_instructions(path_instructions);

    if (!svg_path.is_empty()) {
        // 5. Let (x, y) be the last point in svgPath.
        auto xy = svg_path.last_point();

        // 6. Add all the subpaths, if any, from svgPath to output.
        this->path() = move(svg_path);

        // 7. Create a new subpath in output with (x, y) as the only point in the subpath.
        this->move_to(xy.x(), xy.y());
    }

    // 8. Return output.
}

Path2D::~Path2D() = default;

void Path2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::Path2DPrototype>(realm, "Path2D"_fly_string));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-path2d-addpath
WebIDL::ExceptionOr<void> Path2D::add_path(JS::NonnullGCPtr<Path2D> path, Geometry::DOMMatrix2DInit& transform)
{
    // The addPath(path, transform) method, when invoked on a Path2D object a, must run these steps:

    // 1. If the Path2D object path has no subpaths, then return.
    if (path->path().is_empty())
        return {};

    // 2. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
    auto matrix = TRY(Geometry::DOMMatrix::create_from_dom_matrix_2d_init(realm(), transform));

    // 3. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42 element are infinite or NaN, then return.
    if (!isfinite(matrix->m11()) || !isfinite(matrix->m12()) || !isfinite(matrix->m21()) || !isfinite(matrix->m22()) || !isfinite(matrix->m41()) || !isfinite(matrix->m42()))
        return {};

    // 4. Create a copy of all the subpaths in path. Let this copy be known as c.
    // 5. Transform all the coordinates and lines in c by the transform matrix matrix.
    auto copy = path->path().copy_transformed(Gfx::AffineTransform { static_cast<float>(matrix->m11()), static_cast<float>(matrix->m12()), static_cast<float>(matrix->m21()), static_cast<float>(matrix->m22()), static_cast<float>(matrix->m41()), static_cast<float>(matrix->m42()) });

    // 6. Let (x, y) be the last point in the last subpath of c.
    auto xy = copy.last_point();

    // 7. Add all the subpaths in c to a.
    // FIXME: Is this correct?
    this->path().append_path(copy);

    // 8. Create a new subpath in a with (x, y) as the only point in the subpath.
    this->move_to(xy.x(), xy.y());

    return {};
}

}
