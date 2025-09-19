/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSTransformComponent.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#dictdef-cssmatrixcomponentoptions
struct CSSMatrixComponentOptions {
    Optional<bool> is2d;
};

// https://drafts.css-houdini.org/css-typed-om-1/#cssmatrixcomponent
class CSSMatrixComponent final : public CSSTransformComponent {
    WEB_PLATFORM_OBJECT(CSSMatrixComponent, CSSTransformComponent);
    GC_DECLARE_ALLOCATOR(CSSMatrixComponent);

public:
    [[nodiscard]] static GC::Ref<CSSMatrixComponent> create(JS::Realm&, Is2D, GC::Ref<Geometry::DOMMatrix>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMatrixComponent>> construct_impl(JS::Realm&, GC::Ref<Geometry::DOMMatrixReadOnly>, Optional<CSSMatrixComponentOptions> = {});

    virtual ~CSSMatrixComponent() override;

    virtual WebIDL::ExceptionOr<Utf16String> to_string() const override;

    virtual WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> to_matrix() const override;

    GC::Ref<Geometry::DOMMatrix> matrix() const { return m_matrix; }
    WebIDL::ExceptionOr<void> set_matrix(GC::Ref<Geometry::DOMMatrix> matrix);

private:
    explicit CSSMatrixComponent(JS::Realm&, Is2D, GC::Ref<Geometry::DOMMatrix>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<Geometry::DOMMatrix> m_matrix;
};

}
