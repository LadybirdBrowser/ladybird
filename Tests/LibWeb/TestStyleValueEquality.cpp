/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LightDarkStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

// These tests build separately-allocated style values with equal (or deliberately unequal)
// contents and check that StyleValue::equals() compares by value, not by pointer identity.
// Style value equality feeds restyle invalidation and transition change detection, so drift
// here does not show up in rendering tests until it causes stale styles or spurious
// transitions.

namespace Web::CSS {

TEST_CASE(rust_scalar_handles_create_typed_wrappers)
{
    auto number = [] {
        auto original = NumberStyleValue::create(42);
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(original->rust_style_value_data()));
    }();

    EXPECT(number->is_number());
    EXPECT_EQ(number->as_number().number(), 42);

    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_keyword(0))->is_keyword());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_integer(1))->is_integer());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(1, 0))->is_angle());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_flex(1, 0))->is_flex());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_frequency(1, 0))->is_frequency());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_length(1, 0))->is_length());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_percentage(1))->is_percentage());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_resolution(1, 0))->is_resolution());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_time(1, 0))->is_time());
}

TEST_CASE(rust_transformation_handles_retain_child_data)
{
    auto data = [] {
        auto original = TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::ScaleX,
            { NumberStyleValue::create(2) });
        return StyleValueFFI::rust_style_value_retain(original->rust_style_value_data());
    }();

    auto child = [&] {
        auto transformation = StyleValue::adopt_rust_style_value_data(data);
        EXPECT(transformation->is_transformation());
        auto values = transformation->as_transformation().values();
        EXPECT_EQ(values.size(), 1u);
        return values[0];
    }();

    EXPECT(child->is_number());
    EXPECT_EQ(child->as_number().number(), 2);
}

TEST_CASE(rust_value_list_handles_retain_child_data)
{
    auto data = [] {
        auto original = StyleValueList::create(
            { NumberStyleValue::create(3) },
            StyleValueList::Separator::Space);
        return StyleValueFFI::rust_style_value_retain(original->rust_style_value_data());
    }();

    auto child = [&] {
        auto list = StyleValue::adopt_rust_style_value_data(data);
        EXPECT(list->is_value_list());
        auto values = list->as_value_list().values();
        EXPECT_EQ(values.size(), 1u);
        return values[0];
    }();

    EXPECT(child->is_number());
    EXPECT_EQ(child->as_number().number(), 3);
}

TEST_CASE(rust_tuple_handles_retain_optional_child_data)
{
    auto data = [] {
        auto original = TupleStyleValue::create({ NumberStyleValue::create(4), nullptr });
        return StyleValueFFI::rust_style_value_retain(original->rust_style_value_data());
    }();

    auto child = [&] {
        auto tuple = StyleValue::adopt_rust_style_value_data(data);
        EXPECT(tuple->is_tuple());
        auto values = tuple->as_tuple().tuple();
        EXPECT_EQ(values.size(), 2u);
        EXPECT(!values[1]);
        return values[0];
    }();

    EXPECT(child);
    EXPECT(child->is_number());
    EXPECT_EQ(child->as_number().number(), 4);
}

TEST_CASE(rust_interpolates_matching_transform_functions)
{
    auto make_transform = [](double degrees) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Rotate,
                { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(degrees, 0)) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(0);
    auto to = make_transform(360);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 1u);
    EXPECT(arguments[0]->is_angle());
    EXPECT_APPROXIMATE(arguments[0]->rust_style_value_data()->angle.value, 90.0);
    EXPECT_EQ(arguments[0]->rust_style_value_data()->angle.unit, 0u);
}

TEST_CASE(rust_interpolates_perspective_transform_functions)
{
    auto make_transform = [](double depth) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Perspective,
                { LengthStyleValue::create(Length::make_px(depth)) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(400);
    auto to = make_transform(500);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 1u);
    EXPECT(arguments[0]->is_length());
    EXPECT_APPROXIMATE(arguments[0]->as_length().length().raw_value(), 421.0526315789474);
    EXPECT(arguments[0]->as_length().length().is_px());
}

TEST_CASE(rust_interpolates_rotate_3d_transform_functions)
{
    auto make_transform = [](double x, double y, double z, double degrees) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Rotate3d,
                { NumberStyleValue::create(x),
                    NumberStyleValue::create(y),
                    NumberStyleValue::create(z),
                    StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(degrees, 0)) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(1, 1, 0, 90);
    auto to = make_transform(0, 1, 1, 180);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 4u);
    EXPECT_APPROXIMATE(arguments[0]->as_number().number(), 0.5240828967217527);
    EXPECT_APPROXIMATE(arguments[1]->as_number().number(), 0.8042617338748014);
    EXPECT_APPROXIMATE(arguments[2]->as_number().number(), 0.28017883715304875);
    EXPECT_APPROXIMATE(arguments[3]->rust_style_value_data()->angle.value, 106.91089335915852);
}

