/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSKeywordValue.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

// What the Rust implementation of the CSS Typed OM (Libraries/LibWeb/Bindings/Rust/src/css) and LibWeb's C++ code ask
// of each other. The tables of the properties and the entry points of the parser are in C++ for now, so what depends
// on them is here.

namespace Web::CSS {

using StyleValueData = StyleValueFFI::StyleValueData;
using FfiNumericType = StyleValueFFI::FfiNumericType;
using FfiCalcNode = StyleValueFFI::CalcNode;

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_css_style_value_create(JSVM*, FlatPtr associated_property, StyleValueData const*, u8 serialization_mode);
void* web_css_style_value_reify(JSVM*, StyleValueData const*, FlatPtr associated_property);
void* web_css_style_value_parse(JSVM*, FlatPtr property, FlatPtr css_text, FlatPtr* error);
bool web_css_style_value_associated_property(void* value, FlatPtr* associated_property);
StyleValueData const* web_css_style_value_create_an_internal_representation(JSVM*, void* value, u16 property_id, FlatPtr property_name, bool perform_type_check, FlatPtr* error);
u8 web_css_style_value_kind(void* value);
void* web_css_numeric_value_create_sum(JSVM*, void* first, void* second);
FfiNumericType web_css_numeric_value_type(void* value);
FlatPtr web_css_numeric_value_to_string(void* value);
FfiCalcNode const* web_css_numeric_value_create_calculation_node(void* value);
void* web_css_unit_value_create(JSVM*, double value, FlatPtr unit);
double web_css_unit_value_value(void* value);
FlatPtr web_css_unit_value_unit(void* value);
FlatPtr web_css_keyword_value_value(void* value);
}

// The order of `enum StyleValueKind` in Bindings/Rust/src/css/exports.rs.
enum class StyleValueKind : u8 {
    Other,
    Numeric,
    Unit,
    Unparsed,
};

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

static StyleValueKind kind_of(CSSStyleValue const& value)
{
    return static_cast<StyleValueKind>(web_css_style_value_kind(const_cast<CSSStyleValue*>(&value)));
}

GC::Ref<CSSStyleValue> CSSStyleValue::create(Utf16FlyString associated_property, StyleValue const& source_value, SerializationMode serialization_mode)
{
    auto* value = web_css_style_value_create(vm_abi(), associated_property.to_utf16_string().into_raw(), StyleValueFFI::rust_style_value_retain(source_value.rust_style_value_data()), to_underlying(serialization_mode));
    return GC::Ref { *static_cast<CSSStyleValue*>(value) };
}

GC::Ref<CSSStyleValue> CSSStyleValue::reify(StyleValue const& value, Utf16FlyString const& associated_property)
{
    auto* reified_value = web_css_style_value_reify(vm_abi(), StyleValueFFI::rust_style_value_retain(value.rust_style_value_data()), associated_property.to_utf16_string().into_raw());
    return GC::Ref { *static_cast<CSSStyleValue*>(reified_value) };
}

WebIDL::ExceptionOr<GC::Ref<CSSStyleValue>> CSSStyleValue::parse_a_css_style_value(Utf16FlyString const& property, Utf16View css_text)
{
    FlatPtr error = 0;
    auto* value = web_css_style_value_parse(vm_abi(), property.to_utf16_string().into_raw(), Utf16String::from_utf16(css_text).into_raw(), &error);
    if (!value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_raw(error) };
    return GC::Ref { *static_cast<CSSStyleValue*>(value) };
}

Optional<Utf16FlyString> CSSStyleValue::associated_property() const
{
    FlatPtr associated_property = 0;
    if (!web_css_style_value_associated_property(const_cast<CSSStyleValue*>(this), &associated_property))
        return {};
    return Utf16FlyString { Utf16String::from_raw(associated_property) };
}

WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> CSSStyleValue::create_an_internal_representation(PropertyNameAndID const& property, PerformTypeCheck perform_type_check) const
{
    FlatPtr error = 0;
    auto const* value = web_css_style_value_create_an_internal_representation(vm_abi(), const_cast<CSSStyleValue*>(this), to_underlying(property.id()), property.name().to_utf16_string().into_raw(), perform_type_check == PerformTypeCheck::Yes, &error);
    if (!value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_raw(error) };
    return NonnullRefPtr<StyleValue const> { *StyleValue::adopt_rust_style_value_data(value) };
}

bool CSSStyleValue::is_numeric_value() const
{
    auto kind = kind_of(*this);
    return kind == StyleValueKind::Numeric || kind == StyleValueKind::Unit;
}

bool CSSStyleValue::is_unit_value() const
{
    return kind_of(*this) == StyleValueKind::Unit;
}

bool CSSStyleValue::is_unparsed_value() const
{
    return kind_of(*this) == StyleValueKind::Unparsed;
}

