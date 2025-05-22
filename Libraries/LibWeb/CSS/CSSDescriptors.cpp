/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSDescriptors.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

CSSDescriptors::CSSDescriptors(JS::Realm& realm, AtRuleID at_rule_id, Vector<Descriptor> descriptors)
    : CSSStyleDeclaration(realm, Computed::No, Readonly::No)
    , m_at_rule_id(at_rule_id)
    , m_descriptors(move(descriptors))
{
}

CSSDescriptors::~CSSDescriptors() = default;

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
size_t CSSDescriptors::length() const
{
    // The length attribute must return the number of CSS declarations in the declarations.
    return m_descriptors.size();
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-item
String CSSDescriptors::item(size_t index) const
{
    // The item(index) method must return the property name of the CSS declaration at position index.
    if (index >= length())
        return {};

    return to_string(m_descriptors[index].descriptor_id).to_string();
}

// https://drafts.csswg.org/cssom/#set-a-css-declaration
bool CSSDescriptors::set_a_css_declaration(DescriptorID descriptor_id, NonnullRefPtr<CSSStyleValue const> value, Important)
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
WebIDL::ExceptionOr<void> CSSDescriptors::set_property(StringView property, StringView value, StringView priority)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties of readonly CSSStyleDeclaration"_string);

    // 2. If property is not a custom property, follow these substeps:
    Optional<DescriptorID> descriptor_id;
    {
        // 1. Let property be property converted to ASCII lowercase.
        // 2. If property is not a case-sensitive match for a supported CSS property, then return.
        descriptor_id = descriptor_id_from_string(m_at_rule_id, property);
        if (!descriptor_id.has_value())
            return {};
    }

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        MUST(remove_property(property));
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !priority.equals_ignoring_ascii_case("important"sv))
        return {};

    // 5. Let component value list be the result of parsing value for property property.
    RefPtr<CSSStyleValue const> component_value_list = parse_css_descriptor(Parser::ParsingParams {}, m_at_rule_id, *descriptor_id, value);

    // 6. If component value list is null, then return.
    if (!component_value_list)
        return {};

    // 7. Let updated be false.
    auto updated = false;

    // 8. If property is a shorthand property, then for each longhand property longhand that property maps to, in canonical order, follow these substeps:
    if (is_shorthand(m_at_rule_id, *descriptor_id)) {
        for_each_expanded_longhand(m_at_rule_id, *descriptor_id, component_value_list, [this, &updated, priority](DescriptorID longhand_id, auto longhand_value) {
            VERIFY(longhand_value);

            // 1. Let longhand result be the result of set the CSS declaration longhand with the appropriate value(s)
            //    from component value list, with the important flag set if priority is not the empty string, and unset
            //    otherwise, and with the list of declarations being the declarations.
            auto longhand_result = set_a_css_declaration(longhand_id, longhand_value.release_nonnull(), priority.is_empty() ? Important::No : Important::Yes);

            // 2. If longhand result is true, let updated be true.
            if (longhand_result)
                updated = true;
        });
    }
    // 9. Otherwise, let updated be the result of set the CSS declaration property with value component value list,
    //    with the important flag set if priority is not the empty string, and unset otherwise, and with the list of
    //    declarations being the declarations.
    else {
        updated = set_a_css_declaration(*descriptor_id, *component_value_list, !priority.is_empty() ? Important::Yes : Important::No);
    }

    // 10. If updated is true, update style attribute for the CSS declaration block.
    if (updated)
        update_style_attribute();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> CSSDescriptors::remove_property(StringView property)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties of readonly CSSStyleDeclaration"_string);

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // AD-HOC: We compare names case-insensitively instead.

    // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
    auto value = get_property_value(property);

    // 4. Let removed be false.
    bool removed = false;
    auto descriptor_id = descriptor_id_from_string(m_at_rule_id, property);

    // 5. If property is a shorthand property, for each longhand property longhand that property maps to:
    if (descriptor_id.has_value() && is_shorthand(m_at_rule_id, *descriptor_id)) {
        for_each_expanded_longhand(m_at_rule_id, *descriptor_id, nullptr, [this, &removed](DescriptorID longhand_id, auto const&) {
            // 1. If longhand is not a property name of a CSS declaration in the declarations, continue.
            // 2. Remove that CSS declaration and let removed be true.
            if (m_descriptors.remove_first_matching([longhand_id](auto& entry) { return entry.descriptor_id == longhand_id; })) {
                removed = true;
            }
        });
    }
    // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the
    //    declarations, remove that CSS declaration and let removed be true.
    else if (descriptor_id.has_value()) {
        removed = m_descriptors.remove_first_matching([descriptor_id](auto& entry) { return entry.descriptor_id == *descriptor_id; });
    }

    // 7. If removed is true, Update style attribute for the CSS declaration block.
    if (removed)
        update_style_attribute();

    // 8. Return value.
    return value;
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
String CSSDescriptors::get_property_value(StringView property) const
{
    // 1. If property is not a custom property, follow these substeps: ...
    // NB: These substeps only apply to shorthands, and descriptors cannot be shorthands.

    // 2. If property is a case-sensitive match for a property name of a CSS declaration in the declarations, then
    //    return the result of invoking serialize a CSS value of that declaration.
    auto descriptor_id = descriptor_id_from_string(m_at_rule_id, property);
    if (descriptor_id.has_value()) {
        auto match = m_descriptors.first_matching([descriptor_id](auto& entry) { return entry.descriptor_id == *descriptor_id; });
        if (match.has_value())
            return match->value->to_string(SerializationMode::Normal);
    }

    // 3. Return the empty string.
    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
StringView CSSDescriptors::get_property_priority(StringView) const
{
    // AD-HOC: It's not valid for descriptors to be !important.
    return {};
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration-block
String CSSDescriptors::serialized() const
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
        auto value = descriptor.value->to_string(SerializationMode::Normal);

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
WebIDL::ExceptionOr<void> CSSDescriptors::set_css_text(StringView value)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties of readonly CSSStyleDeclaration"_string);

    // 2. Empty the declarations.
    m_descriptors.clear();

    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the
    //    declarations, in specified order.
    auto descriptors = parse_css_descriptor_declaration_block(Parser::ParsingParams {}, m_at_rule_id, value);
    if (!descriptors.is_empty())
        m_descriptors = move(descriptors);

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    return {};
}

void CSSDescriptors::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& descriptor : m_descriptors) {
        descriptor.value->visit_edges(visitor);
    }
}

