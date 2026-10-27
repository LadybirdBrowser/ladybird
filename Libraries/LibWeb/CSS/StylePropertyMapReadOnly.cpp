/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StylePropertyMapReadOnly.h"
#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/Zoom.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StylePropertyMapReadOnly);

GC::Ref<StylePropertyMapReadOnly> StylePropertyMapReadOnly::create_computed_style(DOM::AbstractElement element)
{
    return GC::Heap::the().allocate<StylePropertyMapReadOnly>(element);
}

StylePropertyMapReadOnly::StylePropertyMapReadOnly(Source source)
    : m_declarations(move(source))
{
}

StylePropertyMapReadOnly::~StylePropertyMapReadOnly() = default;

void StylePropertyMapReadOnly::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_document);

    m_declarations.visit(
        [&visitor](DOM::AbstractElement& element) { element.visit(visitor); },
        [&visitor](GC::Ref<CSSStyleDeclaration>& declaration) { visitor.visit(declaration); });
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

static GC::Ref<CSSStyleValue> reify_property_value(StyleValue const& value, PropertyNameAndID const& property, bool is_computed)
{
    // INTEROP: Computed border-image values represent all four sides or both axes, even when their serialization
    //          collapses to a single numeric value or keyword.
    if (is_computed && first_is_one_of(property.id(), PropertyID::BorderImageWidth, PropertyID::BorderImageOutset, PropertyID::BorderImageSlice, PropertyID::BorderImageRepeat))
        return value.default_reify(property.name());
    return value.reify(property.name());
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-get
WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, Empty>> StylePropertyMapReadOnly::get(Utf16String property_name)
{
    // The get(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property_name_utf16 = Utf16FlyString::from_utf16(property_name.utf16_view());
    auto property = PropertyNameAndID::from_name(property_name_utf16);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("'{}' is not a valid CSS property", property_name) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // 4. If props[property] exists, subdivide into iterations props[property], then reify the first item of the result and return it.
    if (auto property_value = get_style_value(props, property.value())) {
        if (props.has<DOM::AbstractElement>() && property_is_shorthand(property->id()))
            return CSSStyleValue::create(property->name(), *property_value, SerializationMode::ResolvedValue);
        auto iterations = subdivide_property_value(*property_value, property.value(), props.get_pointer<DOM::AbstractElement>());
        return reify_property_value(*iterations.first(), property.value(), props.has<DOM::AbstractElement>());
    }

    // 5. Otherwise, return undefined.
    return Empty {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-getall
WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> StylePropertyMapReadOnly::get_all(Utf16String property_name)
{
    // The getAll(property) method, when called on a StylePropertyMap this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property_name_utf16 = Utf16FlyString::from_utf16(property_name.utf16_view());
    auto property = PropertyNameAndID::from_name(property_name_utf16);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("'{}' is not a valid CSS property", property_name) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    GC::RootVector<GC::Ref<CSSStyleValue>> results;

    // 4. If props[property] exists, subdivide into iterations props[property], then reify each item of the result, and return the list.
    if (auto property_value = get_style_value(props, property.value())) {
        if (props.has<DOM::AbstractElement>() && property_is_shorthand(property->id())) {
            results.append(CSSStyleValue::create(property->name(), *property_value, SerializationMode::ResolvedValue));
            return results;
        }
        auto iterations = subdivide_property_value(*property_value, property.value(), props.get_pointer<DOM::AbstractElement>());
        for (auto const& style_value : iterations)
            results.append(reify_property_value(*style_value, property.value(), props.has<DOM::AbstractElement>()));
        return results;
    }

    // 5. Otherwise, return an empty list.
    return results;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-has
WebIDL::ExceptionOr<bool> StylePropertyMapReadOnly::has(Utf16String property_name)
{
    // The has(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property_name_utf16 = Utf16FlyString::from_utf16(property_name.utf16_view());
    auto property = PropertyNameAndID::from_name(property_name_utf16);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("'{}' is not a valid CSS property", property_name) };

    // 3. Let props be the value of this’s [[declarations]] internal slot.
    auto& props = m_declarations;

    // 4. If props[property] exists, return true. Otherwise, return false.
    return props.visit(
        [&property](DOM::AbstractElement& element) {
            if (!has_computed_style(element))
                return false;
            // From https://drafts.css-houdini.org/css-typed-om-1/#dom-element-computedstylemap we need to include:
            // "the name and computed value of every longhand CSS property supported by the User Agent, every
            // registered custom property, and every non-registered custom property which is not set to its initial
            // value on this"
            // Ensure style is computed on the element before we try to read it, so we can check custom properties.
            element.document().update_style_for_element(element);
            if (property->is_custom_property()) {
                if (element.get_custom_property(property->name()))
                    return true;
                return element.document().get_registered_custom_property(property->name()).has_value();
            }
            return true;
        },
        [&property](GC::Ref<CSSStyleDeclaration> const& declaration) {
            return declaration->has_property(property.value());
        });
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-size
WebIDL::UnsignedLong StylePropertyMapReadOnly::size() const
{
    // The size attribute, on getting from a StylePropertyMapReadOnly this, must perform the following steps:

    // 1. Return the size of the value of this’s [[declarations]] internal slot.
    return property_names().size();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
ReadonlySpan<Utf16FlyString> StylePropertyMapReadOnly::property_names() const
{
    if (auto* element = m_declarations.get_pointer<DOM::AbstractElement>(); element && !has_computed_style(*element))
        return {};

    auto needs_update = m_declarations.visit(
        [&](DOM::AbstractElement const& element) {
            element.document().update_style_for_element(element);
            auto custom_property_data = element.custom_property_data();
            auto& document = element.document();
            if (m_cached_document.ptr() == &document
                && m_cached_custom_property_data == custom_property_data
                && m_cached_registration_generation == document.custom_property_registration_generation()) {
                return false;
            }

            m_cached_document = document;
            m_cached_custom_property_data = move(custom_property_data);
            m_cached_registration_generation = document.custom_property_registration_generation();
            m_cached_property_names.clear_with_capacity();
            for (auto id = to_underlying(first_longhand_property_id); id <= to_underlying(last_longhand_property_id); ++id)
                m_cached_property_names.append(string_from_property_id(static_cast<PropertyID>(id)));

            HashTable<Utf16FlyString> custom_properties;
            if (m_cached_custom_property_data) {
                m_cached_custom_property_data->for_each_property([&](auto const& name, auto const& property) {
                    if (!property.value->is_guaranteed_invalid())
                        custom_properties.set(name);
                });
            }
            document.for_each_registered_custom_property([&](CustomPropertyRegistration const& registration) {
                custom_properties.set(registration.property_name);
            });
            for (auto const& name : custom_properties)
                m_cached_property_names.append(name);
            return true;
        },
        [&](GC::Ref<CSSStyleDeclaration> const& declaration) {
            // NB: Inline and declared maps are both backed by CSSStyleProperties.
            auto const& properties = as<CSSStyleProperties>(*declaration);
            if (m_cached_declaration_revision == properties.revision())
                return false;
            m_cached_declaration_revision = properties.revision();
            m_cached_property_names.clear_with_capacity();
            for (size_t i = 0; i < declaration->length(); ++i)
                m_cached_property_names.append(Utf16FlyString { declaration->item(i) });
            return true;
        });

    // INTEROP: Inline and declared maps preserve declaration order.
    if (needs_update && m_declarations.has<DOM::AbstractElement>()) {
        // Unless otherwise stated, the initial ordering of the [[declarations]] internal slot is based on the key of each entry:
        // 1. Standardized properties (not custom properties or vendor-prefixed properties), ASCII lowercased
        //    and then sorted in increasing code-point order.
        // 2. Vendor-prefixed/experimental properties (those whose name starts with a single dash), ASCII lowercased
        //    and then sorted in increasing code-point order.
        // 3. Custom properties, sorted in increasing code-point order. (These are never lower-cased; they are
        //    preserved exactly as written.)
        quick_sort(m_cached_property_names, [](auto const& left, auto const& right) {
            auto category = [](auto const& name) {
                if (name.starts_with("--"sv))
                    return 2;
                return name.starts_with("-"sv) ? 1 : 0;
            };
            if (category(left) != category(right))
                return category(left) < category(right);
            auto left_view = left.view();
            auto right_view = right.view();
            auto left_iterator = left_view.begin();
            auto right_iterator = right_view.begin();
            for (; left_iterator != left_view.end() && right_iterator != right_view.end(); ++left_iterator, ++right_iterator) {
                if (*left_iterator != *right_iterator)
                    return *left_iterator < *right_iterator;
            }
            return left_iterator == left_view.end() && right_iterator != right_view.end();
        });
    }
    return m_cached_property_names;
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
void StylePropertyMapReadOnly::collect_iteration_entries(Vector<IterationEntry>& entries, GC::RootVector<GC::Ref<CSSStyleValue>>& values)
{
    // The value pairs to iterate over for a StylePropertyMapReadOnly object this are obtained as follows:
    // 1. Let declarations be this’s [[declarations]] slot.
    // 2. Let value pairs be an empty list.
    // 3. For each prop → value in declarations:
    // NB: Copy the names, since reifying values may refresh the cached names.
    Vector<Utf16FlyString> names { property_names() };
    for (auto const& name : names) {
        // 1. Let iterations be the result of dividing into iterations value.
        // 2. Reify each item of iterations, and let objects be the result.
        auto objects = MUST(get_all(name.to_utf16_string()));
        // 3. Append prop/objects to value pairs.
        entries.append({ name, values.size(), objects.size() });
        for (auto object : objects)
            values.append(object);
    }
    // 4. Return value pairs.
}

// https://webidl.spec.whatwg.org/#es-iterable
void StylePropertyMapReadOnly::for_each(ForEachCallback callback)
{
    // INTEROP: Snapshot style-map entries before invoking callbacks, matching Chrome and Safari.
    Vector<IterationEntry> entries;
    GC::RootVector<GC::Ref<CSSStyleValue>> values;
    collect_iteration_entries(entries, values);
    for (auto const& entry : entries) {
        if (callback(entry.name.to_utf16_string(), values.span().slice(entry.value_offset, entry.value_count)) == IterationDecision::Break)
            break;
    }
}

RefPtr<StyleValue const> StylePropertyMapReadOnly::get_style_value(Source& source, PropertyNameAndID const& property)
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

}
