/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <LibGfx/AffineTransform.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct DOMMatrix2DInit;

}

namespace Web::SVG {

// https://svgwg.org/svg2-draft/coords.html#InterfaceSVGTransform
class SVGTransform final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGTransform, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGTransform);

public:
    enum class Type : u16 {
        Unknown = 0,
        Matrix = 1,
        Translate = 2,
        Scale = 3,
        Rotate = 4,
        SkewX = 5,
        SkewY = 6,
    };

    enum class ReadOnly : u8 {
        Yes,
        No,
    };

    // A <transform-function> value: its type, the angle argument of a rotate(), skewX() or skewY() function, and its
    // matrix as the elements m11, m12, m13, m14, m21, ..., m44, in that order.
    struct Value {
        Type type { Type::Matrix };
        float angle { 0 };
        Array<double, 16> elements { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        bool is_2d { true };

        static Value from_transform(Transform const&);
        static Value from_matrix(Array<double, 16> const& elements, bool is_2d);
        Gfx::AffineTransform affine_transform() const;

        bool operator==(Value const&) const = default;
    };

    // A new, detached SVGTransform object whose value is matrix(1, 0, 0, 1, 0, 0).
    [[nodiscard]] static GC::Ref<SVGTransform> create();
    [[nodiscard]] static GC::Ref<SVGTransform> create(Value);
    virtual ~SVGTransform() override;

    Type type() const;
    GC::Ref<Geometry::DOMMatrix> matrix();
    float angle() const;

    WebIDL::ExceptionOr<void> set_matrix(Bindings::DOMMatrix2DInit const&);
    WebIDL::ExceptionOr<void> set_translate(float tx, float ty);
    WebIDL::ExceptionOr<void> set_scale(float sx, float sy);
    WebIDL::ExceptionOr<void> set_rotate(float angle, float cx, float cy);
    WebIDL::ExceptionOr<void> set_skew_x(float angle);
    WebIDL::ExceptionOr<void> set_skew_y(float angle);

    Value value() const;

    // https://svgwg.org/svg2-draft/types.html#TermAttach
    void attach(GC::Ref<SVGTransformList>, ReadOnly);

    // https://svgwg.org/svg2-draft/types.html#TermDetach
    void detach();

    bool is_detached() const { return !m_list; }

    // Sets the value when the list this transform belongs to is synchronized with its reflected attribute.
    void set_value_from_reflected_attribute(Value);

private:
    explicit SVGTransform(Value);

    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> set_value_and_reserialize(Value);
    void set_value(Value);

    mutable Value m_value;

    // https://svgwg.org/svg2-draft/coords.html#TransformMatrixObject
    GC::Ptr<Geometry::DOMMatrix> m_matrix;

    // The list this transform is an element of, if it reflects an element of a presentation attribute value.
    GC::Ptr<SVGTransformList> m_list;

    ReadOnly m_read_only { ReadOnly::No };
};

}
