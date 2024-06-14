/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSStyleDeclarationPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSStyleDeclaration);
JS_DEFINE_ALLOCATOR(PropertyOwningCSSStyleDeclaration);
JS_DEFINE_ALLOCATOR(ElementInlineCSSStyleDeclaration);

CSSStyleDeclaration::CSSStyleDeclaration(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void CSSStyleDeclaration::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleDeclaration);
}

JS::GCPtr<CSSRule> CSSStyleDeclaration::parent_rule() const
{
    return nullptr;
}

JS::NonnullGCPtr<PropertyOwningCSSStyleDeclaration> PropertyOwningCSSStyleDeclaration::create(JS::Realm& realm, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    return realm.heap().allocate<PropertyOwningCSSStyleDeclaration>(realm, realm, move(properties), move(custom_properties));
}

PropertyOwningCSSStyleDeclaration::PropertyOwningCSSStyleDeclaration(JS::Realm& realm, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
    : CSSStyleDeclaration(realm)
    , m_properties(move(properties))
    , m_custom_properties(move(custom_properties))
{
}

void PropertyOwningCSSStyleDeclaration::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_rule);
    for (auto& property : m_properties) {
        if (property.value->is_image())
            property.value->as_image().visit_edges(visitor);
    }
}

String PropertyOwningCSSStyleDeclaration::item(size_t index) const
{
    if (index >= m_properties.size())
        return {};
    return CSS::string_from_property_id(m_properties[index].property_id).to_string();
}

JS::NonnullGCPtr<ElementInlineCSSStyleDeclaration> ElementInlineCSSStyleDeclaration::create(DOM::Element& element, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    auto& realm = element.realm();
    return realm.heap().allocate<ElementInlineCSSStyleDeclaration>(realm, element, move(properties), move(custom_properties));
}

ElementInlineCSSStyleDeclaration::ElementInlineCSSStyleDeclaration(DOM::Element& element, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
    : PropertyOwningCSSStyleDeclaration(element.realm(), move(properties), move(custom_properties))
    , m_element(element.make_weak_ptr<DOM::Element>())
{
}

void ElementInlineCSSStyleDeclaration::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

size_t PropertyOwningCSSStyleDeclaration::length() const
{
    return m_properties.size();
}

Optional<StyleProperty> PropertyOwningCSSStyleDeclaration::property(PropertyID property_id) const
{
    for (auto& property : m_properties) {
        if (property.property_id == property_id)
            return property;
    }
    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> PropertyOwningCSSStyleDeclaration::set_property(PropertyID property_id, StringView value, StringView priority)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    // NOTE: This is handled by the virtual override in ResolvedCSSStyleDeclaration.

    // FIXME: 2. If property is not a custom property, follow these substeps:
    // FIXME:    1. Let property be property converted to ASCII lowercase.
    // FIXME:    2. If property is not a case-sensitive match for a supported CSS property, then return.
    // NOTE: This must be handled before we've turned the property string into a PropertyID.

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        MUST(remove_property(property_id));
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !Infra::is_ascii_case_insensitive_match(priority, "important"sv))
        return {};

    // 5. Let component value list be the result of parsing value for property property.
    auto component_value_list = is<ElementInlineCSSStyleDeclaration>(this)
        ? parse_css_value(CSS::Parser::ParsingContext { static_cast<ElementInlineCSSStyleDeclaration&>(*this).element()->document() }, value, property_id)
        : parse_css_value(CSS::Parser::ParsingContext { realm() }, value, property_id);

    // 6. If component value list is null, then return.
    if (!component_value_list)
        return {};

    // 7. Let updated be false.
    bool updated = false;

    // FIXME: 8. If property is a shorthand property, then for each longhand property longhand that property maps to, in canonical order, follow these substeps:
    // FIXME:    1. Let longhand result be the result of set the CSS declaration longhand with the appropriate value(s) from component value list,
    //              with the important flag set if priority is not the empty string, and unset otherwise, and with the list of declarations being the declarations.
    // FIXME:    2. If longhand result is true, let updated be true.

    // 9. Otherwise, let updated be the result of set the CSS declaration property with value component value list,
    //    with the important flag set if priority is not the empty string, and unset otherwise,
    //    and with the list of declarations being the declarations.
    updated = set_a_css_declaration(property_id, component_value_list.release_nonnull(), !priority.is_empty() ? Important::Yes : Important::No);

    // 10. If updated is true, update style attribute for the CSS declaration block.
    if (updated)
        update_style_attribute();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> PropertyOwningCSSStyleDeclaration::remove_property(PropertyID property_id)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    // NOTE: This is handled by the virtual override in ResolvedCSSStyleDeclaration.

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // NOTE: We've already converted it to a PropertyID enum value.

    // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
    // FIXME: The trip through string_from_property_id() here is silly.
    auto value = get_property_value(string_from_property_id(property_id));

    // 4. Let removed be false.
    bool removed = false;

    // FIXME: 5. If property is a shorthand property, for each longhand property longhand that property maps to:
    //           1. If longhand is not a property name of a CSS declaration in the declarations, continue.
    //           2. Remove that CSS declaration and let removed be true.

    // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the declarations, remove that CSS declaration and let removed be true.
    removed = m_properties.remove_first_matching([&](auto& entry) { return entry.property_id == property_id; });

    // 7. If removed is true, Update style attribute for the CSS declaration block.
    if (removed)
        update_style_attribute();

    // 8. Return value.
    return value;
}

