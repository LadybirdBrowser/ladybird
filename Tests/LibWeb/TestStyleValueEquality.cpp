/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/WindingRule.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusRectStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContrastColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LightDarkStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/OverflowClipMarginStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextIndentStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>

// These tests build separately-allocated style values with equal (or deliberately unequal)
// contents and check that StyleValue::equals() compares by value, not by pointer identity.
// Style value equality feeds restyle invalidation and transition change detection, so drift
// here does not show up in rendering tests until it causes stale styles or spurious
// transitions.

namespace Web::CSS {

static StyleValueFFI::StyleValueData const* create_test_image(StringView url)
{
    auto url_string = MUST(String::from_utf8(url));
    auto url_bytes = url_string.bytes();
    return StyleValueFFI::rust_style_value_create_image(
        url_string.to_raw_leaked(), url_bytes.data(), url_bytes.size(), 0, nullptr, 0,
        0, nullptr, 0, false, false, false, false);
}

TEST_CASE(rust_serialization_transfers_a_native_utf16_string)
{
    auto value = StringStyleValue::create(Utf16FlyString::from_utf8("hello 😀"sv));
    auto text = StyleValueFFI::rust_style_value_serialize(
        value->rust_style_value_data(), to_underlying(SerializationMode::Normal));

    EXPECT(text.has_value);
    auto serialized = Utf16String::adopt_raw(text.raw);
    EXPECT_EQ(serialized, u"\"hello 😀\""sv);
    EXPECT(!serialized.has_ascii_storage());

    auto ascii_value = StringStyleValue::create(Utf16FlyString::from_utf8("abc"sv));
    auto ascii_text = StyleValueFFI::rust_style_value_serialize(
        ascii_value->rust_style_value_data(), to_underlying(SerializationMode::Normal));

    EXPECT(ascii_text.has_value);
    auto ascii_serialized = Utf16String::adopt_raw(ascii_text.raw);
    EXPECT_EQ(ascii_serialized, u"\"abc\""sv);
    EXPECT(ascii_serialized.has_ascii_storage());
}

TEST_CASE(rust_composites_scalar_style_values)
{
    auto underlying_number = NumberStyleValue::create(2);
    auto animated_number = NumberStyleValue::create(3);
    auto result = StyleValueFFI::rust_test_composite_style_value(
        underlying_number->rust_style_value_data(),
        animated_number->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto number = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(number->as_number().number(), 5);

    auto underlying_length = LengthStyleValue::create(Length::make_px(10));
    auto animated_length = LengthStyleValue::create(Length::make_px(15));
    result = StyleValueFFI::rust_test_composite_style_value(
        underlying_length->rust_style_value_data(),
        animated_length->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Accumulate);
    EXPECT(result.handled);
    auto length = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(length->as_length().length().raw_value(), 25);

    result = StyleValueFFI::rust_test_composite_style_value(
        underlying_number->rust_style_value_data(),
        animated_number->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Replace);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    auto underlying_ratio = RatioStyleValue::create(NumberStyleValue::create(4), NumberStyleValue::create(3));
    auto animated_ratio = RatioStyleValue::create(NumberStyleValue::create(16), NumberStyleValue::create(9));
    result = StyleValueFFI::rust_test_composite_style_value(
        underlying_ratio->rust_style_value_data(),
        animated_ratio->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    auto underlying_opacity = OpacityValueStyleValue::create(NumberStyleValue::create(0.75));
    auto animated_opacity = OpacityValueStyleValue::create(NumberStyleValue::create(0.75));
    result = StyleValueFFI::rust_test_composite_style_value(
        underlying_opacity->rust_style_value_data(),
        animated_opacity->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto opacity = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(opacity->as_opacity_value().resolved(), 1);
}

TEST_CASE(rust_unresolved_value_retains_cached_parsed_value)
{
    RefPtr<StyleValue const> parsed_value = NumberStyleValue::create(42);
    auto unresolved_value = UnresolvedStyleValue::create_attr_tainted_with_parsed_value(
        {}, {}, {}, UnresolvedStyleValue::SourceTextMode::Trim, parsed_value.release_nonnull());

    auto restored_parsed_value = unresolved_value->parsed_value();
    EXPECT(restored_parsed_value);
    EXPECT_EQ(restored_parsed_value->as_number().number(), 42);
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
    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_custom_ident(Utf16FlyString::from_utf8("foo"sv).to_raw_leaked()))->is_custom_ident());
    EXPECT(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_counter_style(false, Utf16FlyString::from_utf8("decimal"sv).to_raw_leaked(), 0, nullptr, 0))->is_counter_style());
}

TEST_CASE(rust_style_value_handles_outlive_original_shells)
{
    auto handle = [] {
        auto original = NumberStyleValue::create(42);
        return RustStyleValueHandle { StyleValueFFI::rust_style_value_retain(original->rust_style_value_data()) };
    }();
    auto copied_handle = handle;

    auto wrapper = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(copied_handle.data()));
    EXPECT(wrapper->is_number());
    EXPECT_EQ(wrapper->as_number().number(), 42);
    EXPECT_EQ(handle, copied_handle);
}

