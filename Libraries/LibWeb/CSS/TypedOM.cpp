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

// What LibWeb's C++ code asks of the Rust implementation of the CSS Typed OM (Libraries/LibWeb/Bindings/Rust/src/css).

namespace Web::CSS {

using StyleValueData = StyleValueFFI::StyleValueData;
using FfiNumericType = StyleValueFFI::FfiNumericType;
using FfiCalcNode = StyleValueFFI::CalcNode;

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_css_style_value_create(JSVM*, FlatPtr associated_property, StyleValueData const*, u8 serialization_mode);
void* web_css_style_value_reify(JSVM*, StyleValueData const*, FlatPtr associated_property);
bool web_css_style_value_associated_property(void* value, FlatPtr* associated_property);
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

Optional<Utf16FlyString> CSSStyleValue::associated_property() const
{
    FlatPtr associated_property = 0;
    if (!web_css_style_value_associated_property(const_cast<CSSStyleValue*>(this), &associated_property))
        return {};
    return Utf16FlyString { Utf16String::adopt_raw(associated_property) };
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
    return Utf16String::adopt_raw(web_css_numeric_value_to_string(const_cast<CSSNumericValue*>(this)));
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
    return Utf16FlyString { Utf16String::adopt_raw(web_css_unit_value_unit(const_cast<CSSUnitValue*>(this))) };
}

Utf16String CSSKeywordValue::value() const
{
    return Utf16String::adopt_raw(web_css_keyword_value_value(const_cast<CSSKeywordValue*>(this)));
}

}