// https://drafts.csswg.org/cssom/#update-style-attribute-for
void ElementInlineCSSStyleDeclaration::update_style_attribute()
{
    // 1. Assert: declaration block’s computed flag is unset.
    // NOTE: Unnecessary, only relevant for ResolvedCSSStyleDeclaration.

    // 2. Let owner node be declaration block’s owner node.
    // 3. If owner node is null, then return.
    if (!m_element)
        return;

    // 4. Set declaration block’s updating flag.
    m_updating = true;

    // 5. Set an attribute value for owner node using "style" and the result of serializing declaration block.
    MUST(m_element->set_attribute(HTML::AttributeNames::style, serialized()));

    // 6. Unset declaration block’s updating flag.
    m_updating = false;
}

// https://drafts.csswg.org/cssom/#set-a-css-declaration
bool PropertyOwningCSSStyleDeclaration::set_a_css_declaration(PropertyID property_id, NonnullRefPtr<StyleValue const> value, Important important)
{
    // FIXME: Handle logical property groups.

    for (auto& property : m_properties) {
        if (property.property_id == property_id) {
            if (property.important == important && *property.value == *value)
                return false;
            property.value = move(value);
            property.important = important;
            return true;
        }
    }

    m_properties.append(CSS::StyleProperty {
        .important = important,
        .property_id = property_id,
        .value = move(value),
    });
    return true;
}

String CSSStyleDeclaration::get_property_value(StringView property_name) const
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return {};
    auto maybe_property = property(property_id.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->value->to_string();
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
StringView CSSStyleDeclaration::get_property_priority(StringView property_name) const
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return {};
    auto maybe_property = property(property_id.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->important == Important::Yes ? "important"sv : ""sv;
}

WebIDL::ExceptionOr<void> CSSStyleDeclaration::set_property(StringView property_name, StringView css_text, StringView priority)
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return {};
    return set_property(property_id.value(), css_text, priority);
}

WebIDL::ExceptionOr<String> CSSStyleDeclaration::remove_property(StringView property_name)
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return String {};
    return remove_property(property_id.value());
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
String CSSStyleDeclaration::css_text() const
{
    // 1. If the computed flag is set, then return the empty string.
    // NOTE: See ResolvedCSSStyleDeclaration::serialized()

    // 2. Return the result of serializing the declarations.
    return serialized();
}

