/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Matrix4x4.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dictdef-dommatrix2dinit
struct DOMMatrix2DInit {
    Optional<double> a;
    Optional<double> b;
    Optional<double> c;
    Optional<double> d;
    Optional<double> e;
    Optional<double> f;
    Optional<double> m11;
    Optional<double> m12;
    Optional<double> m21;
    Optional<double> m22;
    Optional<double> m41;
    Optional<double> m42;
};

// https://drafts.fxtf.org/geometry/#dictdef-dommatrixinit
struct DOMMatrixInit : public DOMMatrix2DInit {
    double m13 { 0.0 };
    double m14 { 0.0 };
    double m23 { 0.0 };
    double m24 { 0.0 };
    double m31 { 0.0 };
    double m32 { 0.0 };
    double m33 { 0.0 };
    double m34 { 0.0 };
    double m43 { 0.0 };
    double m44 { 0.0 };
    Optional<bool> is2d;
};

// https://drafts.fxtf.org/geometry/#dommatrixreadonly
class DOMMatrixReadOnly
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(DOMMatrixReadOnly, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DOMMatrixReadOnly);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrixReadOnly>> construct_impl(JS::Realm&, Optional<Variant<String, Vector<double>>> const& init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrixReadOnly>> create_from_dom_matrix_2d_init(JS::Realm&, DOMMatrix2DInit& init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrixReadOnly>> create_from_dom_matrix_init(JS::Realm&, DOMMatrixInit& init);
    static JS::NonnullGCPtr<DOMMatrixReadOnly> create(JS::Realm&);

    virtual ~DOMMatrixReadOnly() override;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrixReadOnly>> from_matrix(JS::VM&, DOMMatrixInit& other);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrixReadOnly>> from_float32_array(JS::VM&, JS::Handle<WebIDL::BufferSource> const& array32);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrixReadOnly>> from_float64_array(JS::VM&, JS::Handle<WebIDL::BufferSource> const& array64);

    // https://drafts.fxtf.org/geometry/#dommatrix-attributes
    double m11() const { return m_matrix.elements()[0][0]; }
    double m12() const { return m_matrix.elements()[1][0]; }
    double m13() const { return m_matrix.elements()[2][0]; }
    double m14() const { return m_matrix.elements()[3][0]; }
    double m21() const { return m_matrix.elements()[0][1]; }
    double m22() const { return m_matrix.elements()[1][1]; }
    double m23() const { return m_matrix.elements()[2][1]; }
    double m24() const { return m_matrix.elements()[3][1]; }
    double m31() const { return m_matrix.elements()[0][2]; }
    double m32() const { return m_matrix.elements()[1][2]; }
    double m33() const { return m_matrix.elements()[2][2]; }
    double m34() const { return m_matrix.elements()[3][2]; }
    double m41() const { return m_matrix.elements()[0][3]; }
    double m42() const { return m_matrix.elements()[1][3]; }
    double m43() const { return m_matrix.elements()[2][3]; }
    double m44() const { return m_matrix.elements()[3][3]; }

    double a() const { return m11(); }
    double b() const { return m12(); }
    double c() const { return m21(); }
    double d() const { return m22(); }
    double e() const { return m41(); }
    double f() const { return m42(); }

    bool is2d() const { return m_is_2d; }
    bool is_identity() const;

    JS::NonnullGCPtr<DOMMatrix> translate(Optional<double> const& tx, Optional<double> const& ty, Optional<double> const& tz) const;
    JS::NonnullGCPtr<DOMMatrix> scale(Optional<double> scale_x, Optional<double> scale_y, Optional<double> scale_z, Optional<double> origin_x, Optional<double> origin_y, Optional<double> origin_z);
    JS::NonnullGCPtr<DOMMatrix> scale_non_uniform(Optional<double> scale_x, Optional<double> scale_y);
    JS::NonnullGCPtr<DOMMatrix> scale3d(Optional<double> scale, Optional<double> origin_x, Optional<double> origin_y, Optional<double> origin_z);
    JS::NonnullGCPtr<DOMMatrix> rotate(Optional<double> rot_x, Optional<double> rot_y, Optional<double> rot_z);
    JS::NonnullGCPtr<DOMMatrix> rotate_from_vector(Optional<double> x, Optional<double> y);
    JS::NonnullGCPtr<DOMMatrix> rotate_axis_angle(Optional<double> x, Optional<double> y, Optional<double> z, Optional<double> angle);
    JS::NonnullGCPtr<DOMMatrix> skew_x(double sx = 0) const;
    JS::NonnullGCPtr<DOMMatrix> skew_y(double sy = 0) const;
    WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> multiply(DOMMatrixInit other = {});
    JS::NonnullGCPtr<DOMMatrix> flip_x();
    JS::NonnullGCPtr<DOMMatrix> flip_y();
    JS::NonnullGCPtr<DOMMatrix> inverse() const;

    JS::NonnullGCPtr<DOMPoint> transform_point(DOMPointInit const&) const;
    JS::NonnullGCPtr<DOMPoint> transform_point(DOMPointReadOnly const&) const;
    JS::NonnullGCPtr<JS::Float32Array> to_float32_array() const;
    JS::NonnullGCPtr<JS::Float64Array> to_float64_array() const;

    WebIDL::ExceptionOr<String> to_string() const;

    virtual StringView interface_name() const override { return "DOMMatrixReadOnly"sv; }
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::SerializationRecord&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&) override;

protected:
    DOMMatrixReadOnly(JS::Realm&, double m11, double m12, double m21, double m22, double m41, double m42);
    DOMMatrixReadOnly(JS::Realm&, double m11, double m12, double m13, double m14, double m21, double m22, double m23, double m24, double m31, double m32, double m33, double m34, double m41, double m42, double m43, double m44);
    DOMMatrixReadOnly(JS::Realm&, DOMMatrixReadOnly const& other);
    explicit DOMMatrixReadOnly(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    // NOTE: The matrix used in the spec is column-major (https://drafts.fxtf.org/geometry/#4x4-abstract-matrix) but Gfx::Matrix4x4 is row-major so we need to transpose the values.
    Gfx::DoubleMatrix4x4 m_matrix { Gfx::DoubleMatrix4x4::identity() };

    bool m_is_2d { true };

private:
    void initialize_from_create_2d_matrix(double m11, double m12, double m21, double m22, double m41, double m42);
    void initialize_from_create_3d_matrix(double m11, double m12, double m13, double m14, double m21, double m22, double m23, double m24, double m31, double m32, double m33, double m34, double m41, double m42, double m43, double m44);
};

WebIDL::ExceptionOr<void> validate_and_fixup_dom_matrix_2d_init(DOMMatrix2DInit& init);
WebIDL::ExceptionOr<void> validate_and_fixup_dom_matrix_init(DOMMatrixInit& init);

struct ParsedMatrix {
    Gfx::DoubleMatrix4x4 matrix;
    bool is_2d_transform;
};

WebIDL::ExceptionOr<ParsedMatrix> parse_dom_matrix_init_string(JS::Realm& realm, StringView transform_list);

}
