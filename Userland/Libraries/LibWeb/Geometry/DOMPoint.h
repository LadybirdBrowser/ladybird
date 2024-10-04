/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Geometry/DOMPointReadOnly.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#DOMPoint
class DOMPoint final : public DOMPointReadOnly {
    WEB_PLATFORM_OBJECT(DOMPoint, DOMPointReadOnly);
    JS_DECLARE_ALLOCATOR(DOMPoint);

public:
    static JS::NonnullGCPtr<DOMPoint> construct_impl(JS::Realm&, double x = 0, double y = 0, double z = 0, double w = 1);
    static JS::NonnullGCPtr<DOMPoint> create(JS::Realm&);

    static JS::NonnullGCPtr<DOMPoint> from_point(JS::VM&, DOMPointInit const&);

    virtual ~DOMPoint() override;

    double x() const { return m_x; }
    double y() const { return m_y; }
    double z() const { return m_z; }
    double w() const { return m_w; }

    void set_x(double x) { m_x = x; }
    void set_y(double y) { m_y = y; }
    void set_z(double z) { m_z = z; }
    void set_w(double w) { m_w = w; }

    virtual StringView interface_name() const override { return "DOMPoint"sv; }

private:
    DOMPoint(JS::Realm&, double x, double y, double z, double w);
    DOMPoint(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