// https://www.w3.org/TR/cssom/#serialize-a-css-declaration
static String serialize_a_css_declaration(CSS::PropertyID property, StringView value, Important important)
{
    StringBuilder builder;

    // 1. Let s be the empty string.
    // 2. Append property to s.
    builder.append(string_from_property_id(property));

    // 3. Append ": " (U+003A U+0020) to s.
    builder.append(": "sv);

    // 4. Append value to s.
    builder.append(value);

    // 5. If the important flag is set, append " !important" (U+0020 U+0021 U+0069 U+006D U+0070 U+006F U+0072 U+0074 U+0061 U+006E U+0074) to s.
    if (important == Important::Yes)
        builder.append(" !important"sv);

    // 6. Append ";" (U+003B) to s.
    builder.append(';');

    // 7. Return s.
    return MUST(builder.to_string());
}

// https://www.w3.org/TR/cssom/#serialize-a-css-declaration-block
String PropertyOwningCSSStyleDeclaration::serialized() const
{
    // 1. Let list be an empty array.
    Vector<String> list;

    // 2. Let already serialized be an empty array.
    HashTable<PropertyID> already_serialized;

    // NOTE: The spec treats custom properties the same as any other property, and expects the above loop to handle them.
    //       However, our implementation separates them from regular properties, so we need to handle them separately here.
    // FIXME: Is the relative order of custom properties and regular properties supposed to be preserved?
    for (auto& declaration : m_custom_properties) {
        // 1. Let property be declaration’s property name.
        auto const& property = declaration.key;

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        // NOTE: It is never in already serialized, as there are no shorthands for custom properties.

        // 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in preferred order.
        // NOTE: There are no shorthands for custom properties.

        // 4. Shorthand loop: For each shorthand in shorthands, follow these substeps: ...
        // NOTE: There are no shorthands for custom properties.

        // 5. Let value be the result of invoking serialize a CSS value of declaration.
        auto value = declaration.value.value->to_string();

        // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value,
        //    and the important flag set if declaration has its important flag set.
        // NOTE: We have to inline this here as the actual implementation does not accept custom properties.
        String serialized_declaration = [&] {
            // https://www.w3.org/TR/cssom/#serialize-a-css-declaration
            StringBuilder builder;

            // 1. Let s be the empty string.
            // 2. Append property to s.
            builder.append(property);

            // 3. Append ": " (U+003A U+0020) to s.
            builder.append(": "sv);

            // 4. Append value to s.
            builder.append(value);

            // 5. If the important flag is set, append " !important" (U+0020 U+0021 U+0069 U+006D U+0070 U+006F U+0072 U+0074 U+0061 U+006E U+0074) to s.
            if (declaration.value.important == Important::Yes)
                builder.append(" !important"sv);

            // 6. Append ";" (U+003B) to s.
            builder.append(';');

            // 7. Return s.
            return MUST(builder.to_string());
        }();

        // 7. Append serialized declaration to list.
        list.append(move(serialized_declaration));

        // 8. Append property to already serialized.
        // NOTE: We don't need to do this, as we don't have shorthands for custom properties.
    }

    // 3. Declaration loop: For each CSS declaration declaration in declaration block’s declarations, follow these substeps:
    for (auto& declaration : m_properties) {
        // 1. Let property be declaration’s property name.
        auto property = declaration.property_id;

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        if (already_serialized.contains(property))
            continue;

        // FIXME: 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in preferred order.

        // FIXME: 4. Shorthand loop: For each shorthand in shorthands, follow these substeps: ...

        // 5. Let value be the result of invoking serialize a CSS value of declaration.
        auto value = declaration.value->to_string();

        // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value,
        //    and the important flag set if declaration has its important flag set.
        auto serialized_declaration = serialize_a_css_declaration(property, move(value), declaration.important);

        // 7. Append serialized declaration to list.
        list.append(move(serialized_declaration));

        // 8. Append property to already serialized.
        already_serialized.set(property);
    }

    // 4. Return list joined with " " (U+0020).
    StringBuilder builder;
    builder.join(' ', list);
    return MUST(builder.to_string());
}

