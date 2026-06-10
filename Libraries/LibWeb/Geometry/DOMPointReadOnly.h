/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct DOMMatrixInit;
struct DOMPointInit;

}

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dompointreadonly
class DOMPointReadOnly
    : public Bindings::Wrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(DOMPointReadOnly, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMPointReadOnly);

public:
    static GC::Ref<DOMPointReadOnly> create(double x, double y, double z, double w);
    static GC::Ref<DOMPointReadOnly> create();
    static GC::Ref<DOMPointReadOnly> dom_point_read_only_from_point(Bindings::DOMPointInit const&);

    virtual ~DOMPointReadOnly() override;

    double x() const { return m_x; }
    double y() const { return m_y; }
    double z() const { return m_z; }
    double w() const { return m_w; }

    WebIDL::ExceptionOr<GC::Ref<DOMPoint>> matrix_transform(GC::Ref<DOMMatrix>) const;
    WebIDL::ExceptionOr<GC::Ref<DOMPoint>> matrix_transform(Bindings::DOMMatrixInit const&) const;

    virtual WebIDL::ExceptionOr<void> serialization_steps(JS::Realm&, HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

protected:
    DOMPointReadOnly(double x, double y, double z, double w);
    DOMPointReadOnly();

    double m_x;
    double m_y;
    double m_z;
    double m_w;
};

}
