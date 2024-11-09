/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dommatrix
class DOMMatrix : public DOMMatrixReadOnly {
    WEB_PLATFORM_OBJECT(DOMMatrix, DOMMatrixReadOnly);
    JS_DECLARE_ALLOCATOR(DOMMatrix);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> construct_impl(JS::Realm&, Optional<Variant<String, Vector<double>>> const& init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> create_from_dom_matrix_2d_init(JS::Realm&, DOMMatrix2DInit& init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> create_from_dom_matrix_init(JS::Realm&, DOMMatrixInit& init);
    static JS::NonnullGCPtr<DOMMatrix> create_from_dom_matrix_read_only(JS::Realm&, DOMMatrixReadOnly const& read_only_matrix);
    static JS::NonnullGCPtr<DOMMatrix> create(JS::Realm&);

    virtual ~DOMMatrix() override;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> from_matrix(JS::VM&, DOMMatrixInit other = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> from_float32_array(JS::VM&, JS::Handle<WebIDL::BufferSource> const& array32);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> from_float64_array(JS::VM&, JS::Handle<WebIDL::BufferSource> const& array64);

    void set_m11(double value);
    void set_m12(double value);
    void set_m13(double value);
    void set_m14(double value);
    void set_m21(double value);
    void set_m22(double value);
    void set_m23(double value);
    void set_m24(double value);
    void set_m31(double value);
    void set_m32(double value);
    void set_m33(double value);
    void set_m34(double value);
    void set_m41(double value);
    void set_m42(double value);
    void set_m43(double value);
    void set_m44(double value);

    void set_a(double value);
    void set_b(double value);
    void set_c(double value);
    void set_d(double value);
    void set_e(double value);
    void set_f(double value);

    WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> multiply_self(DOMMatrixInit other = {});
    WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> pre_multiply_self(DOMMatrixInit other = {});
    JS::NonnullGCPtr<DOMMatrix> translate_self(Optional<double> tx, Optional<double> ty, Optional<double> tz);
    JS::NonnullGCPtr<DOMMatrix> scale_self(Optional<double> scale_x, Optional<double> scale_y, Optional<double> scale_z, Optional<double> origin_x, Optional<double> origin_y, Optional<double> origin_z);
    JS::NonnullGCPtr<DOMMatrix> scale3d_self(Optional<double> scale, Optional<double> origin_x, Optional<double> origin_y, Optional<double> origin_z);
    JS::NonnullGCPtr<DOMMatrix> rotate_self(Optional<double> rot_x, Optional<double> rot_y, Optional<double> rot_z);
    JS::NonnullGCPtr<DOMMatrix> rotate_from_vector_self(Optional<double> x, Optional<double> y);
    JS::NonnullGCPtr<DOMMatrix> rotate_axis_angle_self(Optional<double> x, Optional<double> y, Optional<double> z, Optional<double> angle);
    JS::NonnullGCPtr<DOMMatrix> skew_x_self(double sx = 0);
    JS::NonnullGCPtr<DOMMatrix> skew_y_self(double sy = 0);
    JS::NonnullGCPtr<DOMMatrix> invert_self();

    WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> set_matrix_value(String const& transform_list);

    virtual StringView interface_name() const override { return "DOMMatrix"sv; }

private:
    DOMMatrix(JS::Realm&, double m11, double m12, double m21, double m22, double m41, double m42);
    DOMMatrix(JS::Realm&, double m11, double m12, double m13, double m14, double m21, double m22, double m23, double m24, double m31, double m32, double m33, double m34, double m41, double m42, double m43, double m44);
    DOMMatrix(JS::Realm&, DOMMatrixReadOnly const& read_only_matrix);
    explicit DOMMatrix(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
