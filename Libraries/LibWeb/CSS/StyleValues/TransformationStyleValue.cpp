/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Steffen T. Larssen <dudedbz@gmail.com>
 * Copyright (c) 2024-2025, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSMatrixComponent.h>
#include <LibWeb/CSS/CSSPerspective.h>
#include <LibWeb/CSS/CSSRotate.h>
#include <LibWeb/CSS/CSSScale.h>
#include <LibWeb/CSS/CSSSkew.h>
#include <LibWeb/CSS/CSSSkewX.h>
#include <LibWeb/CSS/CSSSkewY.h>
#include <LibWeb/CSS/CSSTransformComponent.h>
#include <LibWeb/CSS/CSSTranslate.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Geometry/DOMMatrix.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-transform-function
GC::Ptr<CSSTransformComponent> TransformationStyleValue::reify_a_transform_function() const
{
    auto values = this->values();

    auto reify_numeric_argument = [&](size_t index) {
        return GC::Ref { as<CSSNumericValue>(*values[index]->reify({})) };
    };
    auto reify_0 = [&] { return CSSUnitValue::create(0, "number"_utf16_fly_string); };
    auto reify_1 = [&] { return CSSUnitValue::create(1, "number"_utf16_fly_string); };
    auto reify_0px = [&] { return CSSUnitValue::create(0, "px"_utf16_fly_string); };
    auto reify_0deg = [&] { return CSSUnitValue::create(0, "deg"_utf16_fly_string); };

    // To reify a <transform-function> func, perform the appropriate set of steps below, based on func:
    switch (transform_function()) {
    // -> matrix()
    // -> matrix3d()
    //    1. Return a new CSSMatrixComponent object, whose matrix internal slot is set to a 4x4 matrix representing the
    //       same information as func, and whose is2D internal slot is true if func is matrix(), and false otherwise.
    case TransformFunction::Matrix:
    case TransformFunction::Matrix3d: {
        Array<float, 16> transform_as_matrix;
        bool is_2d_matrix = true;
        if (!ComputedValuesFFI::rust_transform_list_to_abstract_matrix(rust_style_value_data(), transform_as_matrix.data(), &is_2d_matrix))
            return nullptr;

        auto matrix = Geometry::DOMMatrix::create();
        matrix->set_m11(transform_as_matrix[0]);
        matrix->set_m12(transform_as_matrix[4]);
        matrix->set_m13(transform_as_matrix[8]);
        matrix->set_m14(transform_as_matrix[12]);
        matrix->set_m21(transform_as_matrix[1]);
        matrix->set_m22(transform_as_matrix[5]);
        matrix->set_m23(transform_as_matrix[9]);
        matrix->set_m24(transform_as_matrix[13]);
        matrix->set_m31(transform_as_matrix[2]);
        matrix->set_m32(transform_as_matrix[6]);
        matrix->set_m33(transform_as_matrix[10]);
        matrix->set_m34(transform_as_matrix[14]);
        matrix->set_m41(transform_as_matrix[3]);
        matrix->set_m42(transform_as_matrix[7]);
        matrix->set_m43(transform_as_matrix[11]);
        matrix->set_m44(transform_as_matrix[15]);

        auto is_2d = transform_function() == TransformFunction::Matrix ? CSSTransformComponent::Is2D::Yes : CSSTransformComponent::Is2D::No;
        return CSSMatrixComponent::create(is_2d, matrix);
    }

    // -> translate()
    // -> translateX()
    // -> translateY()
    // -> translate3d()
    // -> translateZ()
    //    1. Return a new CSSTranslate object, whose x, y, and z internal slots are set to the reification of the
    //       specified x/y/z offsets, or the reification of 0px if not specified in func, and whose is2D internal slot
    //       is true if func is translate(), translateX(), or translateY(), and false otherwise.
    case TransformFunction::Translate: {
        // NB: Default y to 0px if it's not specified.
        auto y = values.size() > 1 ? reify_numeric_argument(1) : reify_0px();
        return CSSTranslate::create(CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), y, reify_0px());
    }
    case TransformFunction::TranslateX:
        return CSSTranslate::create(CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), reify_0px(), reify_0px());
    case TransformFunction::TranslateY:
        return CSSTranslate::create(CSSTransformComponent::Is2D::Yes, reify_0px(), reify_numeric_argument(0), reify_0px());
    case TransformFunction::Translate3d:
        return CSSTranslate::create(CSSTransformComponent::Is2D::No, reify_numeric_argument(0), reify_numeric_argument(1), reify_numeric_argument(2));
    case TransformFunction::TranslateZ:
        return CSSTranslate::create(CSSTransformComponent::Is2D::No, reify_0px(), reify_0px(), reify_numeric_argument(0));

    // -> scale()
    // -> scaleX()
    // -> scaleY()
    // -> scale3d()
    // -> scaleZ()
    //    1. Return a new CSSScale object, whose x, y, and z internal slots are set to the specified x/y/z scales, or
    //       to 1 if not specified in func and whose is2D internal slot is true if func is scale(), scaleX(), or
    //       scaleY(), and false otherwise.
    case TransformFunction::Scale: {
        // NB: Default y to a copy of x if it's not specified.
        auto y = values.size() > 1 ? reify_numeric_argument(1) : reify_numeric_argument(0);
        return CSSScale::create(CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), y, reify_1());
    }
    case TransformFunction::ScaleX:
        return CSSScale::create(CSSTransformComponent::Is2D::Yes, reify_numeric_argument(0), reify_1(), reify_1());
    case TransformFunction::ScaleY:
        return CSSScale::create(CSSTransformComponent::Is2D::Yes, reify_1(), reify_numeric_argument(0), reify_1());
    case TransformFunction::Scale3d:
        return CSSScale::create(CSSTransformComponent::Is2D::No, reify_numeric_argument(0), reify_numeric_argument(1), reify_numeric_argument(2));
    case TransformFunction::ScaleZ:
        return CSSScale::create(CSSTransformComponent::Is2D::No, reify_1(), reify_1(), reify_numeric_argument(0));

    // -> rotate()
    // -> rotate3d()
    // -> rotateX()
    // -> rotateY()
    // -> rotateZ()
    //    1. Return a new CSSRotate object, whose angle internal slot is set to the reification of the specified angle,
    //       and whose x, y, and z internal slots are set to the specified rotation axis coordinates, or the implicit
    //       axis coordinates if not specified in func and whose is2D internal slot is true if func is rotate(), and
    //       false otherwise.
    case TransformFunction::Rotate:
        return CSSRotate::create(CSSTransformComponent::Is2D::Yes, reify_0(), reify_0(), reify_1(), reify_numeric_argument(0));
    case TransformFunction::Rotate3d:
        return CSSRotate::create(CSSTransformComponent::Is2D::No, reify_numeric_argument(0), reify_numeric_argument(1), reify_numeric_argument(2), reify_numeric_argument(3));
    case TransformFunction::RotateX:
        return CSSRotate::create(CSSTransformComponent::Is2D::No, reify_1(), reify_0(), reify_0(), reify_numeric_argument(0));
    case TransformFunction::RotateY:
        return CSSRotate::create(CSSTransformComponent::Is2D::No, reify_0(), reify_1(), reify_0(), reify_numeric_argument(0));
    case TransformFunction::RotateZ:
        return CSSRotate::create(CSSTransformComponent::Is2D::No, reify_0(), reify_0(), reify_1(), reify_numeric_argument(0));

    // -> skew()
    //    1. Return a new CSSSkew object, whose ax and ay internal slots are set to the reification of the specified x
    //       and y angles, or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
    case TransformFunction::Skew: {
        // NB: Default y to 0deg if it's not specified.
        auto y = values.size() > 1 ? reify_numeric_argument(1) : reify_0deg();
        return CSSSkew::create(reify_numeric_argument(0), y);
    }

    // -> skewX()
    //    1. Return a new CSSSkewX object, whose ax internal slot is set to the reification of the specified x angle,
    //       or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
    case TransformFunction::SkewX:
        return CSSSkewX::create(reify_numeric_argument(0));

    // -> skewY()
    //    1. Return a new CSSSkewY object, whose ay internal slot is set to the reification of the specified y angle,
    //       or the reification of 0deg if not specified in func, and whose is2D internal slot is true.
    case TransformFunction::SkewY:
        return CSSSkewY::create(reify_numeric_argument(0));

    // -> perspective()
    //    1. Return a new CSSPerspective object, whose length internal slot is set to the reification of the specified
    //       length (see reify a numeric value if it is a length, and reify an identifier if it is the keyword none)
    //       and whose is2D internal slot is false.
    case TransformFunction::Perspective: {
        CSSPerspectiveValueInternal length = [&]() -> CSSPerspectiveValueInternal {
            auto reified = values[0]->reify({});
            if (auto* keyword = as_if<CSSKeywordValue>(*reified))
                return GC::Ref { *keyword };
            if (auto* numeric = as_if<CSSNumericValue>(*reified))
                return GC::Ref { *numeric };
            VERIFY_NOT_REACHED();
        }();
        return CSSPerspective::create(length);
    }
    }
    VERIFY_NOT_REACHED();
}

Vector<NonnullRefPtr<TransformationStyleValue const>> transformations_for_style_value(StyleValue const& value)
{
    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return {};

    if (!value.is_value_list())
        return {};

    auto& list = value.as_value_list();
    Vector<NonnullRefPtr<TransformationStyleValue const>> transformations;
    for (auto const& transform_value : list.values()) {
        VERIFY(transform_value->is_transformation());
        transformations.append(transform_value->as_transformation());
    }
    return transformations;
}

}
