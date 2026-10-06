/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>

// These tests build separately-allocated style values with equal (or deliberately unequal)
// contents and check that StyleValue::equals() compares by value, not by pointer identity.
// Style value equality feeds restyle invalidation and transition change detection, so drift
// here does not show up in rendering tests until it causes stale styles or spurious
// transitions.

namespace Web::CSS {

TEST_CASE(computed_longhand_table_publication_identity_includes_sidecars)
{
    auto value = NumberStyleValue::create(1);
    auto* original = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_set(original, to_underlying(PropertyID::Color), value->rust_style_value_data(), -1);

    auto* copy = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_copy_from(copy, original);
    ComputedValuesFFI::rust_computed_longhand_table_clear_seeded_state(copy);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(original);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(copy);
    EXPECT(!ComputedValuesFFI::rust_computed_longhand_tables_equal_for_publication(original, copy));
    ComputedValuesFFI::rust_computed_longhand_table_release(original);
    ComputedValuesFFI::rust_computed_longhand_table_release(copy);

    original = ComputedValuesFFI::rust_computed_longhand_table_create();
    copy = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_metadata(copy)->display_before_box_type_transformation = 1;
    ComputedValuesFFI::rust_computed_longhand_table_freeze(original);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(copy);
    EXPECT(!ComputedValuesFFI::rust_computed_longhand_tables_equal_for_publication(original, copy));
    ComputedValuesFFI::rust_computed_longhand_table_release(original);
    ComputedValuesFFI::rust_computed_longhand_table_release(copy);

    original = ComputedValuesFFI::rust_computed_longhand_table_create();
    copy = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_metadata(copy)->effective_color_scheme = 1;
    ComputedValuesFFI::rust_computed_longhand_table_freeze(original);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(copy);
    EXPECT(!ComputedValuesFFI::rust_computed_longhand_tables_equal_for_publication(original, copy));
    ComputedValuesFFI::rust_computed_longhand_table_release(original);
    ComputedValuesFFI::rust_computed_longhand_table_release(copy);
}

TEST_CASE(computed_longhand_table_inherited_copy_refreshes_effective_color_scheme)
{
    auto* child = ComputedValuesFFI::rust_computed_longhand_table_create();
    auto* parent = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_metadata(child)->effective_color_scheme = 0;
    ComputedValuesFFI::rust_computed_longhand_table_metadata(parent)->effective_color_scheme = 1;
    ComputedValuesFFI::rust_computed_longhand_table_freeze(child);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(parent);

    auto* inherited = ComputedValuesFFI::rust_computed_longhand_table_create_with_inherited_values(child, parent);
    EXPECT_EQ(ComputedValuesFFI::rust_computed_longhand_table_metadata(inherited)->effective_color_scheme, 1);

    ComputedValuesFFI::rust_computed_longhand_table_release(child);
    ComputedValuesFFI::rust_computed_longhand_table_release(parent);
    ComputedValuesFFI::rust_computed_longhand_table_release(inherited);
}

static StyleValueFFI::StyleValueData const* create_test_image(StringView url)
{
    auto url_string = MUST(String::from_utf8(url));
    auto url_bytes = url_string.bytes();
    return StyleValueFFI::rust_style_value_create_image(
        { url_bytes.data(), nullptr, url_bytes.size() }, 0, nullptr, 0,
        {}, false);
}

TEST_CASE(rust_serialization_transfers_a_native_utf16_string)
{
    auto value = CustomIdentStyleValue::create(Utf16FlyString::from_utf8("hello😀"sv));
    auto text = StyleValueFFI::rust_style_value_serialize(
        value->rust_style_value_data(), to_underlying(SerializationMode::Normal));

    auto serialized = Utf16String::adopt_raw(text);
    EXPECT_EQ(serialized, u"hello😀"sv);
    EXPECT(!serialized.has_ascii_storage());

    auto ascii_value = CustomIdentStyleValue::create(Utf16FlyString::from_utf8("abc"sv));
    auto ascii_text = StyleValueFFI::rust_style_value_serialize(
        ascii_value->rust_style_value_data(), to_underlying(SerializationMode::Normal));

    auto ascii_serialized = Utf16String::adopt_raw(ascii_text);
    EXPECT_EQ(ascii_serialized, u"abc"sv);
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

    expect_type(StyleValueFFI::rust_style_value_create_display(0), StyleValue::Type::Display);
    expect_type(StyleValueFFI::rust_style_value_create_guaranteed_invalid(), StyleValue::Type::GuaranteedInvalid);
    expect_type(StyleValueFFI::rust_style_value_create_shorthand(0, nullptr, 0, nullptr, 0), StyleValue::Type::Shorthand);
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
    auto retained_shadow = static_cast<DropShadowFilterStyleValue const&>(filter->as_filter()).shadow();
    filter = KeywordStyleValue::create(Keyword::None);
    EXPECT_EQ(retained_shadow->to_string(SerializationMode::Normal), "1px 2px 3px"sv);
}

TEST_CASE(rust_calculated_handles_create_typed_wrappers)
{
    StyleValueFFI::FfiNumericType resolved_type {};
    resolved_type.valid = true;
    resolved_type.has_exponent_bits |= 1u << to_underlying(NumericType::BaseType::Length);
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

TEST_CASE(rust_counter_definition_handles_retain_value_data)
{
    auto value = IntegerStyleValue::create(2);
    StyleValueFFI::FfiCounterDefinition definition {
        Utf16FlyString::from_utf8("item"sv).to_raw_leaked(),
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

TEST_CASE(rust_image_handles_create_typed_wrappers)
{
    auto image = StyleValue::adopt_rust_style_value_data(
        create_test_image("image.png"sv));
    EXPECT(image->is_image());
    EXPECT_EQ(image->to_string(SerializationMode::Normal), "url(\"image.png\")"sv);
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
