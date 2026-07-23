/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusRectStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LightDarkStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextIndentStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

// These tests build separately-allocated style values with equal (or deliberately unequal)
// contents and check that StyleValue::equals() compares by value, not by pointer identity.
// Style value equality feeds restyle invalidation and transition change detection, so drift
// here does not show up in rendering tests until it causes stale styles or spurious
// transitions.

namespace Web::CSS {

TEST_CASE(rust_composites_scalar_style_values)
{
    auto underlying_number = NumberStyleValue::create(2);
    auto animated_number = NumberStyleValue::create(3);
    auto result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_number->rust_style_value_data(),
        animated_number->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto number = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(number->as_number().number(), 5);

    auto underlying_length = LengthStyleValue::create(Length::make_px(10));
    auto animated_length = LengthStyleValue::create(Length::make_px(15));
    result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_length->rust_style_value_data(),
        animated_length->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Accumulate);
    EXPECT(result.handled);
    auto length = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(length->as_length().length().raw_value(), 25);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_number->rust_style_value_data(),
        animated_number->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Replace);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    auto underlying_ratio = RatioStyleValue::create(NumberStyleValue::create(4), NumberStyleValue::create(3));
    auto animated_ratio = RatioStyleValue::create(NumberStyleValue::create(16), NumberStyleValue::create(9));
    result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_ratio->rust_style_value_data(),
        animated_ratio->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    auto underlying_opacity = OpacityValueStyleValue::create(NumberStyleValue::create(0.75));
    auto animated_opacity = OpacityValueStyleValue::create(NumberStyleValue::create(0.75));
    result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_opacity->rust_style_value_data(),
        animated_opacity->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto opacity = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(opacity->as_opacity_value().resolved(), 1);
}

TEST_CASE(rust_interpolates_and_composites_scalar_dimensions)
{
    auto from_frequency = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_frequency(100, 0));
    auto to_frequency = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_frequency(200, 0));
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Width),
        from_frequency->rust_style_value_data(),
        to_frequency->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    auto frequency = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(frequency->rust_style_value_data()->frequency.value, 125);

    auto underlying_time = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_time(2, 0));
    auto animated_time = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_time(3, 0));
    result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_time->rust_style_value_data(),
        animated_time->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto time = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(time->rust_style_value_data()->time.value, 5);
}

TEST_CASE(rust_interpolates_and_composites_value_lists)
{
    auto from = StyleValueList::create({ NumberStyleValue::create(1), NumberStyleValue::create(3) }, StyleValueList::Separator::Space);
    auto to = StyleValueList::create({ NumberStyleValue::create(3), NumberStyleValue::create(7) }, StyleValueList::Separator::Space);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Width),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->as_value_list().values()[0]->as_number().number(), 2);
    EXPECT_EQ(interpolated->as_value_list().values()[1]->as_number().number(), 5);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->as_value_list().values()[0]->as_number().number(), 4);
    EXPECT_EQ(composited->as_value_list().values()[1]->as_number().number(), 10);
}

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

TEST_CASE(rust_superellipse_handles_retain_parameter_data)
{
    auto data = [] {
        auto original = SuperellipseStyleValue::create(NumberStyleValue::create(4));
        return StyleValueFFI::rust_style_value_retain(original->rust_style_value_data());
    }();

    auto superellipse = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(superellipse->is_superellipse());
    EXPECT_EQ(superellipse->to_string(SerializationMode::Normal), "superellipse(4)"sv);
}

TEST_CASE(rust_interpolates_superellipse_values)
{
    auto from = SuperellipseStyleValue::create(NumberStyleValue::create(-AK::Infinity<double>));
    auto to = SuperellipseStyleValue::create(NumberStyleValue::create(AK::Infinity<double>));
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::CornerTopLeftShape),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::ResolvedValue), "bevel"sv);
}

TEST_CASE(rust_text_indent_handles_retain_length_percentage_data)
{
    auto data = [] {
        auto length = LengthStyleValue::create(Length::make_px(4));
        return StyleValueFFI::rust_style_value_create_text_indent(
            StyleValueFFI::rust_style_value_retain(length->rust_style_value_data()), true, true);
    }();

    auto text_indent = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(text_indent->is_text_indent());
    EXPECT_EQ(text_indent->to_string(SerializationMode::Normal), "4px each-line hanging"sv);
}

TEST_CASE(rust_interpolates_and_composites_text_indent_values)
{
    auto make_text_indent = [](double pixels) {
        auto length = LengthStyleValue::create(Length::make_px(pixels));
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_text_indent(
            StyleValueFFI::rust_style_value_retain(length->rust_style_value_data()), true, false));
    };
    auto from = make_text_indent(2);
    auto to = make_text_indent(6);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::TextIndent),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "4px hanging"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "8px hanging"sv);
}

