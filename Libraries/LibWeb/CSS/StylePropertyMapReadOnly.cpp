/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StylePropertyMap.h>
#include <LibWeb/CSS/StylePropertyMapReadOnly.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/Zoom.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

using StyleValueData = StyleValueFFI::StyleValueData;

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_style_property_map_create(JSVM*, void* declaration);
void* web_style_property_map_read_only_create(JSVM*, void* element);

// Declared in Bindings/Rust/src/css/style_property_map_read_only.rs.
bool web_css_style_map_property(FlatPtr name, u16* id, FlatPtr* canonical_name);
u8 web_css_style_map_iterations(bool is_computed, void* source, FlatPtr name, void* values, void (*append)(void*, StyleValueData const*));
bool web_css_style_map_has(bool is_computed, void* source, FlatPtr name);
void web_css_style_map_property_names(bool is_computed, void* source, void* names, void (*append)(void*, FlatPtr));
size_t web_css_style_map_property_count(bool is_computed, void* source);

// Declared in Bindings/Rust/src/css/style_property_map.rs.
bool web_css_style_map_property_is_list_valued(u16 property_id);
StyleValueData const* web_css_style_map_parse(void* declaration, u16 property_id, FlatPtr css_text);
StyleValueData const* web_css_style_map_declared_value(void* declaration, FlatPtr name);
u8 web_css_style_map_set_value(void* declaration, FlatPtr name, StyleValueData const* value, FlatPtr* message);
u8 web_css_style_map_remove(void* declaration, FlatPtr name, FlatPtr* message);
u8 web_css_style_map_clear(void* declaration, FlatPtr* message);
}

// What web_css_style_map_iterations() found, which Bindings/Rust/src/css/style_property_map_read_only.rs reads.
enum class Iterations : u8 {
    Absent,
    Values,
    Shorthand,
};

// How a change to a declaration ended, which Bindings/Rust/src/css/style_property_map.rs reads.
enum class ChangeResult : u8 {
    Success,
    TypeError,
    NoModificationAllowedError,
};

template<typename T>
static u8 change_result(WebIDL::ExceptionOr<T> result, FlatPtr* message)
{
    if (!result.is_exception())
        return to_underlying(ChangeResult::Success);
    return to_underlying(result.exception().visit(
        [&](WebIDL::SimpleException const& exception) {
            VERIFY(exception.type == WebIDL::SimpleExceptionType::TypeError);
            *message = Utf16String { exception.message }.into_raw();
            return ChangeResult::TypeError;
        },
        [&](GC::Ref<WebIDL::DOMException> const& exception) {
            VERIFY(exception->name() == "NoModificationAllowedError"sv);
            *message = exception->message().to_utf16_string().into_raw();
            return ChangeResult::NoModificationAllowedError;
        },
        [](JS::Completion const&) -> ChangeResult {
            VERIFY_NOT_REACHED();
        }));
}

using Source = Variant<DOM::AbstractElement, GC::Ref<CSSStyleDeclaration>>;

static CSSStyleDeclaration& declaration_from_abi(void* declaration)
{
    return *static_cast<CSSStyleDeclaration*>(static_cast<GC::Cell*>(declaration));
}

static Source source_from_abi(bool is_computed, void* source)
{
    if (is_computed)
        return DOM::AbstractElement { *static_cast<DOM::Element*>(static_cast<GC::Cell*>(source)) };
    return GC::Ref { declaration_from_abi(source) };
}

static StyleValueData const* into_rust(StyleValue const& value)
{
    return StyleValueFFI::rust_style_value_retain(value.rust_style_value_data());
}

static PropertyNameAndID property_from_abi(FlatPtr name)
{
    return PropertyNameAndID::from_name(Utf16FlyString { Utf16String::adopt_raw(name) }).release_value();
}

// A computed style map describes an element's computed style, and a disconnected element has none.
// `getComputedStyle` refuses one outright rather than reporting a style nothing decided, and the map
// answers the same way: empty, with every property absent.
static bool has_computed_style(DOM::AbstractElement const& abstract_element)
{
    return abstract_element.element().is_connected();
}

static StyleValueVector subdivide_property_value(StyleValue const& value, PropertyNameAndID const& property, DOM::AbstractElement const* computed_element)
{
    // https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
    // If an internal representation is from a list-valued property, this list defines how to reify a single iteration
    // of the property; multiple iterations are reflected by returning multiple values from StylePropertyMap.getAll().
    if (computed_element && property.is_custom_property() && value.is_value_list()) {
        auto registration = computed_element->document().get_registered_custom_property(property.name());
        if (registration.has_value() && Parser::ValueParserFFI::rust_syntax_value_is_list(registration->syntax.data(), value.rust_style_value_data()))
            return value.as_value_list().values();
    }
    return value.subdivide_into_iterations(property);
}