TEST_CASE(rust_handles_create_every_remaining_typed_wrapper)
{
    auto expect_type = [](StyleValueFFI::StyleValueData const* data, StyleValue::Type type) {
        EXPECT_EQ(StyleValue::adopt_rust_style_value_data(data)->type(), type);
    };

    expect_type(StyleValueFFI::rust_style_value_create_color_scheme(nullptr, nullptr, 0, false), StyleValue::Type::ColorScheme);
    expect_type(StyleValueFFI::rust_style_value_create_display(0), StyleValue::Type::Display);
    expect_type(StyleValueFFI::rust_style_value_create_empty_optional(), StyleValue::Type::EmptyOptional);
    expect_type(StyleValueFFI::rust_style_value_create_grid_auto_flow(true, false), StyleValue::Type::GridAutoFlow);
    expect_type(StyleValueFFI::rust_style_value_create_grid_template_area(nullptr, 0, 0, 0), StyleValue::Type::GridTemplateArea);
    expect_type(StyleValueFFI::rust_style_value_create_guaranteed_invalid(), StyleValue::Type::GuaranteedInvalid);
    expect_type(StyleValueFFI::rust_style_value_create_repeat_style(0, 0), StyleValue::Type::RepeatStyle);
    expect_type(StyleValueFFI::rust_style_value_create_scrollbar_gutter(0), StyleValue::Type::ScrollbarGutter);
    expect_type(StyleValueFFI::rust_style_value_create_shorthand(0, nullptr, 0, nullptr, 0), StyleValue::Type::Shorthand);
    expect_type(StyleValueFFI::rust_style_value_create_text_underline_position(0, 0), StyleValue::Type::TextUnderlinePosition);
    expect_type(StyleValueFFI::rust_style_value_create_unicode_range(0, 0), StyleValue::Type::UnicodeRange);
    auto url = MUST(String::from_utf8("https://example.com/"sv));
    auto url_bytes = url.bytes();
    expect_type(StyleValueFFI::rust_style_value_create_url(url.to_raw_leaked(), url_bytes.data(), url_bytes.size(), 0, nullptr, 0), StyleValue::Type::URL);
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

TEST_CASE(rust_shorthand_handles_retain_child_data)
{
    auto data = [] {
        auto child = NumberStyleValue::create(4);
        Array properties { to_underlying(PropertyID::MarginTop) };
        Array values { StyleValueFFI::rust_style_value_retain(child->rust_style_value_data()) };
        return StyleValueFFI::rust_style_value_create_shorthand(
            to_underlying(PropertyID::Margin), properties.data(), properties.size(), values.data(), values.size());
    }();

    auto child = [&] {
        auto shorthand = StyleValue::adopt_rust_style_value_data(data);
        EXPECT(shorthand->is_shorthand());
        auto values = shorthand->as_shorthand().values();
        EXPECT_EQ(values.size(), 1u);
        return values[0];
    }();

    EXPECT(child->is_number());
    EXPECT_EQ(child->as_number().number(), 4);
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

TEST_CASE(rust_open_type_tagged_handles_retain_setting_data)
{
    auto data = [] {
        auto value = NumberStyleValue::create(400);
        auto setting = OpenTypeTaggedStyleValue::create(
            OpenTypeTaggedStyleValue::Mode::FontVariationSettings,
            Utf16FlyString::from_utf8("wght"sv), value);
        return StyleValueFFI::rust_style_value_retain(setting->rust_style_value_data());
    }();

    auto setting = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(setting->is_open_type_tagged());
    auto value = setting->as_open_type_tagged().value();
    setting = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(value->to_string(SerializationMode::Normal), "400"sv);
}

TEST_CASE(rust_function_handles_retain_argument_data)
{
    auto data = [] {
        auto value = NumberStyleValue::create(2);
        return StyleValueFFI::rust_style_value_create_function(
            Utf16FlyString::from_utf8("foo"sv).to_raw_leaked(),
            StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
    }();

    auto function = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(function->is_function());
    auto value = function->as_function().value();
    function = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(value->to_string(SerializationMode::Normal), "2"sv);
}

TEST_CASE(rust_font_style_handles_retain_angle_data)
{
    auto data = [] {
        auto angle = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(20, 0));
        return StyleValueFFI::rust_style_value_create_font_style(
            to_underlying(FontStyleKeyword::Oblique),
            StyleValueFFI::rust_style_value_retain(angle->rust_style_value_data()));
    }();

    auto font_style = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(font_style->is_font_style());
    auto angle = font_style->as_font_style().angle();
    font_style = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(angle->to_string(SerializationMode::Normal), "20deg"sv);
}

TEST_CASE(rust_overflow_clip_margin_handles_retain_offset_data)
{
    auto data = StyleValueFFI::rust_style_value_create_overflow_clip_margin(
        false,
        0,
        StyleValueFFI::rust_style_value_create_length(10, to_underlying(LengthUnit::Px)));

    auto clip_margin = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(clip_margin->is_overflow_clip_margin());
    NonnullRefPtr<StyleValue const> offset { clip_margin->as_overflow_clip_margin().offset() };
    clip_margin = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(offset->to_string(SerializationMode::Normal), "10px"sv);
}

TEST_CASE(rust_grid_track_placement_handles_retain_line_data)
{
    auto data = StyleValueFFI::rust_style_value_create_grid_track_placement(
        2,
        StyleValueFFI::rust_style_value_create_integer(3),
        false,
        0,
        0,
        0);

    auto placement = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(placement->is_grid_track_placement());
    auto line = placement->as_grid_track_placement().grid_track_placement().line_number();
    placement = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(line->to_string(SerializationMode::Normal), "3"sv);
}

TEST_CASE(rust_counter_handles_retain_counter_style_data)
{
    auto data = StyleValueFFI::rust_style_value_create_counter(
        to_underlying(CounterStyleValue::CounterFunction::Counter),
        Utf16FlyString::from_utf8("example"sv).to_raw_leaked(),
        StyleValueFFI::rust_style_value_create_counter_style(
            false,
            Utf16FlyString::from_utf8("decimal"sv).to_raw_leaked(),
            0,
            nullptr,
            0),
        Utf16FlyString::from_utf8(""sv).to_raw_leaked());

    auto counter = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(counter->is_counter());
    auto counter_style = counter->as_counter().counter_style();
    counter = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(counter_style->to_string(SerializationMode::Normal), "decimal"sv);
}

TEST_CASE(rust_light_dark_handles_retain_color_data)
{
    auto light = NumberStyleValue::create(1);
    auto dark = NumberStyleValue::create(2);
    auto data = StyleValueFFI::rust_style_value_create_light_dark(
        false,
        0,
        to_underlying(ColorSyntax::Modern),
        StyleValueFFI::rust_style_value_retain(light->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(dark->rust_style_value_data()));

    light = NumberStyleValue::create(3);
    dark = NumberStyleValue::create(4);
    auto light_dark = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(light_dark->is_color());
    EXPECT_EQ(light_dark->to_string(SerializationMode::Normal), "light-dark(1, 2)"sv);
}

TEST_CASE(rust_scrollbar_color_handles_retain_color_data)
{
    auto thumb = NumberStyleValue::create(1);
    auto track = NumberStyleValue::create(2);
    auto data = StyleValueFFI::rust_style_value_create_scrollbar_color(
        StyleValueFFI::rust_style_value_retain(thumb->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(track->rust_style_value_data()));

    thumb = NumberStyleValue::create(3);
    track = NumberStyleValue::create(4);
    auto scrollbar_color = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(scrollbar_color->is_scrollbar_color());
    auto retained_thumb = scrollbar_color->as_scrollbar_color().thumb_color();
    scrollbar_color = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_thumb->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_contrast_color_handles_retain_color_data)
{
    auto color = NumberStyleValue::create(1);
    auto data = StyleValueFFI::rust_style_value_create_contrast_color(
        false,
        0,
        to_underlying(ColorSyntax::Modern),
        StyleValueFFI::rust_style_value_retain(color->rust_style_value_data()));

    color = NumberStyleValue::create(2);
    auto contrast_color = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(contrast_color->is_color());
    EXPECT_EQ(contrast_color->to_string(SerializationMode::Normal), "contrast-color(1)"sv);
}

TEST_CASE(rust_random_value_sharing_handles_retain_fixed_data)
{
    auto fixed_value = NumberStyleValue::create(1);
    auto data = StyleValueFFI::rust_style_value_create_random_value_sharing(
        StyleValueFFI::rust_style_value_retain(fixed_value->rust_style_value_data()),
        false,
        false,
        0,
        false);

    fixed_value = NumberStyleValue::create(2);
    auto sharing = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(sharing->is_random_value_sharing());
    EXPECT_EQ(sharing->to_string(SerializationMode::Normal), "fixed 1"sv);
}

TEST_CASE(rust_anchor_size_handles_retain_fallback_data)
{
    auto fallback = NumberStyleValue::create(1);
    auto data = StyleValueFFI::rust_style_value_create_anchor_size(
        false,
        0,
        false,
        0,
        StyleValueFFI::rust_style_value_retain(fallback->rust_style_value_data()));

    fallback = NumberStyleValue::create(2);
    auto anchor_size = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(anchor_size->is_anchor_size());
    auto retained_fallback = anchor_size->as_anchor_size().fallback_value();
    anchor_size = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_fallback->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_anchor_handles_retain_side_and_fallback_data)
{
    auto side = NumberStyleValue::create(1);
    auto fallback = NumberStyleValue::create(2);
    auto data = StyleValueFFI::rust_style_value_create_anchor(
        false,
        0,
        StyleValueFFI::rust_style_value_retain(side->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(fallback->rust_style_value_data()));

    side = NumberStyleValue::create(3);
    fallback = NumberStyleValue::create(4);
    auto anchor = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(anchor->is_anchor());
    auto retained_side = anchor->as_anchor().anchor_side();
    anchor = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_side->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_content_handles_retain_list_data)
{
    auto content = StyleValueList::create({ NumberStyleValue::create(1) }, StyleValueList::Separator::Space);
    auto alt_text = StyleValueList::create({ NumberStyleValue::create(2) }, StyleValueList::Separator::Space);
    auto data = StyleValueFFI::rust_style_value_create_content(
        StyleValueFFI::rust_style_value_retain(content->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(alt_text->rust_style_value_data()));

    content = StyleValueList::create({ NumberStyleValue::create(3) }, StyleValueList::Separator::Space);
    alt_text = StyleValueList::create({ NumberStyleValue::create(4) }, StyleValueList::Separator::Space);
    auto content_value = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(content_value->is_content());
    EXPECT_EQ(content_value->to_string(SerializationMode::Normal), "1 / 2"sv);
}

TEST_CASE(rust_counter_style_system_handles_retain_first_symbol_data)
{
    auto first_symbol = NumberStyleValue::create(1);
    auto data = StyleValueFFI::rust_style_value_create_counter_style_system(
        1,
        0,
        StyleValueFFI::rust_style_value_retain(first_symbol->rust_style_value_data()),
        0);

    first_symbol = NumberStyleValue::create(2);
    auto system = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(system->is_counter_style_system());
    auto retained_symbol = system->as_counter_style_system().value().get<CounterStyleSystemStyleValue::Fixed>().first_symbol;
    system = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_symbol->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_font_source_handles_retain_local_name_data)
{
    auto local_name = StringStyleValue::create("Font Name"_utf16_fly_string);
    auto data = StyleValueFFI::rust_style_value_create_font_source(
        true,
        StyleValueFFI::rust_style_value_retain(local_name->rust_style_value_data()),
        0,
        nullptr,
        0,
        0,
        nullptr,
        0,
        false,
        0,
        nullptr,
        0);

    local_name = StringStyleValue::create("Other Font"_utf16_fly_string);
    auto source = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(source->is_font_source());
    EXPECT_EQ(source->to_string(SerializationMode::Normal), "local(\"Font Name\")"sv);
}

TEST_CASE(rust_pending_substitution_handles_retain_shorthand_data)
{
    auto shorthand = NumberStyleValue::create(1);
    auto data = StyleValueFFI::rust_style_value_create_pending_substitution(
        StyleValueFFI::rust_style_value_retain(shorthand->rust_style_value_data()));

    shorthand = NumberStyleValue::create(2);
    auto pending = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(pending->is_pending_substitution());
    RefPtr<StyleValue const> retained_shorthand = pending->as_pending_substitution().original_shorthand_value();
    pending = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_shorthand->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_radial_size_handles_retain_component_data)
{
    auto horizontal = LengthStyleValue::create(Length::make_px(10));
    auto vertical = LengthStyleValue::create(Length::make_px(20));
    auto data = StyleValueFFI::rust_style_value_create_radial_size(
        2,
        false,
        0,
        StyleValueFFI::rust_style_value_retain(horizontal->rust_style_value_data()),
        false,
        0,
        StyleValueFFI::rust_style_value_retain(vertical->rust_style_value_data()));

    horizontal = LengthStyleValue::create(Length::make_px(30));
    vertical = LengthStyleValue::create(Length::make_px(40));
    auto size = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(size->is_radial_size());
    auto components = size->as_radial_size().components();
    size = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(components[0].get<NonnullRefPtr<StyleValue const>>()->to_string(SerializationMode::Normal), "10px"sv);
    EXPECT_EQ(components[1].get<NonnullRefPtr<StyleValue const>>()->to_string(SerializationMode::Normal), "20px"sv);
}

TEST_CASE(rust_shadow_handles_retain_child_data)
{
    auto offset_x = LengthStyleValue::create(Length::make_px(1));
    auto offset_y = LengthStyleValue::create(Length::make_px(2));
    auto blur_radius = LengthStyleValue::create(Length::make_px(3));
    auto spread_distance = LengthStyleValue::create(Length::make_px(4));
    auto data = StyleValueFFI::rust_style_value_create_shadow(
        to_underlying(ShadowStyleValue::ShadowType::Normal),
        nullptr,
        StyleValueFFI::rust_style_value_retain(offset_x->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(offset_y->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(blur_radius->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(spread_distance->rust_style_value_data()),
        to_underlying(ShadowPlacement::Outer));

    offset_x = LengthStyleValue::create(Length::make_px(5));
    offset_y = LengthStyleValue::create(Length::make_px(6));
    blur_radius = LengthStyleValue::create(Length::make_px(7));
    spread_distance = LengthStyleValue::create(Length::make_px(8));
    auto shadow = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(shadow->is_shadow());
    auto retained_offset_x = shadow->as_shadow().offset_x();
    auto retained_offset_y = shadow->as_shadow().offset_y();
    auto retained_blur_radius = shadow->as_shadow().blur_radius_or_null();
    auto retained_spread_distance = shadow->as_shadow().spread_distance_or_null();
    shadow = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_offset_x->to_string(SerializationMode::Normal), "1px"sv);
    EXPECT_EQ(retained_offset_y->to_string(SerializationMode::Normal), "2px"sv);
    EXPECT_EQ(retained_blur_radius->to_string(SerializationMode::Normal), "3px"sv);
    EXPECT_EQ(retained_spread_distance->to_string(SerializationMode::Normal), "4px"sv);
}

TEST_CASE(rust_filter_handles_retain_child_data)
{
    auto shadow = ShadowStyleValue::create(
        ShadowStyleValue::ShadowType::Text,
        nullptr,
        LengthStyleValue::create(Length::make_px(1)),
        LengthStyleValue::create(Length::make_px(2)),
        LengthStyleValue::create(Length::make_px(3)),
        nullptr,
        ShadowPlacement::Outer);
    auto data = StyleValueFFI::rust_style_value_create_filter(
        to_underlying(FilterStyleValue::Kind::DropShadow),
        0,
        StyleValueFFI::rust_style_value_retain(shadow->rust_style_value_data()));

    shadow = ShadowStyleValue::create(
        ShadowStyleValue::ShadowType::Text,
        nullptr,
        LengthStyleValue::create(Length::make_px(4)),
        LengthStyleValue::create(Length::make_px(5)),
        LengthStyleValue::create(Length::make_px(6)),
        nullptr,
        ShadowPlacement::Outer);
    auto filter = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(filter->is_filter());
    auto retained_shadow = static_cast<DropShadowFilterStyleValue const&>(filter->as_filter()).shadow_style_value();
    filter = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_shadow->to_string(SerializationMode::Normal), "1px 2px 3px"sv);
}

TEST_CASE(rust_calculated_handles_create_typed_wrappers)
{
    StyleValueFFI::FfiNumericType resolved_type {};
    resolved_type.valid = true;
    resolved_type.has_exponent[to_underlying(NumericType::BaseType::Length)] = true;
    resolved_type.exponents[to_underlying(NumericType::BaseType::Length)] = 1;
    auto data = StyleValueFFI::rust_style_value_create_calculated(
        StyleValueFFI::rust_calc_node_create_numeric_dimension(4, 10, to_underlying(LengthUnit::Px)),
        resolved_type,
        false,
        false,
        0,
        0,
        false,
        nullptr,
        0);

    auto calculated = StyleValue::adopt_rust_style_value_data(data);
    auto retained_data = StyleValueFFI::rust_style_value_retain(calculated->rust_style_value_data());
    calculated = KeywordStyleValue::create(Keyword::None);

    auto retained_calculated = StyleValue::adopt_rust_style_value_data(retained_data);
    EXPECT(retained_calculated->is_calculated());
    EXPECT_EQ(retained_calculated->to_string(SerializationMode::Normal), "calc(10px)"sv);
}

TEST_CASE(rust_custom_property_stores_retain_value_data)
{
    auto name = Utf16FlyString::from_utf8("--value"sv);
    auto name_view = name.view();
    auto name_raw = name.to_raw_leaked();
    ComputedValuesFFI::FfiCustomPropertyStoreEntry entry {
        .name_raw = name_raw,
        .name = {
            .ascii = name_view.has_ascii_storage() ? reinterpret_cast<u8 const*>(name_view.ascii_span().data()) : nullptr,
            .utf16 = name_view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(name_view.utf16_span().data()),
            .length = name_view.length_in_code_units(),
        },
        .important = false,
        .data = StyleValueFFI::rust_style_value_create_number(1),
    };
    auto const* store = ComputedValuesFFI::rust_custom_property_store_create(&entry, 1, nullptr, nullptr);
    auto result = ComputedValuesFFI::rust_custom_property_store_get(store, name_raw);
    EXPECT(result.found);
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
        static_cast<StyleValueFFI::StyleValueData const*>(result.data)));
    ComputedValuesFFI::rust_custom_property_store_destroy(store);
    EXPECT_EQ(value->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_calculation_nodes_retain_style_value_data)
{
    auto random = StyleValueFFI::rust_calc_node_create_random(
        StyleValueFFI::rust_calc_node_create_numeric_dimension(0, 0, 0),
        StyleValueFFI::rust_calc_node_create_numeric_dimension(0, 1, 0),
        nullptr,
        StyleValueFFI::rust_style_value_create_random_value_sharing(
            StyleValueFFI::rust_style_value_create_number(0.25), false, false, 0, false));
    auto const* sharing_data = static_cast<StyleValueFFI::StyleValueData const*>(
        StyleValueFFI::rust_calc_node_style_value(random));
    auto sharing = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(sharing_data));
    StyleValueFFI::rust_calc_node_release(random);
    EXPECT(sharing->is_random_value_sharing());
    EXPECT_EQ(sharing->to_string(SerializationMode::Normal), "fixed 0.25"sv);

    StyleValueFFI::FfiNumericType numeric_type {};
    auto function = StyleValueFFI::rust_calc_node_create_non_math_function(
        StyleValueFFI::rust_style_value_create_tree_counting_function(0, 0), &numeric_type);
    auto const* function_data = static_cast<StyleValueFFI::StyleValueData const*>(
        StyleValueFFI::rust_calc_node_style_value(function));
    auto tree_counting_function = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(function_data));
    StyleValueFFI::rust_calc_node_release(function);
    EXPECT(tree_counting_function->is_tree_counting_function());
    EXPECT_EQ(tree_counting_function->to_string(SerializationMode::Normal), "sibling-count()"sv);
}

TEST_CASE(rust_basic_shape_handles_retain_polygon_point_data)
{
    auto x = LengthStyleValue::create(Length::make_px(1));
    auto y = LengthStyleValue::create(Length::make_px(2));
    StyleValueFFI::RetainedShapePoint point {
        { StyleValueFFI::rust_style_value_retain(x->rust_style_value_data()) },
        { StyleValueFFI::rust_style_value_retain(y->rust_style_value_data()) },
    };
    auto data = StyleValueFFI::rust_style_value_create_basic_shape(5, nullptr, nullptr, nullptr, nullptr, nullptr, to_underlying(Gfx::WindingRule::Nonzero), &point, 1, 0);

    x = LengthStyleValue::create(Length::make_px(3));
    y = LengthStyleValue::create(Length::make_px(4));
    auto shape = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(shape->is_basic_shape());
    auto const& retained_point = shape->rust_style_value_data()->basic_shape.points.pointer[0];
    auto point_x = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(retained_point.x.pointer)));
    auto point_y = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(retained_point.y.pointer)));
    shape = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(point_x->to_string(SerializationMode::Normal), "1px"sv);
    EXPECT_EQ(point_y->to_string(SerializationMode::Normal), "2px"sv);
}

