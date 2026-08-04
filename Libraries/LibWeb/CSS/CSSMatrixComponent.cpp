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
    return Utf16String::from_utf8(TRY(m_matrix->to_string()));
}

WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSMatrixComponent::to_matrix() const
{
    // AD-HOC: Not specced, but we already have a matrix so use that.
    //          https://github.com/w3c/css-houdini-drafts/issues/1153
    return matrix();
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
    if (is_2d()) {
        return TransformationStyleValue::create(property.id(), TransformFunction::Matrix,
            {
                NumberStyleValue::create(m_matrix->a()),
                NumberStyleValue::create(m_matrix->b()),
                NumberStyleValue::create(m_matrix->c()),
                NumberStyleValue::create(m_matrix->d()),
                NumberStyleValue::create(m_matrix->e()),
                NumberStyleValue::create(m_matrix->f()),
            });
    }

    return TransformationStyleValue::create(property.id(), TransformFunction::Matrix3d,
        {
            NumberStyleValue::create(m_matrix->m11()),
            NumberStyleValue::create(m_matrix->m12()),
            NumberStyleValue::create(m_matrix->m13()),
            NumberStyleValue::create(m_matrix->m14()),
            NumberStyleValue::create(m_matrix->m21()),
            NumberStyleValue::create(m_matrix->m22()),
            NumberStyleValue::create(m_matrix->m23()),
            NumberStyleValue::create(m_matrix->m24()),
            NumberStyleValue::create(m_matrix->m31()),
            NumberStyleValue::create(m_matrix->m32()),
            NumberStyleValue::create(m_matrix->m33()),
            NumberStyleValue::create(m_matrix->m34()),
            NumberStyleValue::create(m_matrix->m41()),
            NumberStyleValue::create(m_matrix->m42()),
            NumberStyleValue::create(m_matrix->m43()),
            NumberStyleValue::create(m_matrix->m44()),
        });
}

}
