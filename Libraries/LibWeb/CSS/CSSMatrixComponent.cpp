/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMatrixComponent.h"
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/CSSMatrixComponent.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMatrixComponent);

GC::Ref<CSSMatrixComponent> CSSMatrixComponent::create(Is2D is_2d, GC::Ref<Geometry::DOMMatrix> matrix)
{
    return GC::Heap::the().allocate<CSSMatrixComponent>(is_2d, matrix);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmatrixcomponent-cssmatrixcomponent
WebIDL::ExceptionOr<GC::Ref<CSSMatrixComponent>> CSSMatrixComponent::create_from_dom_matrix_read_only(GC::Ref<Geometry::DOMMatrixReadOnly> matrix, Optional<bool> is_2d_option)
{
    // The CSSMatrixComponent(matrix, options) constructor must, when invoked, perform the following steps:

    // 1. Let this be a new CSSMatrixComponent object with its matrix internal slot set to matrix.
    // NB: Done below.

    // 2. If options was passed and has a is2D field, set this’s is2D internal slot to the value of that field.
    // 3. Otherwise, set this’s is2D internal slot to the value of matrix’s is2D internal slot.
    auto is_2d = matrix->is2d() ? Is2D::Yes : Is2D::No;
    if (is_2d_option.has_value())
        is_2d = is_2d_option.value() ? Is2D::Yes : Is2D::No;

    auto this_ = CSSMatrixComponent::create(is_2d, Geometry::DOMMatrix::create_from_dom_matrix_read_only(matrix));

    // 4. Return this.
    return this_;
}

WebIDL::ExceptionOr<GC::Ref<CSSMatrixComponent>> CSSMatrixComponent::create_for_constructor(GC::Ref<Geometry::DOMMatrixReadOnly> matrix, Bindings::CSSMatrixComponentOptions const& options)
{
    return create_from_dom_matrix_read_only(matrix, options.is2d);
}

CSSMatrixComponent::CSSMatrixComponent(Is2D is_2d, GC::Ref<Geometry::DOMMatrix> matrix)
    : CSSTransformComponent(is_2d)
    , m_matrix(matrix)
{
}

CSSMatrixComponent::~CSSMatrixComponent() = default;

void CSSMatrixComponent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_matrix);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmatrixcomponent
WebIDL::ExceptionOr<Utf16String> CSSMatrixComponent::to_string() const
{
    // 1. Return the serialization of this’s matrix internal slot.
    // FIXME: This means we ignore our is_2d state. https://github.com/w3c/css-houdini-drafts/issues/1155
    return m_matrix->to_string();
}

WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSMatrixComponent::to_matrix() const
{
    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
    // 1. Let matrix be a new DOMMatrix object, initialized to this’s equivalent 4x4 transform matrix, as defined in
    //    CSS Transforms 1 § 12. Mathematical Description of Transform Functions, and with its is2D internal slot set to
    //    the same value as this’s is2D internal slot.
    // NB: The elements are m11, m12, m13, m14, m21, ..., m44. A 2D component only uses m11, m12, m21, m22, m41 and m42.
    auto elements = m_matrix->elements();
    if (is_2d()) {
        elements = { elements[0], elements[1], 0, 0,
            elements[4], elements[5], 0, 0,
            0, 0, 1, 0,
            elements[12], elements[13], 0, 1 };
    }
    auto matrix = Geometry::DOMMatrix::create();
    matrix->set_elements(elements, is_2d());

    // 2. Return matrix.
    return matrix;
}

WebIDL::ExceptionOr<void> CSSMatrixComponent::set_matrix(GC::Ref<Geometry::DOMMatrix> matrix)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    // FIXME: Should this modify is_2d? Or should we modify the matrix's is_2d?
    m_matrix = matrix;
    return {};
}

WebIDL::ExceptionOr<NonnullRefPtr<TransformationStyleValue const>> CSSMatrixComponent::create_style_value(PropertyNameAndID const& property) const
{
    // NB: The elements are m11, m12, m13, m14, m21, ..., m44, and the elements a to f of a 2D matrix are m11, m12, m21,
    //     m22, m41 and m42.
    auto elements = m_matrix->elements();
    StyleValueVector values;
    if (is_2d()) {
        for (auto index : { 0, 1, 4, 5, 12, 13 })
            values.append(NumberStyleValue::create(elements[index]));
        return TransformationStyleValue::create(property.id(), TransformFunction::Matrix, move(values));
    }

    for (auto element : elements)
        values.append(NumberStyleValue::create(element));
    return TransformationStyleValue::create(property.id(), TransformFunction::Matrix3d, move(values));
}

}
