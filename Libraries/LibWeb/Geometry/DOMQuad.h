/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct DOMPointInit;
struct DOMQuadInit;
struct DOMRectInit;

}

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#domquad
class DOMQuad
    : public Bindings::Wrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(DOMQuad, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMQuad);

public:
    static GC::Ref<DOMQuad> create(GC::Ref<DOMPoint> p1, GC::Ref<DOMPoint> p2, GC::Ref<DOMPoint> p3, GC::Ref<DOMPoint> p4);
    static GC::Ref<DOMQuad> create(Bindings::DOMPointInit const& p1, Bindings::DOMPointInit const& p2, Bindings::DOMPointInit const& p3, Bindings::DOMPointInit const& p4);
    static GC::Ref<DOMQuad> create();
    static GC::Ref<DOMQuad> dom_quad_from_rect(Bindings::DOMRectInit const&);
    static GC::Ref<DOMQuad> dom_quad_from_quad(Bindings::DOMQuadInit const&);

    virtual ~DOMQuad() override;

    GC::Ref<DOMPoint> p1() const { return m_p1; }
    GC::Ref<DOMPoint> p2() const { return m_p2; }
    GC::Ref<DOMPoint> p3() const { return m_p3; }
    GC::Ref<DOMPoint> p4() const { return m_p4; }

    GC::Ref<DOMRect> get_bounds() const;

    virtual WebIDL::ExceptionOr<void> serialization_steps(JS::Realm&, HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

private:
    DOMQuad(GC::Ref<DOMPoint> p1, GC::Ref<DOMPoint> p2, GC::Ref<DOMPoint> p3, GC::Ref<DOMPoint> p4);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<DOMPoint> m_p1;
    GC::Ref<DOMPoint> m_p2;
    GC::Ref<DOMPoint> m_p3;
    GC::Ref<DOMPoint> m_p4;
};

}