static RefPtr<StyleValue const> get_style_value(Source& source, PropertyNameAndID const& property)
{
    return source.visit(
        [&property](DOM::AbstractElement& element) -> RefPtr<StyleValue const> {
            if (!has_computed_style(element))
                return nullptr;
            // From https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap we need to include:
            // "the name and computed value of every longhand CSS property supported by the User Agent, every
            // registered custom property, and every non-registered custom property which is not set to its initial
            // value on this"
            // Ensure style is computed on the element before we try to read it.
            element.document().update_style_for_element(element);
            if (property.is_custom_property()) {
                if (auto custom_property = element.get_custom_property(property.name()))
                    return custom_property;
                if (auto registered_custom_property = element.document().get_registered_custom_property(property.name()); registered_custom_property.has_value())
                    return registered_custom_property->initial_value;
                return nullptr;
            }

            if (auto computed_values = element.computed_style()) {
                auto value_for_property = [&](auto& self, PropertyID property_id) -> RefPtr<StyleValue const> {
                    // NB: The computed longhands of all cannot be expressed as a value of all.
                    if (property_id == PropertyID::All)
                        return nullptr;
                    // FIXME: Expose overflow-clip-margin shorthands once their four-sided grammar is interoperable.
                    if (first_is_one_of(property_id, PropertyID::OverflowClipMargin, PropertyID::OverflowClipMarginBlock, PropertyID::OverflowClipMarginInline))
                        return nullptr;
                    if (property_is_shorthand(property_id)) {
                        Vector<PropertyID> longhands = longhands_for_shorthand(property_id);
                        StyleValueVector values;
                        values.ensure_capacity(longhands.size());
                        for (auto longhand : longhands) {
                            auto value = self(self, longhand);
                            if (!value)
                                return nullptr;
                            values.unchecked_append(value.release_nonnull());
                        }
                        auto shorthand = ShorthandStyleValue::create(property_id, move(longhands), move(values));
                        if (shorthand->to_utf16_string(SerializationMode::ResolvedValue).is_empty())
                            return nullptr;
                        return shorthand;
                    }
                    if (property_id < first_longhand_property_id || property_id > last_longhand_property_id)
                        return nullptr;
                    if (property_is_logical_alias(property_id))
                        property_id = map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { computed_values->writing_mode(), computed_values->direction() });
                    if (property_id == PropertyID::BackgroundColor && computed_values->background_color_style_value())
                        return computed_values->background_color_style_value();
                    auto value = computed_values->computed_style_value(property_id);
                    if (!value)
                        return nullptr;
                    // INTEROP: Computed style maps expose zero letter-spacing as normal, matching the legacy CSSOM
                    //          resolved value.
                    if (property_id == PropertyID::LetterSpacing && value->is_length() && value->as_length().raw_value() == 0)
                        return KeywordStyleValue::create(Keyword::Normal);
                    return unscaled_by_zoom(property_id, value.release_nonnull(), computed_values->effective_zoom());
                };
                return value_for_property(value_for_property, property.id());
            }

            return nullptr;
        },
        [&property](GC::Ref<CSSStyleDeclaration>& declaration) {
            return declaration->get_property_style_value(property);
        });
}

GC::Ref<StylePropertyMapReadOnly> StylePropertyMapReadOnly::create_computed_style(DOM::Element& element)
{
    auto* map = web_style_property_map_read_only_create(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(&element));
    return GC::Ref { *static_cast<StylePropertyMapReadOnly*>(map) };
}

GC::Ref<StylePropertyMap> StylePropertyMap::create(GC::Ref<CSSStyleDeclaration> declaration)
{
    auto* map = web_style_property_map_create(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(declaration.ptr()));
    return GC::Ref { *static_cast<StylePropertyMap*>(map) };
}

bool web_css_style_map_property(FlatPtr raw_name, u16* id, FlatPtr* canonical_name)
{
    auto property = PropertyNameAndID::from_name(Utf16FlyString { Utf16String::adopt_raw(raw_name) });
    if (!property.has_value())
        return false;
    *id = to_underlying(property->id());
    *canonical_name = property->name().to_utf16_string().into_raw();
    return true;
}

u8 web_css_style_map_iterations(bool is_computed, void* raw_source, FlatPtr name, void* values, void (*append)(void*, StyleValueData const*))
{
    auto source = source_from_abi(is_computed, raw_source);
    auto property = property_from_abi(name);

    auto property_value = get_style_value(source, property);
    if (!property_value)
        return to_underlying(Iterations::Absent);

    if (is_computed && property_is_shorthand(property.id())) {
        append(values, into_rust(*property_value));
        return to_underlying(Iterations::Shorthand);
    }

    for (auto const& value : subdivide_property_value(*property_value, property, source.get_pointer<DOM::AbstractElement>()))
        append(values, into_rust(*value));
    return to_underlying(Iterations::Values);
}