GC::Ref<CSSNumericValue> CSSNumericValue::create_sum(GC::Ref<CSSNumericValue> first, GC::Ref<CSSNumericValue> second)
{
    return GC::Ref { *static_cast<CSSNumericValue*>(web_css_numeric_value_create_sum(vm_abi(), first.ptr(), second.ptr())) };
}

NumericType CSSNumericValue::type() const
{
    return NumericType::from_ffi(web_css_numeric_value_type(const_cast<CSSNumericValue*>(this))).release_value();
}

Utf16String CSSNumericValue::to_string() const
{
    return Utf16String::from_raw(web_css_numeric_value_to_string(const_cast<CSSNumericValue*>(this)));
}

Optional<CalcNodeRef> CSSNumericValue::create_calculation_node() const
{
    auto const* node = web_css_numeric_value_create_calculation_node(const_cast<CSSNumericValue*>(this));
    if (!node)
        return {};
    return CalcNodeRef::adopt(node);
}

GC::Ref<CSSUnitValue> CSSUnitValue::create(double value, Utf16FlyString const& unit)
{
    return GC::Ref { *static_cast<CSSUnitValue*>(web_css_unit_value_create(vm_abi(), value, unit.to_utf16_string().into_raw())) };
}

double CSSUnitValue::value() const
{
    return web_css_unit_value_value(const_cast<CSSUnitValue*>(this));
}

Utf16FlyString CSSUnitValue::unit() const
{
    return Utf16FlyString { Utf16String::from_raw(web_css_unit_value_unit(const_cast<CSSUnitValue*>(this))) };
}

Utf16String CSSKeywordValue::value() const
{
    return Utf16String::from_raw(web_css_keyword_value_value(const_cast<CSSKeywordValue*>(this)));
}

static StyleValueData const* into_rust(StyleValue const& value)
{
    return StyleValueFFI::rust_style_value_retain(value.rust_style_value_data());
}

static PropertyID property_id_from_abi(u16 property_id)
{
    return static_cast<PropertyID>(property_id);
}

// The order of `enum TypedOmValueType` in Bindings/Rust/src/css/typed_om.rs.
enum class TypedOmValueType : u8 {
    Image,
    TransformFunction,
    TransformList,
};

