/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMatrixComponent.h"
#include <LibWeb/Bindings/CSSMatrixComponentPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMatrixComponent);

GC::Ref<CSSMatrixComponent> CSSMatrixComponent::create(JS::Realm& realm, Is2D is_2d, GC::Ref<Geometry::DOMMatrix> matrix)
{
    return realm.create<CSSMatrixComponent>(realm, is_2d, matrix);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmatrixcomponent-cssmatrixcomponent
WebIDL::ExceptionOr<GC::Ref<CSSMatrixComponent>> CSSMatrixComponent::construct_impl(JS::Realm& realm, GC::Ref<Geometry::DOMMatrixReadOnly> matrix, Optional<CSSMatrixComponentOptions> options)
{
    // The CSSMatrixComponent(matrix, options) constructor must, when invoked, perform the following steps:

    // 1. Let this be a new CSSMatrixComponent object with its matrix internal slot set to matrix.
    // NB: Done below.

    // 2. If options was passed and has a is2D field, set this’s is2D internal slot to the value of that field.
    // 3. Otherwise, set this’s is2D internal slot to the value of matrix’s is2D internal slot.
    auto is_2d = matrix->is2d() ? Is2D::Yes : Is2D::No;
    if (options.has_value() && options->is2d.has_value())
        is_2d = options->is2d.value() ? Is2D::Yes : Is2D::No;

    auto this_ = CSSMatrixComponent::create(realm, is_2d, Geometry::DOMMatrix::create_from_dom_matrix_read_only(realm, matrix));

    // 4. Return this.
    return this_;
}

CSSMatrixComponent::CSSMatrixComponent(JS::Realm& realm, Is2D is_2d, GC::Ref<Geometry::DOMMatrix> matrix)
    : CSSTransformComponent(realm, is_2d)
    , m_matrix(matrix)
{
}

CSSMatrixComponent::~CSSMatrixComponent() = default;

void CSSMatrixComponent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMatrixComponent);
    Base::initialize(realm);
}

void CSSMatrixComponent::visit_edges(Visitor& visitor)
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

}