TEST_CASE(rust_counter_definition_handles_retain_value_data)
{
    auto value = IntegerStyleValue::create(2);
    StyleValueFFI::RetainedCounterDefinition definition {
        { Utf16FlyString::from_utf8("item"sv).to_raw_leaked() },
        false,
        { StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) },
    };
    auto data = StyleValueFFI::rust_style_value_create_counter_definitions(&definition, 1);

    value = IntegerStyleValue::create(3);
    auto definitions = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(definitions->is_counter_definitions());
    auto retained_value = definitions->as_counter_definitions().counter_definitions()[0].value;
    definitions = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_value->to_string(SerializationMode::Normal), "2"sv);
}

TEST_CASE(rust_easing_handles_retain_linear_stop_data)
{
    auto output = NumberStyleValue::create(1);
    auto input = PercentageStyleValue::create(Percentage { 50 });
    StyleValueFFI::RetainedLinearEasingStop stop {
        { StyleValueFFI::rust_style_value_retain(output->rust_style_value_data()) },
        { StyleValueFFI::rust_style_value_retain(input->rust_style_value_data()) },
    };
    auto data = StyleValueFFI::rust_style_value_create_easing(0, &stop, 1, nullptr, nullptr, nullptr, nullptr, nullptr, 0);

    output = NumberStyleValue::create(2);
    input = PercentageStyleValue::create(Percentage { 75 });
    auto easing = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(easing->is_easing());
    EXPECT_EQ(easing->to_string(SerializationMode::Normal), "linear(1 50%)"sv);
    auto const& retained_stop = easing->rust_style_value_data()->easing.linear_stops.pointer[0];
    auto retained_output = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(retained_stop.output.pointer)));
    easing = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_output->to_string(SerializationMode::Normal), "1"sv);
}