bool web_css_style_map_has(bool is_computed, void* raw_source, FlatPtr name)
{
    auto source = source_from_abi(is_computed, raw_source);
    auto property = property_from_abi(name);
    return source.visit(
        [&property](DOM::AbstractElement& element) {
            if (!has_computed_style(element))
                return false;
            // From https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap we need to include:
            // "the name and computed value of every longhand CSS property supported by the User Agent, every
            // registered custom property, and every non-registered custom property which is not set to its initial
            // value on this"
            // Ensure style is computed on the element before we try to read it, so we can check custom properties.
            element.document().update_style_for_element(element);
            if (property.is_custom_property()) {
                if (element.get_custom_property(property.name()))
                    return true;
                return element.document().get_registered_custom_property(property.name()).has_value();
            }
            return true;
        },
        [&property](GC::Ref<CSSStyleDeclaration> const& declaration) {
            return declaration->has_property(property);
        });
}

static HashTable<Utf16FlyString> computed_custom_property_names(DOM::AbstractElement const& element)
{
    HashTable<Utf16FlyString> custom_properties;
    if (auto custom_property_data = element.custom_property_data()) {
        custom_property_data->for_each_property([&](auto const& name, auto const& property) {
            if (!property.value->is_guaranteed_invalid())
                custom_properties.set(name);
        });
    }
    element.document().for_each_registered_custom_property([&](CustomPropertyRegistration const& registration) {
        custom_properties.set(registration.property_name);
    });
    return custom_properties;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
void web_css_style_map_property_names(bool is_computed, void* raw_source, void* names, void (*append)(void*, FlatPtr))
{
    source_from_abi(is_computed, raw_source)
        .visit(
            [&](DOM::AbstractElement const& element) {
                if (!has_computed_style(element))
                    return;

                element.document().update_style_for_element(element);
                for (auto id = to_underlying(first_longhand_property_id); id <= to_underlying(last_longhand_property_id); ++id)
                    append(names, string_from_property_id(static_cast<PropertyID>(id)).to_utf16_string().into_raw());
                for (auto const& name : computed_custom_property_names(element))
                    append(names, name.to_utf16_string().into_raw());
            },
            [&](GC::Ref<CSSStyleDeclaration> const& declaration) {
                for (size_t i = 0; i < declaration->length(); ++i)
                    append(names, declaration->item(i).into_raw());
            });
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-size
size_t web_css_style_map_property_count(bool is_computed, void* raw_source)
{
    return source_from_abi(is_computed, raw_source)
        .visit(
            [](DOM::AbstractElement const& element) -> size_t {
                if (!has_computed_style(element))
                    return 0;

                element.document().update_style_for_element(element);
                return number_of_longhand_properties + computed_custom_property_names(element).size();
            },
            [](GC::Ref<CSSStyleDeclaration> const& declaration) -> size_t {
                return declaration->length();
            });
}

bool web_css_style_map_property_is_list_valued(u16 property_id)
{
    return property_is_list_valued(static_cast<PropertyID>(property_id));
}

StyleValueData const* web_css_style_map_parse(void* raw_declaration, u16 property_id, FlatPtr css_text)
{
    auto text = Utf16String::adopt_raw(css_text);
    auto parsing_params = Parser::ParsingParams {};
    if (raw_declaration) {
        if (auto owner_node = declaration_from_abi(raw_declaration).owner_node(); owner_node.has_value())
            parsing_params = Parser::ParsingParams { owner_node->element().document() };
    }
    auto value = parse_css_value(parsing_params, text, static_cast<PropertyID>(property_id));
    return value ? into_rust(*value) : nullptr;
}

StyleValueData const* web_css_style_map_declared_value(void* declaration, FlatPtr name)
{
    auto value = declaration_from_abi(declaration).get_property_style_value(property_from_abi(name));
    return value ? into_rust(*value) : nullptr;
}

u8 web_css_style_map_set_value(void* declaration, FlatPtr name, StyleValueData const* value, FlatPtr* message)
{
    return change_result(declaration_from_abi(declaration).set_property_style_value(property_from_abi(name), StyleValue::adopt_rust_style_value_data(value)), message);
}

u8 web_css_style_map_remove(void* declaration, FlatPtr name, FlatPtr* message)
{
    return change_result(declaration_from_abi(declaration).remove_property(Utf16FlyString { Utf16String::adopt_raw(name) }), message);
}

u8 web_css_style_map_clear(void* declaration, FlatPtr* message)
{
    return change_result(declaration_from_abi(declaration).set_css_text(""sv), message);
}

}