TEST_CASE(rust_interpolates_transform_functions_with_a_common_primitive)
{
    auto make_transform = [](TransformFunction function, double value) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                function,
                { NumberStyleValue::create(value) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(TransformFunction::ScaleX, 2);
    auto to = make_transform(TransformFunction::ScaleY, 3);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    EXPECT_EQ(transformations[0]->as_transformation().transform_function(), TransformFunction::Scale);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 2u);
    EXPECT_APPROXIMATE(arguments[0]->as_number().number(), 1.5);
    EXPECT_APPROXIMATE(arguments[1]->as_number().number(), 2.0);
}

TEST_CASE(rust_extends_transform_lists_with_identity_functions)
{
    auto from = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::ScaleX,
            { NumberStyleValue::create(2) }) },
        StyleValueList::Separator::Space);
    auto to = StyleValueList::create(
        { TransformationStyleValue::create(
              PropertyID::Transform,
              TransformFunction::ScaleX,
              { NumberStyleValue::create(4) }),
            TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::TranslateX,
                { LengthStyleValue::create(Length::make_px(100)) }) },
        StyleValueList::Separator::Space);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 2u);
    EXPECT_APPROXIMATE(transformations[0]->as_transformation().values()[0]->as_number().number(), 3.0);
    EXPECT_APPROXIMATE(transformations[1]->as_transformation().values()[0]->as_length().length().raw_value(), 50.0);
}

TEST_CASE(rust_treats_transform_none_as_an_empty_list)
{
    auto from = KeywordStyleValue::create(Keyword::None);
    auto to = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::Rotate,
            { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(90, 0)) }) },
        StyleValueList::Separator::Space);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 1u);
    EXPECT_APPROXIMATE(arguments[0]->rust_style_value_data()->angle.value, 45.0);
}

TEST_CASE(rust_interpolates_terminal_matrix_transform_functions)
{
    auto make_transform = [](double scale_x, double scale_y, double translate_x, double translate_y) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Matrix,
                { NumberStyleValue::create(scale_x),
                    NumberStyleValue::create(0),
                    NumberStyleValue::create(0),
                    NumberStyleValue::create(scale_y),
                    NumberStyleValue::create(translate_x),
                    NumberStyleValue::create(translate_y) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(2, 2, 10, 30);
    auto to = make_transform(4, 6, 14, 10);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    EXPECT_EQ(transformations[0]->as_transformation().transform_function(), TransformFunction::Matrix3d);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 16u);
    EXPECT_APPROXIMATE(arguments[0]->as_number().number(), 3.0);
    EXPECT_APPROXIMATE(arguments[5]->as_number().number(), 4.0);
    EXPECT_APPROXIMATE(arguments[12]->as_number().number(), 12.0);
    EXPECT_APPROXIMATE(arguments[13]->as_number().number(), 20.0);
}

TEST_CASE(rust_post_multiplies_transform_matrix_suffixes)
{
    auto make_transform = [](double translation) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                  PropertyID::Transform,
                  TransformFunction::Matrix,
                  { NumberStyleValue::create(1),
                      NumberStyleValue::create(0),
                      NumberStyleValue::create(0),
                      NumberStyleValue::create(1),
                      NumberStyleValue::create(0),
                      NumberStyleValue::create(0) }),
                TransformationStyleValue::create(
                    PropertyID::Transform,
                    TransformFunction::TranslateX,
                    { LengthStyleValue::create(Length::make_px(translation)) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(100);
    auto to = make_transform(200);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 16u);
    EXPECT_APPROXIMATE(arguments[12]->as_number().number(), 150.0);
}

TEST_CASE(rust_interpolates_transform_functions_without_a_common_primitive)
{
    auto from = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::Rotate,
            { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(0, 0)) }) },
        StyleValueList::Separator::Space);
    auto to = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::Scale,
            { NumberStyleValue::create(2) }) },
        StyleValueList::Separator::Space);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 16u);
    EXPECT_APPROXIMATE(arguments[0]->as_number().number(), 1.5);
    EXPECT_APPROXIMATE(arguments[5]->as_number().number(), 1.5);
}

TEST_CASE(rust_resolves_percentage_transforms_against_a_reference_box_snapshot)
{
    auto from = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::Rotate,
            { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(0, 0)) }) },
        StyleValueList::Separator::Space);
    auto to = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::TranslateX,
            { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_percentage(50)) }) },
        StyleValueList::Separator::Space);
    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = false,
        .has_transform_reference_box = true,
        .transform_reference_box_width = 200,
        .transform_reference_box_height = 100,
    };

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto transformations = interpolated->as_value_list().values();
    EXPECT_EQ(transformations.size(), 1u);
    auto arguments = transformations[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 16u);
    EXPECT_APPROXIMATE(arguments[12]->as_number().number(), 50.0);
}