extern "C" {

// Declared in Bindings/Rust/src/css/typed_om.rs.
bool web_css_typed_om_property_accepts_type(u16 property_id, TypedOmValueType value_type);
StyleValueData const* web_css_typed_om_numeric_style_value(u16 property_id_abi, StyleValueData const* value_data, FfiCalcNode const* calculation_data, FfiNumericType numeric_type);
StyleValueData const* web_css_typed_om_calculated_style_value(u16 property_id_abi, FfiCalcNode const* calculation_data, FfiNumericType numeric_type, bool perform_type_check);
StyleValueData const* web_css_typed_om_keyword_style_value(u16 property_id_abi, FlatPtr raw_value, bool perform_type_check);
StyleValueData const* web_css_typed_om_style_value_list(StyleValueData const* const* values, size_t count);
StyleValueData const* web_css_typed_om_parse_numeric_value(FlatPtr raw_css_text);
bool web_css_typed_om_parse_value(FlatPtr raw_property, FlatPtr raw_css_text, void* values, void (*append)(void*, StyleValueData const*), FlatPtr* name_or_error);
bool web_css_typed_om_zero_is_a_length(FlatPtr raw_property, bool is_integer);

bool web_css_typed_om_property_accepts_type(u16 property_id, TypedOmValueType value_type)
{
    auto const to_value_type = [&] {
        switch (value_type) {
        case TypedOmValueType::Image:
            return ValueType::Image;
        case TypedOmValueType::TransformFunction:
            return ValueType::TransformFunction;
        case TypedOmValueType::TransformList:
            return ValueType::TransformList;
        }
        VERIFY_NOT_REACHED();
    };
    return property_accepts_type(property_id_from_abi(property_id), to_value_type());
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
StyleValueData const* web_css_typed_om_numeric_style_value(u16 property_id_abi, StyleValueData const* value_data, FfiCalcNode const* calculation_data, FfiNumericType numeric_type)
{
    auto property_id = property_id_from_abi(property_id_abi);
    auto value = StyleValue::adopt_rust_style_value_data(value_data);
    auto calculation = CalcNodeRef::adopt(calculation_data);

    // If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of value is a
    // CSSUnitValue that is outside of that range, replace that value with the result of wrapping it in a fresh
    // CSSMathSum whose values internal slot contains only that part of value.
    auto wrap_in_math_sum = [&] {
        return into_rust(*CalculatedStyleValue::create(move(calculation), NumericType::from_ffi(numeric_type).release_value(), CalculationContext::for_property(property_id)));
    };

    if (value->is_number()) {
        auto number = value->as_number().number();

        // NB: Our parser represents an <opacity-value> as an opacity value that holds the number.
        if (property_accepts_type(property_id, ValueType::OpacityValue))
            return into_rust(*OpacityValueStyleValue::create(value));

        // NB: Prefer <number> to <integer> when both are accepted.
        if (property_accepts_type(property_id, ValueType::Number))
            return property_accepts_number(property_id, number) ? into_rust(*value) : wrap_in_math_sum();

        if (property_accepts_type(property_id, ValueType::Integer)) {
            auto integer = round_to_nearest_integer(number);
            // NB: A number that is not an integer does not match <integer>, so it's wrapped in a CSSMathSum, like a
            //     value outside of the accepted range. It then gets rounded when computed.
            if (static_cast<double>(integer) != number)
                return wrap_in_math_sum();
            if (property_accepts_integer(property_id, integer))
                return into_rust(*IntegerStyleValue::create(integer));
            return wrap_in_math_sum();
        }
        return nullptr;
    }

    if (value->is_percentage()) {
        // NB: Our parser represents an <opacity-value> as an opacity value that holds the percentage.
        if (property_accepts_type(property_id, ValueType::OpacityValue))
            return into_rust(*OpacityValueStyleValue::create(value));
        if (!property_accepts_type(property_id, ValueType::Percentage))
            return nullptr;
        return property_accepts_percentage(property_id, value->as_percentage().percentage()) ? into_rust(*value) : wrap_in_math_sum();
    }

    if (value->is_flex()) {
        // NB: Out-of-range flex values cannot be deferred in a calculation.
        if (property_accepts_type(property_id, ValueType::Flex) && property_accepts_flex(property_id, value->as_flex().flex()))
            return into_rust(*value);
        return nullptr;
    }

    auto dimension_value = [&](ValueType value_type, bool is_accepted) -> StyleValueData const* {
        if (!property_accepts_type(property_id, value_type))
            return nullptr;
        return is_accepted ? into_rust(*value) : wrap_in_math_sum();
    };
    if (value->is_angle())
        return dimension_value(ValueType::Angle, property_accepts_angle(property_id, value->as_angle().angle()));
    if (value->is_frequency())
        return dimension_value(ValueType::Frequency, property_accepts_frequency(property_id, value->as_frequency().frequency()));
    if (value->is_length())
        return dimension_value(ValueType::Length, property_accepts_length(property_id, value->as_length().length()));
    if (value->is_resolution())
        return dimension_value(ValueType::Resolution, property_accepts_resolution(property_id, value->as_resolution().resolution()));
    if (value->is_time())
        return dimension_value(ValueType::Time, property_accepts_time(property_id, value->as_time().time()));
    VERIFY_NOT_REACHED();
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
StyleValueData const* web_css_typed_om_calculated_style_value(u16 property_id_abi, FfiCalcNode const* calculation_data, FfiNumericType numeric_type, bool perform_type_check)
{
    auto property_id = property_id_from_abi(property_id_abi);
    auto calculation = CalcNodeRef::adopt(calculation_data);
    auto type = NumericType::from_ffi(numeric_type).release_value();
    auto context = CalculationContext::for_property(property_id);

    auto matches = [&] {
        if (type.matches_angle(context.percentages_resolve_as))
            return property_accepts_type(property_id, ValueType::Angle);
        if (type.matches_flex(context.percentages_resolve_as)) {
            // https://drafts.csswg.org/css-grid-2/#fr-unit
            // NB: <flex> values "cannot be represented in or combined with other unit types in calc() expressions".
            return property_id == PropertyID::Custom && property_accepts_type(property_id, ValueType::Flex);
        }
        if (type.matches_frequency(context.percentages_resolve_as))
            return property_accepts_type(property_id, ValueType::Frequency);
        if (type.matches_length(context.percentages_resolve_as))
            return property_accepts_type(property_id, ValueType::Length);
        if (type.matches_number(context.percentages_resolve_as))
            return property_accepts_type(property_id, ValueType::Number) || property_accepts_type(property_id, ValueType::Integer) || property_accepts_type(property_id, ValueType::OpacityValue);
        if (type.matches_percentage())
            return property_accepts_type(property_id, ValueType::Percentage) || property_accepts_type(property_id, ValueType::OpacityValue);
        if (type.matches_resolution(context.percentages_resolve_as))
            return property_accepts_type(property_id, ValueType::Resolution);
        if (type.matches_time(context.percentages_resolve_as))
            return property_accepts_type(property_id, ValueType::Time);
        return false;
    }();
    if (perform_type_check && !matches)
        return nullptr;

    auto is_number_or_percentage = type.matches_number({}) || type.matches_percentage();
    auto calculated_value = CalculatedStyleValue::create(move(calculation), move(type), move(context));

    // NB: Our parser represents an <opacity-value> as an opacity value that holds the calculation.
    if (property_accepts_type(property_id, ValueType::OpacityValue) && is_number_or_percentage)
        return into_rust(*OpacityValueStyleValue::create(move(calculated_value)));

    return into_rust(*calculated_value);
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
StyleValueData const* web_css_typed_om_keyword_style_value(u16 property_id_abi, FlatPtr raw_value, bool perform_type_check)
{
    auto property_id = property_id_from_abi(property_id_abi);
    auto value = Utf16FlyString { Utf16String::from_raw(raw_value) };
    auto keyword = keyword_from_string(value);

    // https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue-match-a-grammar
    // NB: The keywords listed for a property are not all valid on their own, like `hanging` for text-indent, and a
    //     property may accept identifiers that are not listed, like a <custom-ident> or the axis keywords of
    //     scroll-snap-type. So we check if the property accepts the value, as an identifier, when parsed.
    auto const matches_grammar = [&] {
        if (keyword.has_value() && is_css_wide_keyword(*keyword))
            return true;
        return parse_css_value(Parser::ParsingParams {}, serialize_an_identifier(value), property_id) != nullptr;
    };
    if (perform_type_check && !matches_grammar())
        return nullptr;

    if (keyword.has_value()) {
        // NB: Style computation expects the same internal representation that the CSS parser produces for the
        //     property. For example, `display` keywords become a DisplayStyleValue, and keyword combinations like
        //     `text-decoration-line` or `white-space-trim` become a list even when only one keyword is given. So we
        //     parse the keyword as a value of the property to get that representation. List-valued properties are
        //     instead wrapped in a list by StylePropertyMap.
        if (!is_css_wide_keyword(*keyword) && property_id != PropertyID::Custom && !property_is_list_valued(property_id)) {
            if (auto parsed_value = parse_css_value(Parser::ParsingParams {}, value.view(), property_id))
                return into_rust(*parsed_value);
        }
        return into_rust(*KeywordStyleValue::create(*keyword));
    }
    return into_rust(*CustomIdentStyleValue::create(value));
}

StyleValueData const* web_css_typed_om_style_value_list(StyleValueData const* const* values, size_t count)
{
    StyleValueVector list;
    list.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i)
        list.unchecked_append(StyleValue::adopt_rust_style_value_data(values[i]));
    return into_rust(*StyleValueList::create(move(list), StyleValueList::Separator::Space));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-parse
StyleValueData const* web_css_typed_om_parse_numeric_value(FlatPtr raw_css_text)
{
    auto css_text = Utf16String::from_raw(raw_css_text);
    for (auto value_type : { ValueType::Number, ValueType::Percentage, ValueType::Length, ValueType::LengthPercentage, ValueType::Angle, ValueType::Time, ValueType::Frequency, ValueType::Resolution, ValueType::Flex }) {
        if (auto value = parse_css_type(Parser::ParsingParams {}, css_text, value_type))
            return into_rust(*value);
    }
    return nullptr;
}

// https://drafts.css-houdini.org/css-typed-om-1/#parse-a-cssstylevalue
bool web_css_typed_om_parse_value(FlatPtr raw_property, FlatPtr raw_css_text, void* values, void (*append)(void*, StyleValueData const*), FlatPtr* name_or_error)
{
    auto property_name = Utf16FlyString { Utf16String::from_raw(raw_property) };
    auto css_text = Utf16String::from_raw(raw_css_text);

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value()) {
        *name_or_error = Utf16String::formatted("'{}' is not a valid CSS property", property_name).into_raw();
        return false;
    }

    // 3. Attempt to parse cssText according to property’s grammar.
    //    If this fails, throw a TypeError.
    //    Otherwise, let whole value be the parsed result.
    auto whole_value = parse_css_value(Parser::ParsingParams {}, css_text, property->id());
    if (!whole_value) {
        *name_or_error = Utf16String::formatted("Failed to parse '{}' as a value for '{}' property", css_text, property_name).into_raw();
        return false;
    }

    // 4. Subdivide into iterations whole value, according to property, and let values be the result.
    for (auto const& value : whole_value->subdivide_into_iterations(property.value()))
        append(values, into_rust(*value));

    *name_or_error = property->name().to_utf16_string().into_raw();
    return true;
}

// Whether the property expects a 0 to be a <length>.
bool web_css_typed_om_zero_is_a_length(FlatPtr raw_property, bool is_integer)
{
    auto property = Utf16String::from_raw(raw_property);
    if (!property.is_ascii())
        return false;
    auto property_id = property_id_from_string(property);
    if (!property_id.has_value() || property_id == PropertyID::Custom || !property_accepts_type(*property_id, ValueType::Length))
        return false;
    return is_integer || (!property_accepts_type(*property_id, ValueType::Number) && !property_accepts_type(*property_id, ValueType::Integer));
}
}

}
