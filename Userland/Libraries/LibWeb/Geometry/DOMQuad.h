/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dictdef-domquadinit
struct DOMQuadInit {
    DOMPointInit p1;
    DOMPointInit p2;
    DOMPointInit p3;
    DOMPointInit p4;
};

// https://drafts.fxtf.org/geometry/#domquad
class DOMQuad
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(DOMQuad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMQuad);

public:
    static GC::Ref<DOMQuad> construct_impl(JS::Realm&, DOMPointInit const& p1, DOMPointInit const& p2, DOMPointInit const& p3, DOMPointInit const& p4);
    static GC::Ref<DOMQuad> create(JS::Realm& realm);

    virtual ~DOMQuad() override;

    static GC::Ref<DOMQuad> from_rect(JS::VM&, DOMRectInit const&);
    static GC::Ref<DOMQuad> from_quad(JS::VM&, DOMQuadInit const&);

    GC::Ref<DOMPoint> p1() const { return m_p1; }
    GC::Ref<DOMPoint> p2() const { return m_p2; }
    GC::Ref<DOMPoint> p3() const { return m_p3; }
    GC::Ref<DOMPoint> p4() const { return m_p4; }

    GC::Ref<DOMRect> get_bounds() const;

    virtual StringView interface_name() const override { return "DOMQuad"sv; }
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::SerializationRecord&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(ReadonlySpan<u32> const&, size_t& position, HTML::DeserializationMemory&) override;

private:
    DOMQuad(JS::Realm&, DOMPointInit const& p1, DOMPointInit const& p2, DOMPointInit const& p3, DOMPointInit const& p4);
    explicit DOMQuad(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOMPoint> m_p1;
    GC::Ref<DOMPoint> m_p2;
    GC::Ref<DOMPoint> m_p3;
    GC::Ref<DOMPoint> m_p4;
};

}