TEST_CASE(rust_handles_non_invertible_transform_matrices_without_a_value)
{
    auto make_transform = [](double scale) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Matrix,
                { NumberStyleValue::create(scale),
                    NumberStyleValue::create(0),
                    NumberStyleValue::create(0),
                    NumberStyleValue::create(scale),
                    NumberStyleValue::create(0),
                    NumberStyleValue::create(0) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(1);
    auto to = make_transform(0);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = true,
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto discrete = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT(discrete->equals(*to));
}

TEST_CASE(counter_definitions_equality_is_deep)
{
    auto make_definitions = [](i32 value) {
        Vector<CounterDefinition> definitions;
        definitions.append(CounterDefinition {
            .name = "chapter"_utf16_fly_string,
            .is_reversed = false,
            .value = IntegerStyleValue::create(value),
        });
        return CounterDefinitionsStyleValue::create(move(definitions));
    };

    auto first = make_definitions(1);
    auto same_as_first = make_definitions(1);
    auto different = make_definitions(2);

    EXPECT(first->equals(same_as_first));
    EXPECT(!first->equals(different));
}

static NonnullRefPtr<RadialGradientStyleValue const> make_radial_gradient()
{
    Vector<ColorStopListElement> stops;
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = KeywordStyleValue::create(Keyword::Currentcolor), .position = nullptr } });
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = KeywordStyleValue::create(Keyword::None), .position = nullptr } });
    return RadialGradientStyleValue::create(
        RadialGradientStyleValue::EndingShape::Ellipse,
        KeywordStyleValue::create(Keyword::FarthestCorner),
        PositionStyleValue::create_center(),
        move(stops),
        GradientRepeating::No,
        nullptr);
}

TEST_CASE(radial_gradient_size_equality_is_deep)
{
    // The size sub-values are separate allocations with equal contents; the gradients must
    // still compare equal.
    EXPECT(make_radial_gradient()->equals(*make_radial_gradient()));
}

static NonnullRefPtr<ConicGradientStyleValue const> make_conic_gradient(ColorSyntax gradient_color_syntax)
{
    auto make_color = [](double r, double g, double b) {
        return ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::RGB,
            NumberStyleValue::create(r),
            NumberStyleValue::create(g),
            NumberStyleValue::create(b),
            NumberStyleValue::create(1),
            ColorSyntax::Legacy);
    };
    Vector<ColorStopListElement> stops;
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = make_color(255, 0, 0), .position = nullptr } });
    stops.append(ColorStopListElement { .transition_hint = nullptr, .color_stop = { .color = make_color(0, 0, 255), .position = nullptr } });
    return ConicGradientStyleValue::create(nullptr, PositionStyleValue::create_center(), move(stops), GradientRepeating::No, nullptr, gradient_color_syntax);
}

TEST_CASE(conic_gradient_equality_considers_color_syntax)
{
    auto legacy = make_conic_gradient(ColorSyntax::Legacy);
    auto modern = make_conic_gradient(ColorSyntax::Modern);

    EXPECT(legacy->equals(*make_conic_gradient(ColorSyntax::Legacy)));
    EXPECT(!legacy->equals(*modern));
}

TEST_CASE(color_equality_rejects_different_color_variants)
{
    auto color_function = ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::RGB,
        NumberStyleValue::create(255),
        NumberStyleValue::create(255),
        NumberStyleValue::create(255),
        NumberStyleValue::create(1),
        ColorSyntax::Legacy);
    auto light_dark = LightDarkStyleValue::create(
        ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::RGB,
            NumberStyleValue::create(255),
            NumberStyleValue::create(255),
            NumberStyleValue::create(255),
            NumberStyleValue::create(1),
            ColorSyntax::Legacy),
        ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::RGB,
            NumberStyleValue::create(0),
            NumberStyleValue::create(0),
            NumberStyleValue::create(0),
            NumberStyleValue::create(1),
            ColorSyntax::Legacy));

    EXPECT(!static_cast<StyleValue const&>(*color_function).equals(*light_dark));
    EXPECT(!static_cast<StyleValue const&>(*light_dark).equals(*color_function));
}

TEST_CASE(radial_size_equality_is_deep)
{
    auto make_size = [](double width, double height) {
        Vector<RadialSizeStyleValue::Component> components;
        components.append(NonnullRefPtr<StyleValue const> { NumberStyleValue::create(width) });
        components.append(NonnullRefPtr<StyleValue const> { NumberStyleValue::create(height) });
        return RadialSizeStyleValue::create(move(components));
    };

    EXPECT(make_size(50, 30)->equals(*make_size(50, 30)));
    EXPECT(!make_size(50, 30)->equals(*make_size(50, 40)));
}

TEST_CASE(unresolved_equality_trims_only_ascii_whitespace)
{
    auto make_unresolved = [](String source_text) {
        return UnresolvedStyleValue::create({}, {}, move(source_text));
    };

    // U+00A0 has the Unicode White_Space property but is not ASCII whitespace; values differing
    // by it must not compare equal, or custom-property change detection misses the update.
    auto plain = make_unresolved("foo"_string);
    auto with_leading_nbsp = make_unresolved("\u00A0foo"_string);

    EXPECT(plain->equals(*make_unresolved("foo"_string)));
    EXPECT(!plain->equals(*with_leading_nbsp));
}

}