RefPtr<CSSStyleValue const> CSSDescriptors::descriptor(DescriptorID descriptor_id) const
{
    auto match = m_descriptors.first_matching([descriptor_id](Descriptor const& descriptor) {
        return descriptor.descriptor_id == descriptor_id;
    });
    if (match.has_value())
        return match->value;
    return nullptr;
}

RefPtr<CSSStyleValue const> CSSDescriptors::descriptor_or_initial_value(DescriptorID descriptor_id) const
{
    if (auto value = descriptor(descriptor_id))
        return value.release_nonnull();

    return descriptor_initial_value(m_at_rule_id, descriptor_id);
}

bool is_shorthand(AtRuleID at_rule, DescriptorID descriptor)
{
    if (at_rule == AtRuleID::Page && descriptor == DescriptorID::Margin)
        return true;

    return false;
}

void for_each_expanded_longhand(AtRuleID at_rule, DescriptorID descriptor, RefPtr<CSSStyleValue const> value, Function<void(DescriptorID, RefPtr<CSSStyleValue const>)> callback)
{
    if (at_rule == AtRuleID::Page && descriptor == DescriptorID::Margin) {
        if (!value) {
            callback(DescriptorID::MarginTop, nullptr);
            callback(DescriptorID::MarginRight, nullptr);
            callback(DescriptorID::MarginBottom, nullptr);
            callback(DescriptorID::MarginLeft, nullptr);
            return;
        }

        if (value->is_value_list()) {
            auto& values = value->as_value_list().values();
            if (values.size() == 4) {
                callback(DescriptorID::MarginTop, values[0]);
                callback(DescriptorID::MarginRight, values[1]);
                callback(DescriptorID::MarginBottom, values[2]);
                callback(DescriptorID::MarginLeft, values[3]);
            } else if (values.size() == 3) {
                callback(DescriptorID::MarginTop, values[0]);
                callback(DescriptorID::MarginRight, values[1]);
                callback(DescriptorID::MarginBottom, values[2]);
                callback(DescriptorID::MarginLeft, values[1]);
            } else if (values.size() == 2) {
                callback(DescriptorID::MarginTop, values[0]);
                callback(DescriptorID::MarginRight, values[1]);
                callback(DescriptorID::MarginBottom, values[0]);
                callback(DescriptorID::MarginLeft, values[1]);
            } else if (values.size() == 1) {
                callback(DescriptorID::MarginTop, values[0]);
                callback(DescriptorID::MarginRight, values[0]);
                callback(DescriptorID::MarginBottom, values[0]);
                callback(DescriptorID::MarginLeft, values[0]);
            }

        } else {
            callback(DescriptorID::MarginTop, *value);
            callback(DescriptorID::MarginRight, *value);
            callback(DescriptorID::MarginBottom, *value);
            callback(DescriptorID::MarginLeft, *value);
        }
    }
}

}