TEST_CASE(rust_background_size_handles_retain_child_data)
{
    auto data = [] {
        auto size_x = LengthStyleValue::create(Length::make_px(4));
        auto size_y = LengthStyleValue::create(Length::make_px(8));
        return StyleValueFFI::rust_style_value_create_background_size(
            StyleValueFFI::rust_style_value_retain(size_x->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(size_y->rust_style_value_data()));
    }();

    auto background_size = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(background_size->is_background_size());
    EXPECT_EQ(background_size->to_string(SerializationMode::Normal), "4px 8px"sv);
}

TEST_CASE(rust_edge_handles_retain_optional_offset_data)
{
    auto data = [] {
        auto offset = LengthStyleValue::create(Length::make_px(4));
        return StyleValueFFI::rust_style_value_create_edge(
            false, 0, StyleValueFFI::rust_style_value_retain(offset->rust_style_value_data()));
    }();

    auto edge = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(edge->is_edge());
    EXPECT_EQ(edge->to_string(SerializationMode::Normal), "4px"sv);

    auto centered_edge = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_create_edge(true, to_underlying(PositionEdge::Center), nullptr));
    EXPECT_EQ(centered_edge->to_string(SerializationMode::Normal), "center"sv);
}

TEST_CASE(rust_interpolates_and_composites_edge_offsets)
{
    auto make_edge = [](double pixels) {
        return EdgeStyleValue::create({}, LengthStyleValue::create(Length::make_px(pixels)));
    };
    auto from = make_edge(4);
    auto to = make_edge(8);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Left),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "6px"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "12px"sv);
}

TEST_CASE(rust_position_handles_retain_edge_data)
{
    auto data = [] {
        auto edge_x = EdgeStyleValue::create({}, LengthStyleValue::create(Length::make_px(4)));
        auto edge_y = EdgeStyleValue::create({}, LengthStyleValue::create(Length::make_px(8)));
        return StyleValueFFI::rust_style_value_create_position(
            StyleValueFFI::rust_style_value_retain(edge_x->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(edge_y->rust_style_value_data()));
    }();

    auto position = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(position->is_position());
    EXPECT_EQ(position->to_string(SerializationMode::Normal), "4px 8px"sv);
}

TEST_CASE(rust_interpolates_and_composites_positions)
{
    auto make_position = [](double x, double y) {
        return PositionStyleValue::create(
            EdgeStyleValue::create({}, LengthStyleValue::create(Length::make_px(x))),
            EdgeStyleValue::create({}, LengthStyleValue::create(Length::make_px(y))));
    };
    auto from = make_position(4, 8);
    auto to = make_position(8, 16);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Left),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "6px 12px"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "12px 24px"sv);
}