TEST_CASE(rust_color_function_handles_retain_channel_data)
{
    auto channel_0 = NumberStyleValue::create(0.1);
    auto channel_1 = NumberStyleValue::create(0.2);
    auto channel_2 = NumberStyleValue::create(0.3);
    auto data = StyleValueFFI::rust_style_value_create_color_function(
        true, to_underlying(ColorStyleValue::ColorType::sRGB), to_underlying(ColorSyntax::Modern),
        StyleValueFFI::rust_style_value_retain(channel_0->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(channel_1->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(channel_2->rust_style_value_data()),
        nullptr, false, 0, nullptr);

    channel_0 = NumberStyleValue::create(0.4);
    channel_1 = NumberStyleValue::create(0.5);
    channel_2 = NumberStyleValue::create(0.6);
    auto color = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(color->is_color_function());
    auto const* retained_channel_data = static_cast<StyleValueFFI::StyleValueData const*>(
        color->rust_style_value_data()->color_function.channel_0.pointer);
    auto retained_channel = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(retained_channel_data));
    color = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_channel->to_string(SerializationMode::Normal), "0.1"sv);
}

TEST_CASE(rust_color_mix_handles_retain_component_data)
{
    auto make_color = [](double red) {
        return ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::sRGB,
            NumberStyleValue::create(red), NumberStyleValue::create(0), NumberStyleValue::create(0));
    };
    auto first = make_color(0.1);
    auto second = make_color(0.2);
    auto data = StyleValueFFI::rust_style_value_create_color_mix(
        false, 0, to_underlying(ColorSyntax::Modern), nullptr,
        StyleValueFFI::rust_style_value_retain(first->rust_style_value_data()), nullptr,
        StyleValueFFI::rust_style_value_retain(second->rust_style_value_data()), nullptr);

    first = make_color(0.3);
    second = make_color(0.4);
    auto color_mix = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(color_mix->is_color());
    EXPECT_EQ(color_mix->rust_style_value_data()->tag, StyleValueFFI::StyleValueData::Tag::ColorMix);
    auto const* retained_color_data = static_cast<StyleValueFFI::StyleValueData const*>(
        color_mix->rust_style_value_data()->color_mix.first_color.pointer);
    auto retained_color = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(retained_color_data));
    color_mix = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_color->to_string(SerializationMode::Normal), "color(srgb 0.1 0 0)"sv);
}

