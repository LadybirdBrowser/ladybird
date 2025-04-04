/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Parser/Parser.h"

#include <LibWeb/Bindings/CSSFontFaceDescriptorsPrototype.h>
#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFontFaceDescriptors);

GC::Ref<CSSFontFaceDescriptors> CSSFontFaceDescriptors::create(JS::Realm& realm, Vector<Descriptor> descriptors)
{
    return realm.create<CSSFontFaceDescriptors>(realm, move(descriptors));
}

CSSFontFaceDescriptors::CSSFontFaceDescriptors(JS::Realm& realm, Vector<Descriptor> descriptors)
    : CSSStyleDeclaration(realm, Computed::No, Readonly::No)
    , m_descriptors(move(descriptors))
{
}

CSSFontFaceDescriptors::~CSSFontFaceDescriptors() = default;

void CSSFontFaceDescriptors::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFontFaceDescriptors);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
size_t CSSFontFaceDescriptors::length() const
{
    // The length attribute must return the number of CSS declarations in the declarations.
    return m_descriptors.size();
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-item
String CSSFontFaceDescriptors::item(size_t index) const
{
    // The item(index) method must return the property name of the CSS declaration at position index.
    if (index >= length())
        return {};

    return to_string(m_descriptors[index].descriptor_id).to_string();
}

// https://drafts.csswg.org/cssom/#set-a-css-declaration
bool CSSFontFaceDescriptors::set_a_css_declaration(DescriptorID descriptor_id, NonnullRefPtr<CSSStyleValue const> value, Important)
{
    VERIFY(!is_computed());

    for (auto& descriptor : m_descriptors) {
        if (descriptor.descriptor_id == descriptor_id) {
            if (*descriptor.value == *value)
                return false;
            descriptor.value = move(value);
            return true;
        }
    }

    m_descriptors.append(Descriptor {
        .descriptor_id = descriptor_id,
        .value = move(value),
    });
    return true;
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_property(StringView property, StringView value, StringView priority)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties of readonly CSSFontFaceDescriptors"_string);

    // 2. If property is not a custom property, follow these substeps:
    Optional<DescriptorID> descriptor_id;
    {
        // 1. Let property be property converted to ASCII lowercase.
        // 2. If property is not a case-sensitive match for a supported CSS property, then return.
        descriptor_id = descriptor_id_from_string(AtRuleID::FontFace, property);
        if (!descriptor_id.has_value())
            return {};
    }

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        MUST(remove_property(property));
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !Infra::is_ascii_case_insensitive_match(priority, "important"sv))
        return {};

    // 5. Let component value list be the result of parsing value for property property.
    RefPtr<CSSStyleValue> component_value_list = parse_css_descriptor(Parser::ParsingParams {}, AtRuleID::FontFace, *descriptor_id, value);

    // 6. If component value list is null, then return.
    if (!component_value_list)
        return {};

    // 7. Let updated be false.
    auto updated = false;

    // 8. If property is a shorthand property...
    // NB: Descriptors can't be shorthands.
    // 9. Otherwise, let updated be the result of set the CSS declaration property with value component value list,
    //    with the important flag set if priority is not the empty string, and unset otherwise, and with the list of
    //    declarations being the declarations.
    updated = set_a_css_declaration(*descriptor_id, *component_value_list, !priority.is_empty() ? Important::Yes : Important::No);

    // 10. If updated is true, update style attribute for the CSS declaration block.
    if (updated)
        update_style_attribute();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> CSSFontFaceDescriptors::remove_property(StringView property)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties of readonly CSSFontFaceDescriptors"_string);

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // AD-HOC: We compare names case-insensitively instead.

    // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
    auto value = get_property_value(property);

    // 4. Let removed be false.
    bool removed = false;

    // 5. If property is a shorthand property...
    // NB: Descriptors can't be shorthands.
    // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the
    //    declarations, remove that CSS declaration and let removed be true.
    // auto descriptor_id = descriptor_from_string()
    auto descriptor_id = descriptor_id_from_string(AtRuleID::FontFace, property);
    if (descriptor_id.has_value()) {
        removed = m_descriptors.remove_first_matching([descriptor_id](auto& entry) { return entry.descriptor_id == *descriptor_id; });
    }

    // 7. If removed is true, Update style attribute for the CSS declaration block.
    if (removed)
        update_style_attribute();

    // 8. Return value.
    return value;
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
String CSSFontFaceDescriptors::get_property_value(StringView property) const
{
    // 1. If property is not a custom property, follow these substeps: ...
    // NB: These substeps only apply to shorthands, and descriptors cannot be shorthands.

    // 2. If property is a case-sensitive match for a property name of a CSS declaration in the declarations, then
    //    return the result of invoking serialize a CSS value of that declaration.
    auto descriptor_id = descriptor_id_from_string(AtRuleID::FontFace, property);
    if (descriptor_id.has_value()) {
        auto match = m_descriptors.first_matching([descriptor_id](auto& entry) { return entry.descriptor_id == *descriptor_id; });
        if (match.has_value())
            return match->value->to_string(CSSStyleValue::SerializationMode::Normal);
    }

    // 3. Return the empty string.
    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
StringView CSSFontFaceDescriptors::get_property_priority(StringView) const
{
    // AD-HOC: It's not valid for descriptors to be !important.
    return {};
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration-block
String CSSFontFaceDescriptors::serialized() const
{
    // 1. Let list be an empty array.
    Vector<String> list;
    list.ensure_capacity(m_descriptors.size());

    // 2. Let already serialized be an empty array.
    // AD-HOC: Not needed as we don't have shorthands.

    // 3. Declaration loop: For each CSS declaration declaration in declaration block’s declarations, follow these substeps:
    for (auto const& descriptor : m_descriptors) {
        // 1. Let property be declaration’s property name.
        auto property = to_string(descriptor.descriptor_id);

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        // AD-HOC: Not needed as we don't have shorthands.

        // 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in preferred order.
        // 4. Shorthand loop: For each shorthand in shorthands, follow these substeps: ...
        // NB: Descriptors can't be shorthands.

        // 5. Let value be the result of invoking serialize a CSS value of declaration.
        auto value = descriptor.value->to_string(CSSStyleValue::SerializationMode::Normal);

        // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value, and the important flag set if declaration has its important flag set.
        auto serialized_declaration = serialize_a_css_declaration(property, value, Important::No);

        // 7. Append serialized declaration to list.
        list.append(serialized_declaration);

        // 8. Append property to already serialized.
        // AD-HOC: Not needed as we don't have shorthands.
    }

    // 4. Return list joined with " " (U+0020).
    return MUST(String::join(' ', list));
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_css_text(StringView value)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties of readonly CSSFontFaceDescriptors"_string);

    // 2. Empty the declarations.
    m_descriptors.clear();

    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the
    //    declarations, in specified order.
    auto descriptors = parse_css_list_of_descriptors(Parser::ParsingParams {}, AtRuleID::FontFace, value);
    if (!descriptors.is_empty())
        m_descriptors = move(descriptors);

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    return {};
}

void CSSFontFaceDescriptors::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& descriptor : m_descriptors) {
        descriptor.value->visit_edges(visitor);
    }
}

RefPtr<CSSStyleValue const> CSSFontFaceDescriptors::descriptor(DescriptorID descriptor_id) const
{
    auto match = m_descriptors.first_matching([descriptor_id](Descriptor const& descriptor) {
        return descriptor.descriptor_id == descriptor_id;
    });
    if (match.has_value())
        return match->value;
    return nullptr;
}

RefPtr<CSSStyleValue const> CSSFontFaceDescriptors::descriptor_or_initial_value(DescriptorID descriptor_id) const
{
    if (auto value = descriptor(descriptor_id))
        return value.release_nonnull();

    return descriptor_initial_value(AtRuleID::FontFace, descriptor_id);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_ascent_override(StringView value)
{
    return set_property("ascent-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::ascent_override() const
{
    return get_property_value("ascent-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_descent_override(StringView value)
{
    return set_property("descent-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::descent_override() const
{
    return get_property_value("descent-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_display(StringView value)
{
    return set_property("font-display"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_display() const
{
    return get_property_value("font-display"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_family(StringView value)
{
    return set_property("font-family"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_family() const
{
    return get_property_value("font-family"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_feature_settings(StringView value)
{
    return set_property("font-feature-settings"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_feature_settings() const
{
    return get_property_value("font-feature-settings"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_language_override(StringView value)
{
    return set_property("font-language-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_language_override() const
{
    return get_property_value("font-language-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_named_instance(StringView value)
{
    return set_property("font-named-instance"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_named_instance() const
{
    return get_property_value("font-named-instance"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_style(StringView value)
{
    return set_property("font-style"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_style() const
{
    return get_property_value("font-style"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_variation_settings(StringView value)
{
    return set_property("font-variation-settings"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_variation_settings() const
{
    return get_property_value("font-variation-settings"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_weight(StringView value)
{
    return set_property("font-weight"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_weight() const
{
    return get_property_value("font-weight"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_font_width(StringView value)
{
    return set_property("font-width"sv, value, ""sv);
}

String CSSFontFaceDescriptors::font_width() const
{
    return get_property_value("font-width"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_line_gap_override(StringView value)
{
    return set_property("line-gap-override"sv, value, ""sv);
}

String CSSFontFaceDescriptors::line_gap_override() const
{
    return get_property_value("line-gap-override"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_src(StringView value)
{
    return set_property("src"sv, value, ""sv);
}

String CSSFontFaceDescriptors::src() const
{
    return get_property_value("src"sv);
}

WebIDL::ExceptionOr<void> CSSFontFaceDescriptors::set_unicode_range(StringView value)
{
    return set_property("unicode-range"sv, value, ""sv);
}

String CSSFontFaceDescriptors::unicode_range() const
{
    return get_property_value("unicode-range"sv);
}

}