TEST_CASE(rust_rect_handles_retain_edge_data)
{
    auto data = [] {
        auto top = LengthStyleValue::create(Length::make_px(1));
        auto right = LengthStyleValue::create(Length::make_px(2));
        auto bottom = LengthStyleValue::create(Length::make_px(3));
        auto left = LengthStyleValue::create(Length::make_px(4));
        return StyleValueFFI::rust_style_value_create_rect(
            StyleValueFFI::rust_style_value_retain(top->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(right->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(bottom->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(left->rust_style_value_data()));
    }();

    auto rect = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(rect->is_rect());
    EXPECT_EQ(rect->to_string(SerializationMode::Normal), "rect(1px, 2px, 3px, 4px)"sv);
}

TEST_CASE(rust_interpolates_and_composites_rects)
{
    auto make_rect = [](double scale) {
        auto top = LengthStyleValue::create(Length::make_px(scale));
        auto right = LengthStyleValue::create(Length::make_px(2 * scale));
        auto bottom = LengthStyleValue::create(Length::make_px(3 * scale));
        auto left = LengthStyleValue::create(Length::make_px(4 * scale));
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_rect(
            StyleValueFFI::rust_style_value_retain(top->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(right->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(bottom->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(left->rust_style_value_data())));
    };
    auto from = make_rect(1);
    auto to = make_rect(3);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Clip),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "rect(2px, 4px, 6px, 8px)"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "rect(4px, 8px, 12px, 16px)"sv);
}

TEST_CASE(rust_border_radius_handles_retain_radius_data)
{
    auto data = [] {
        auto horizontal = LengthStyleValue::create(Length::make_px(4));
        auto vertical = LengthStyleValue::create(Length::make_px(8));
        return StyleValueFFI::rust_style_value_create_border_radius(
            true,
            StyleValueFFI::rust_style_value_retain(horizontal->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(vertical->rust_style_value_data()));
    }();

    auto radius = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(radius->is_border_radius());
    EXPECT_EQ(radius->to_string(SerializationMode::Normal), "4px 8px"sv);
}

TEST_CASE(rust_border_image_slice_handles_retain_offset_data)
{
    auto data = [] {
        auto top = NumberStyleValue::create(1);
        auto right = NumberStyleValue::create(2);
        auto bottom = NumberStyleValue::create(3);
        auto left = NumberStyleValue::create(4);
        return StyleValueFFI::rust_style_value_create_border_image_slice(
            StyleValueFFI::rust_style_value_retain(top->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(right->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(bottom->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(left->rust_style_value_data()),
            true);
    }();

    auto slice = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(slice->is_border_image_slice());
    auto top = slice->as_border_image_slice().top();
    slice = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(top->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_interpolates_and_composites_border_image_slices)
{
    auto make_slice = [](double scale, bool fill = true) {
        return BorderImageSliceStyleValue::create(
            NumberStyleValue::create(scale),
            NumberStyleValue::create(2 * scale),
            NumberStyleValue::create(3 * scale),
            NumberStyleValue::create(4 * scale),
            fill);
    };
    auto from = make_slice(1);
    auto to = make_slice(3);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::BorderImageSlice),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "2 4 6 8 fill"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "4 8 12 16 fill"sv);

    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::BorderImageSlice),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        -1.0f);
    EXPECT(result.handled);
    interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "0 fill"sv);

    auto without_fill = make_slice(3, false);
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::BorderImageSlice),
        from->rust_style_value_data(),
        without_fill->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);
}

TEST_CASE(rust_border_radius_rect_handles_retain_corner_data)
{
    auto data = [] {
        auto top_left = BorderRadiusStyleValue::create(LengthStyleValue::create(Length::make_px(1)), LengthStyleValue::create(Length::make_px(2)));
        auto top_right = BorderRadiusStyleValue::create(LengthStyleValue::create(Length::make_px(3)), LengthStyleValue::create(Length::make_px(4)));
        auto bottom_right = BorderRadiusStyleValue::create(LengthStyleValue::create(Length::make_px(5)), LengthStyleValue::create(Length::make_px(6)));
        auto bottom_left = BorderRadiusStyleValue::create(LengthStyleValue::create(Length::make_px(7)), LengthStyleValue::create(Length::make_px(8)));
        return StyleValueFFI::rust_style_value_create_border_radius_rect(
            StyleValueFFI::rust_style_value_retain(top_left->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(top_right->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(bottom_right->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(bottom_left->rust_style_value_data()));
    }();

    auto rect = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(rect->is_border_radius_rect());
    auto top_left = rect->as_border_radius_rect().top_left();
    rect = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(top_left->to_string(SerializationMode::Normal), "1px 2px"sv);
}

TEST_CASE(rust_interpolates_and_composites_border_radius_rects)
{
    auto make_corner = [](double horizontal, double vertical) {
        return BorderRadiusStyleValue::create(
            LengthStyleValue::create(Length::make_px(horizontal)),
            LengthStyleValue::create(Length::make_px(vertical)));
    };
    auto make_rect = [&](double offset) {
        return BorderRadiusRectStyleValue::create(
            make_corner(1 + offset, 2 + offset),
            make_corner(2 + offset, 3 + offset),
            make_corner(3 + offset, 4 + offset),
            make_corner(4 + offset, 5 + offset));
    };
    auto from = make_rect(0);
    auto to = make_rect(2);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::MarginTop),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "2px 3px 4px 5px / 3px 4px 5px 6px"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "4px 6px 8px 10px / 6px 8px 10px 12px"sv);

    from = BorderRadiusRectStyleValue::create(make_corner(1, 1), make_corner(1, 1), make_corner(1, 1), make_corner(1, 1));
    to = BorderRadiusRectStyleValue::create(make_corner(3, 3), make_corner(3, 3), make_corner(3, 3), make_corner(3, 3));
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::MarginTop),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        -1.0f);
    EXPECT(result.handled);
    interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "0px"sv);
}

TEST_CASE(rust_interpolates_and_composites_border_radii)
{
    auto make_radius = [](double horizontal, double vertical) {
        auto horizontal_radius = LengthStyleValue::create(Length::make_px(horizontal));
        auto vertical_radius = LengthStyleValue::create(Length::make_px(vertical));
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_border_radius(
            horizontal != vertical,
            StyleValueFFI::rust_style_value_retain(horizontal_radius->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(vertical_radius->rust_style_value_data())));
    };
    auto from = make_radius(2, 4);
    auto to = make_radius(6, 8);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::BorderTopLeftRadius),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "4px 6px"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "8px 12px"sv);
}

TEST_CASE(rust_interpolates_and_composites_background_sizes)
{
    auto make_background_size = [](double x, double y) {
        auto size_x = LengthStyleValue::create(Length::make_px(x));
        auto size_y = LengthStyleValue::create(Length::make_px(y));
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_background_size(
            StyleValueFFI::rust_style_value_retain(size_x->rust_style_value_data()),
            StyleValueFFI::rust_style_value_retain(size_y->rust_style_value_data())));
    };
    auto from = StyleValueList::create({ make_background_size(4, 8) }, StyleValueList::Separator::Comma);
    auto to = StyleValueList::create({ make_background_size(8, 16) }, StyleValueList::Separator::Comma);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::BackgroundSize),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "6px 12px"sv);

    result = StyleValueFFI::rust_composite_scalar_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "12px 24px"sv);
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