TEST_CASE(rust_gradient_color_stop_handles_retain_data)
{
    auto color = ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::sRGB,
        NumberStyleValue::create(0.1), NumberStyleValue::create(0.2), NumberStyleValue::create(0.3));
    StyleValueFFI::RetainedColorStop stop {
        { nullptr },
        { StyleValueFFI::rust_style_value_retain(color->rust_style_value_data()) },
        { nullptr },
        { nullptr },
    };
    auto gradient_data = StyleValueFFI::rust_style_value_create_linear_gradient(
        false, nullptr, 0, &stop, 1, 0, false, nullptr, to_underlying(ColorSyntax::Modern));
    color = ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::sRGB,
        NumberStyleValue::create(0.4), NumberStyleValue::create(0.5), NumberStyleValue::create(0.6));
    auto const* color_data = static_cast<StyleValueFFI::StyleValueData const*>(
        gradient_data->linear_gradient.color_stop_list.pointer[0].color.pointer);
    auto retained_color = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(color_data));
    StyleValueFFI::rust_style_value_release(gradient_data);
    EXPECT_EQ(retained_color->to_string(SerializationMode::Normal), "color(srgb 0.1 0.2 0.3)"sv);
}

TEST_CASE(rust_linear_gradient_handles_retain_direction_data)
{
    auto direction = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_create_angle(45, to_underlying(AngleUnit::Deg)));
    auto color = ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::sRGB,
        NumberStyleValue::create(0.1), NumberStyleValue::create(0.2), NumberStyleValue::create(0.3));
    StyleValueFFI::RetainedColorStop stop {
        { nullptr },
        { StyleValueFFI::rust_style_value_retain(color->rust_style_value_data()) },
        { nullptr },
        { nullptr },
    };
    auto data = StyleValueFFI::rust_style_value_create_linear_gradient(
        true, StyleValueFFI::rust_style_value_retain(direction->rust_style_value_data()), 0,
        &stop, 1, 0, false, nullptr, to_underlying(ColorSyntax::Modern));

    direction = KeywordStyleValue::create(Keyword::None);
    auto gradient = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(gradient->is_linear_gradient());
    auto const* retained_direction_data = static_cast<StyleValueFFI::StyleValueData const*>(
        gradient->rust_style_value_data()->linear_gradient.direction_value.pointer);
    auto retained_direction = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(retained_direction_data));
    gradient = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_direction->to_string(SerializationMode::Normal), "45deg"sv);
}

TEST_CASE(rust_conic_gradient_handles_retain_position_data)
{
    ValueComparingNonnullRefPtr<StyleValue const> position = PositionStyleValue::create_center();
    auto color = ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::sRGB,
        NumberStyleValue::create(0.1), NumberStyleValue::create(0.2), NumberStyleValue::create(0.3));
    StyleValueFFI::RetainedColorStop stop {
        { nullptr },
        { StyleValueFFI::rust_style_value_retain(color->rust_style_value_data()) },
        { nullptr },
        { nullptr },
    };
    auto data = StyleValueFFI::rust_style_value_create_conic_gradient(
        nullptr, StyleValueFFI::rust_style_value_retain(position->rust_style_value_data()),
        &stop, 1, false, nullptr, to_underlying(ColorSyntax::Modern));

    position = KeywordStyleValue::create(Keyword::None);
    auto gradient = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(gradient->is_conic_gradient());
    auto const* retained_position_data = static_cast<StyleValueFFI::StyleValueData const*>(
        gradient->rust_style_value_data()->conic_gradient.position.pointer);
    auto retained_position = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(retained_position_data));
    gradient = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_position->to_string(SerializationMode::Normal), "center center"sv);
}