static CSS::PropertyID property_id_from_name(StringView name)
{
    // FIXME: Perhaps this should go in the code generator.
    if (name == "cssFloat"sv)
        return CSS::PropertyID::Float;

    if (auto property_id = CSS::property_id_from_camel_case_string(name); property_id.has_value())
        return property_id.value();

    if (auto property_id = CSS::property_id_from_string(name); property_id.has_value())
        return property_id.value();

    return CSS::PropertyID::Invalid;
}

JS::ThrowCompletionOr<bool> CSSStyleDeclaration::internal_has_property(JS::PropertyKey const& name) const
{
    if (!name.is_string())
        return Base::internal_has_property(name);
    return property_id_from_name(name.to_string()) != CSS::PropertyID::Invalid;
}

JS::ThrowCompletionOr<JS::Value> CSSStyleDeclaration::internal_get(JS::PropertyKey const& name, JS::Value receiver, JS::CacheablePropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    if (name.is_number())
        return { JS::PrimitiveString::create(vm(), item(name.as_number())) };
    if (!name.is_string())
        return Base::internal_get(name, receiver, cacheable_metadata, phase);
    auto property_id = property_id_from_name(name.to_string());
    if (property_id == CSS::PropertyID::Invalid)
        return Base::internal_get(name, receiver, cacheable_metadata, phase);
    if (auto maybe_property = property(property_id); maybe_property.has_value())
        return { JS::PrimitiveString::create(vm(), maybe_property->value->to_string()) };
    return { JS::PrimitiveString::create(vm(), String {}) };
}

JS::ThrowCompletionOr<bool> CSSStyleDeclaration::internal_set(JS::PropertyKey const& name, JS::Value value, JS::Value receiver, JS::CacheablePropertyMetadata* cacheable_metadata)
{
    auto& vm = this->vm();
    if (!name.is_string())
        return Base::internal_set(name, value, receiver, cacheable_metadata);
    auto property_id = property_id_from_name(name.to_string());
    if (property_id == CSS::PropertyID::Invalid)
        return Base::internal_set(name, value, receiver, cacheable_metadata);

    auto css_text = value.is_null() ? String {} : TRY(value.to_string(vm));

    TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return set_property(property_id, css_text); }));
    return true;
}

WebIDL::ExceptionOr<void> PropertyOwningCSSStyleDeclaration::set_css_text(StringView css_text)
{
    dbgln("(STUBBED) PropertyOwningCSSStyleDeclaration::set_css_text(css_text='{}')", css_text);
    return {};
}

void PropertyOwningCSSStyleDeclaration::empty_the_declarations()
{
    m_properties.clear();
    m_custom_properties.clear();
}

void PropertyOwningCSSStyleDeclaration::set_the_declarations(Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    m_properties = move(properties);
    m_custom_properties = move(custom_properties);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> ElementInlineCSSStyleDeclaration::set_css_text(StringView css_text)
{
    // FIXME: What do we do if the element is null?
    if (!m_element) {
        dbgln("FIXME: Returning from ElementInlineCSSStyleDeclaration::set_css_text as m_element is null.");
        return {};
    }

    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    // NOTE: See ResolvedCSSStyleDeclaration.

    // 2. Empty the declarations.
    empty_the_declarations();

    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the declarations, in specified order.
    auto style = parse_css_style_attribute(CSS::Parser::ParsingContext(m_element->document()), css_text, *m_element.ptr());
    auto custom_properties = TRY_OR_THROW_OOM(vm(), style->custom_properties().clone());
    set_the_declarations(style->properties(), move(custom_properties));

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    return {};
}

JS::GCPtr<CSSRule> PropertyOwningCSSStyleDeclaration::parent_rule() const
{
    return m_parent_rule;
}

void PropertyOwningCSSStyleDeclaration::set_parent_rule(JS::NonnullGCPtr<CSSRule> rule)
{
    m_parent_rule = rule;
}

}