TEST_CASE(rust_radial_gradient_handles_retain_size_data)
{
    ValueComparingNonnullRefPtr<StyleValue const> size = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_create_radial_size(
            1, false, 0, StyleValueFFI::rust_style_value_create_length(10, to_underlying(LengthUnit::Px)),
            false, 0, nullptr));
    auto position = PositionStyleValue::create_center();
    auto color = ColorFunctionStyleValue::create(
        ColorStyleValue::ColorType::sRGB,
        NumberStyleValue::create(0.1), NumberStyleValue::create(0.2), NumberStyleValue::create(0.3));
    StyleValueFFI::RetainedColorStop stop {
        { nullptr },
        { StyleValueFFI::rust_style_value_retain(color->rust_style_value_data()) },
        { nullptr },
        { nullptr },
    };
    auto data = StyleValueFFI::rust_style_value_create_radial_gradient(
        0, StyleValueFFI::rust_style_value_retain(size->rust_style_value_data()),
        StyleValueFFI::rust_style_value_retain(position->rust_style_value_data()),
        &stop, 1, false, nullptr, to_underlying(ColorSyntax::Modern));

    size = KeywordStyleValue::create(Keyword::None);
    auto gradient = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(gradient->is_radial_gradient());
    auto const* retained_size_data = static_cast<StyleValueFFI::StyleValueData const*>(
        gradient->rust_style_value_data()->radial_gradient.size.pointer);
    auto retained_size = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(retained_size_data));
    gradient = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_size->to_string(SerializationMode::Normal), "10px"sv);
}

TEST_CASE(rust_grid_track_list_handles_retain_size_data)
{
    auto size = LengthStyleValue::create(Length::make_px(10));
    StyleValueFFI::GridTrackEntryInput entry {};
    entry.kind = StyleValueFFI::GridTrackEntryKind::Size;
    entry.size_value = StyleValueFFI::rust_style_value_retain(size->rust_style_value_data());
    auto data = StyleValueFFI::rust_style_value_create_grid_track_size_list(false, false, &entry, 1);

    size = LengthStyleValue::create(Length::make_px(20));
    auto list = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(list->is_grid_track_size_list());
    auto const* retained_size_data = static_cast<StyleValueFFI::StyleValueData const*>(
        list->rust_style_value_data()->grid_track_size_list.entries.pointer[0].size_value.pointer);
    auto retained_size = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(retained_size_data));
    list = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_size->to_string(SerializationMode::Normal), "10px"sv);
}

TEST_CASE(rust_image_handles_create_typed_wrappers)
{
    auto image = StyleValue::adopt_rust_style_value_data(
        create_test_image("image.png"sv));
    EXPECT(image->is_image());
    EXPECT_EQ(image->to_string(SerializationMode::Normal), "url(\"image.png\")"sv);
}

TEST_CASE(rust_cursor_handles_retain_image_data)
{
    auto data = StyleValueFFI::rust_style_value_create_cursor(
        create_test_image("cursor.png"sv),
        nullptr, nullptr);
    auto cursor = StyleValue::adopt_rust_style_value_data(data);
    EXPECT(cursor->is_cursor());
    auto const* retained_image_data = static_cast<StyleValueFFI::StyleValueData const*>(
        cursor->rust_style_value_data()->cursor.image.pointer);
    auto image = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_retain(retained_image_data));
    cursor = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(image->to_string(SerializationMode::Normal), "url(\"cursor.png\")"sv);
}

TEST_CASE(rust_image_set_handles_retain_option_data)
{
    StyleValueFFI::RetainedImageSetOption option {
        { create_test_image("candidate.png"sv) },
        { StyleValueFFI::rust_style_value_create_resolution(1, 0) },
        false,
        { 0 },
    };
    auto image_set = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_create_image_set(&option, 1));
    EXPECT(image_set->is_image_set());
    auto const* image_data = static_cast<StyleValueFFI::StyleValueData const*>(
        image_set->rust_style_value_data()->image_set.options.pointer[0].image.pointer);
    auto image = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(image_data));
    image_set = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(image->to_string(SerializationMode::Normal), "url(\"candidate.png\")"sv);
}

TEST_CASE(rust_interpolates_font_style_values)
{
    auto normal = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_create_font_style(to_underlying(FontStyleKeyword::Normal), nullptr));
    auto oblique = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_font_style(
        to_underlying(FontStyleKeyword::Oblique), StyleValueFFI::rust_style_value_create_angle(20, 0)));
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontStyle),
        normal->rust_style_value_data(),
        oblique->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "oblique 10deg"sv);

    auto italic = StyleValue::adopt_rust_style_value_data(
        StyleValueFFI::rust_style_value_create_font_style(to_underlying(FontStyleKeyword::Italic), nullptr));
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontStyle),
        italic->rust_style_value_data(),
        oblique->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = true,
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::FontStyle),
        italic->rust_style_value_data(),
        oblique->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "italic"sv);
}

TEST_CASE(rust_interpolates_visibility_values)
{
    auto visible = KeywordStyleValue::create(Keyword::Visible);
    auto hidden = KeywordStyleValue::create(Keyword::Hidden);
    auto collapse = KeywordStyleValue::create(Keyword::Collapse);
    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = false,
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Visibility),
        visible->rust_style_value_data(),
        hidden->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto value = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(value->to_keyword(), Keyword::Visible);

    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Visibility),
        hidden->rust_style_value_data(),
        collapse->rust_style_value_data(),
        0.75f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    context.allow_discrete = true;
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Visibility),
        hidden->rust_style_value_data(),
        collapse->rust_style_value_data(),
        0.75f);
    EXPECT(result.handled);
    value = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(value->to_keyword(), Keyword::Collapse);
}

TEST_CASE(rust_interpolates_content_visibility_values)
{
    auto visible = KeywordStyleValue::create(Keyword::Visible);
    auto hidden = KeywordStyleValue::create(Keyword::Hidden);
    auto auto_value = KeywordStyleValue::create(Keyword::Auto);
    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = false,
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::ContentVisibility),
        hidden->rust_style_value_data(),
        visible->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    context.allow_discrete = true;
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::ContentVisibility),
        hidden->rust_style_value_data(),
        visible->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto value = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(value->to_keyword(), Keyword::Visible);

    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::ContentVisibility),
        visible->rust_style_value_data(),
        auto_value->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    value = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(value->to_keyword(), Keyword::Visible);
}

TEST_CASE(rust_interpolates_display_values)
{
    Display const none_display { DisplayBox::None };
    Display const block_display { DisplayOutside::Block, DisplayInside::Flow };
    Display const inline_display { DisplayOutside::Inline, DisplayInside::Flow };
    auto none = DisplayStyleValue::create(none_display);
    auto block = DisplayStyleValue::create(block_display);
    auto inline_value = DisplayStyleValue::create(inline_display);
    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = false,
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Display),
        none->rust_style_value_data(),
        block->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);

    context.allow_discrete = true;
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Display),
        none->rust_style_value_data(),
        block->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto value = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(value->as_display().display(), block_display);

    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::Display),
        block->rust_style_value_data(),
        inline_value->rust_style_value_data(),
        0.75f);
    EXPECT(result.handled);
    value = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(value->as_display().display(), inline_display);
}

TEST_CASE(cpp_function_facades_retain_argument_wrappers)
{
    auto function = FunctionStyleValue::create(
        "foo"_utf16_fly_string,
        NumberStyleValue::create(2));
    auto value = function->value();
    function = FunctionStyleValue::create(
        "bar"_utf16_fly_string,
        NumberStyleValue::create(3));
    EXPECT_EQ(value->to_string(SerializationMode::Normal), "2"sv);
}

TEST_CASE(rust_interpolates_and_composites_function_values)
{
    auto make_function = [](StringView name, double value) {
        return FunctionStyleValue::create(
            Utf16FlyString::from_utf8(name),
            NumberStyleValue::create(value));
    };
    auto from = make_function("foo"sv, 100);
    auto to = make_function("foo"sv, 300);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontWeight),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "foo(200)"sv);

    result = StyleValueFFI::rust_test_composite_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "foo(400)"sv);

    auto mismatched_name = make_function("bar"sv, 300);
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontWeight),
        from->rust_style_value_data(),
        mismatched_name->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);
}

TEST_CASE(rust_interpolates_and_composites_open_type_settings)
{
    auto make_setting = [](StringView tag, double value) {
        return OpenTypeTaggedStyleValue::create(
            OpenTypeTaggedStyleValue::Mode::FontVariationSettings,
            Utf16FlyString::from_utf8(tag),
            NumberStyleValue::create(value));
    };
    auto from = make_setting("wght"sv, 100);
    auto to = make_setting("wght"sv, 300);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontWeight),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "\"wght\" 200"sv);

    result = StyleValueFFI::rust_test_composite_style_value(
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        StyleValueFFI::FfiCompositeOperation::Add);
    EXPECT(result.handled);
    auto composited = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(composited->to_string(SerializationMode::Normal), "\"wght\" 400"sv);

    auto mismatched_tag = make_setting("slnt"sv, 300);
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontWeight),
        from->rust_style_value_data(),
        mismatched_tag->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);
}

TEST_CASE(rust_interpolates_font_variation_setting_lists)
{
    auto make_setting = [](StringView tag, double value) {
        return OpenTypeTaggedStyleValue::create(
            OpenTypeTaggedStyleValue::Mode::FontVariationSettings,
            Utf16FlyString::from_utf8(tag),
            NumberStyleValue::create(value));
    };
    auto from = StyleValueList::create({ make_setting("wght"sv, 100) }, StyleValueList::Separator::Comma);
    auto to = StyleValueList::create({ make_setting("wght"sv, 300) }, StyleValueList::Separator::Comma);
    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontVariationSettings),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "\"wght\" 200"sv);

    auto unlike = StyleValueList::create({ make_setting("slnt"sv, 300) }, StyleValueList::Separator::Comma);
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::FontVariationSettings),
        from->rust_style_value_data(),
        unlike->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

    result = StyleValueFFI::rust_test_composite_style_value(
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

TEST_CASE(rust_extends_transform_lists_with_identity_perspective)
{
    auto from = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::ScaleZ,
            { NumberStyleValue::create(2) }) },
        StyleValueList::Separator::Space);
    auto to = StyleValueList::create(
        { TransformationStyleValue::create(
              PropertyID::Transform,
              TransformFunction::ScaleZ,
              { NumberStyleValue::create(2) }),
            TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Perspective,
                { LengthStyleValue::create(Length::make_px(500)) }) },
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
    auto perspective = transformations[1]->as_transformation().values();
    EXPECT_EQ(perspective.size(), 1u);
    EXPECT_APPROXIMATE(perspective[0]->as_length().length().raw_value(), 1000.0);
}

TEST_CASE(rust_extends_transform_lists_with_identity_matrices)
{
    auto none = KeywordStyleValue::create(Keyword::None);
    for (auto transform_function : { TransformFunction::Matrix, TransformFunction::Matrix3d }) {
        StyleValueVector arguments;
        auto argument_count = transform_function == TransformFunction::Matrix ? 6u : 16u;
        for (u32 index = 0; index < argument_count; ++index) {
            auto is_diagonal = transform_function == TransformFunction::Matrix
                ? index == 0 || index == 3
                : index % 5 == 0;
            arguments.append(NumberStyleValue::create(is_diagonal ? 1 : 0));
        }
        auto matrix = TransformationStyleValue::create(PropertyID::Transform, transform_function, move(arguments));
        auto to = StyleValueList::create({ matrix }, StyleValueList::Separator::Space);
        auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
            nullptr,
            to_underlying(PropertyID::Transform),
            none->rust_style_value_data(),
            to->rust_style_value_data(),
            0.5f);
        EXPECT(result.handled);
        auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
        auto transformations = interpolated->as_value_list().values();
        EXPECT_EQ(transformations.size(), 1u);
        EXPECT_EQ(transformations[0]->as_transformation().transform_function(), TransformFunction::Matrix3d);
        EXPECT_EQ(transformations[0]->as_transformation().values().size(), 16u);
    }
}

TEST_CASE(rust_expands_omitted_transform_arguments_before_interpolation)
{
    auto from = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::Translate,
            { PercentageStyleValue::create(Percentage { 50 }) }) },
        StyleValueList::Separator::Space);
    auto to = StyleValueList::create(
        { TransformationStyleValue::create(
            PropertyID::Transform,
            TransformFunction::Translate,
            { PercentageStyleValue::create(Percentage { 100 }), PercentageStyleValue::create(Percentage { 50 }) }) },
        StyleValueList::Separator::Space);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    auto arguments = interpolated->as_value_list().values()[0]->as_transformation().values();
    EXPECT_EQ(arguments.size(), 2u);
    EXPECT_APPROXIMATE(arguments[0]->as_percentage().raw_value(), 75.0);
    EXPECT_APPROXIMATE(arguments[1]->as_percentage().raw_value(), 25.0);
}

TEST_CASE(rust_interpolates_mixed_length_percentage_transform_arguments)
{
    auto make_transform = [](NonnullRefPtr<StyleValue const> x, NonnullRefPtr<StyleValue const> y) {
        return StyleValueList::create(
            { TransformationStyleValue::create(
                PropertyID::Transform,
                TransformFunction::Translate,
                { move(x), move(y) }) },
            StyleValueList::Separator::Space);
    };
    auto from = make_transform(
        LengthStyleValue::create(Length::make_px(480)),
        PercentageStyleValue::create(Percentage { 80 }));
    auto to = make_transform(
        PercentageStyleValue::create(Percentage { 240 }),
        LengthStyleValue::create(Length::make_px(400)));

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Transform),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.125f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "translate(calc(30% + 420px), calc(70% + 50px))"sv);
}

TEST_CASE(rust_interpolates_stroke_dasharray_values)
{
    auto make_dasharray = [](std::initializer_list<double> lengths) {
        StyleValueVector values;
        for (auto length : lengths)
            values.append(LengthStyleValue::create(Length::make_px(length)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    };
    auto from = make_dasharray({ 20, 40, 10 });
    auto to = make_dasharray({ 40, 20 });

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::StrokeDasharray),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "30px 30px 25px 20px 40px 15px"sv);

    auto none = KeywordStyleValue::create(Keyword::None);
    StyleValueFFI::FfiAnimationContext context {
        .allow_discrete = true,
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        &context,
        to_underlying(PropertyID::StrokeDasharray),
        none->rust_style_value_data(),
        to->rust_style_value_data(),
        0.75f);
    EXPECT(result.handled);
    interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "40px 20px"sv);
}

TEST_CASE(rust_interpolates_ratio_values)
{
    auto make_ratio = [](double numerator, double denominator) {
        return RatioStyleValue::create(NumberStyleValue::create(numerator), NumberStyleValue::create(denominator));
    };
    auto from = make_ratio(1, 2);
    auto to = make_ratio(2, 1);

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::AspectRatio),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "1 / 1"sv);

    auto from_auto = StyleValueList::create(
        { KeywordStyleValue::create(Keyword::Auto), from },
        StyleValueList::Separator::Space);
    auto to_auto = StyleValueList::create(
        { KeywordStyleValue::create(Keyword::Auto), to },
        StyleValueList::Separator::Space);
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::AspectRatio),
        from_auto->rust_style_value_data(),
        to_auto->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "auto 1 / 1"sv);

    auto degenerate = make_ratio(1, 0);
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::AspectRatio),
        degenerate->rust_style_value_data(),
        to->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    EXPECT_EQ(result.value, nullptr);
}

TEST_CASE(rust_interpolates_individual_scale_values)
{
    auto none = KeywordStyleValue::create(Keyword::None);
    auto scale = TransformationStyleValue::create(
        PropertyID::Scale,
        TransformFunction::Scale3d,
        { NumberStyleValue::create(2), NumberStyleValue::create(0), NumberStyleValue::create(3) });

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Scale),
        none->rust_style_value_data(),
        scale->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->as_transformation().transform_function(), TransformFunction::Scale3d);
    auto arguments = interpolated->as_transformation().values();
    EXPECT_EQ(arguments.size(), 3u);
    EXPECT_APPROXIMATE(arguments[0]->as_number().number(), 1.5);
    EXPECT_APPROXIMATE(arguments[1]->as_number().number(), 0.5);
    EXPECT_APPROXIMATE(arguments[2]->as_number().number(), 2.0);
}

TEST_CASE(rust_interpolates_individual_translate_values)
{
    auto from = TransformationStyleValue::create(
        PropertyID::Translate,
        TransformFunction::Translate3d,
        { LengthStyleValue::create(Length::make_px(480)), LengthStyleValue::create(Length::make_px(400)), LengthStyleValue::create(Length::make_px(320)) });
    auto to = TransformationStyleValue::create(
        PropertyID::Translate,
        TransformFunction::Translate,
        { PercentageStyleValue::create(Percentage { 240 }), PercentageStyleValue::create(Percentage { 160 }) });

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Translate),
        from->rust_style_value_data(),
        to->rust_style_value_data(),
        0.125f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "calc(30% + 420px) calc(20% + 350px) 280px"sv);

    auto zero = TransformationStyleValue::create(
        PropertyID::Translate,
        TransformFunction::Translate,
        { LengthStyleValue::create(Length::make_px(0)), LengthStyleValue::create(Length::make_px(0)) });
    auto three_dimensions = TransformationStyleValue::create(
        PropertyID::Translate,
        TransformFunction::Translate3d,
        { LengthStyleValue::create(Length::make_px(-100)), LengthStyleValue::create(Length::make_px(-50)), LengthStyleValue::create(Length::make_px(100)) });
    result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Translate),
        zero->rust_style_value_data(),
        three_dimensions->rust_style_value_data(),
        0.25f);
    EXPECT(result.handled);
    interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->to_string(SerializationMode::Normal), "-25px -12.5px 25px"sv);
}

TEST_CASE(rust_interpolates_individual_rotate_values)
{
    auto none = KeywordStyleValue::create(Keyword::None);
    auto angle = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_create_angle(90, 0));
    auto rotate = TransformationStyleValue::create(
        PropertyID::Rotate,
        TransformFunction::Rotate3d,
        { NumberStyleValue::create(0), NumberStyleValue::create(1), NumberStyleValue::create(0), angle });

    auto result = StyleValueFFI::rust_interpolate_scalar_style_value(
        nullptr,
        to_underlying(PropertyID::Rotate),
        none->rust_style_value_data(),
        rotate->rust_style_value_data(),
        0.5f);
    EXPECT(result.handled);
    auto interpolated = StyleValue::adopt_rust_style_value_data(result.value);
    EXPECT_EQ(interpolated->as_transformation().transform_function(), TransformFunction::Rotate3d);
    auto arguments = interpolated->as_transformation().values();
    EXPECT_EQ(arguments.size(), 4u);
    EXPECT_APPROXIMATE(arguments[0]->as_number().number(), 0.0);
    EXPECT_APPROXIMATE(arguments[1]->as_number().number(), 1.0);
    EXPECT_APPROXIMATE(arguments[2]->as_number().number(), 0.0);
    EXPECT_APPROXIMATE(arguments[3]->rust_style_value_data()->angle.value, 45.0);
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
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
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
        .current_color = nullptr,
        .has_length_resolution_context = false,
        .length_resolution_context = {},
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
    auto make_unresolved = [](Utf16String source_text) {
        return UnresolvedStyleValue::create({}, {}, move(source_text));
    };

    // U+00A0 has the Unicode White_Space property but is not ASCII whitespace; values differing
    // by it must not compare equal, or custom-property change detection misses the update.
    auto plain = make_unresolved("foo"_utf16);
    auto with_leading_nbsp = make_unresolved("\u00A0foo"_utf16);

    EXPECT(plain->equals(*make_unresolved("foo"_utf16)));
    EXPECT(!plain->equals(*with_leading_nbsp));
}

}
