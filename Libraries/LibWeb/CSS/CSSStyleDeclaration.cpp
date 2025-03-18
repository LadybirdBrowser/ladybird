/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSStyleDeclarationPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleDeclaration);
GC_DEFINE_ALLOCATOR(PropertyOwningCSSStyleDeclaration);
GC_DEFINE_ALLOCATOR(ElementInlineCSSStyleDeclaration);

CSSStyleDeclaration::CSSStyleDeclaration(JS::Realm& realm, Computed computed, Readonly readonly)
    : PlatformObject(realm)
    , m_computed(computed == Computed::Yes)
    , m_readonly(readonly == Readonly::Yes)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
    };
}

void CSSStyleDeclaration::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleDeclaration);
}

GC::Ref<PropertyOwningCSSStyleDeclaration> PropertyOwningCSSStyleDeclaration::create(JS::Realm& realm, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    return realm.create<PropertyOwningCSSStyleDeclaration>(realm, move(properties), move(custom_properties));
}

PropertyOwningCSSStyleDeclaration::PropertyOwningCSSStyleDeclaration(JS::Realm& realm, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
    : CSSStyleDeclaration(realm, Computed::No, Readonly::No)
    , m_properties(move(properties))
    , m_custom_properties(move(custom_properties))
{
}

void PropertyOwningCSSStyleDeclaration::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
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

GC::Ref<ElementInlineCSSStyleDeclaration> ElementInlineCSSStyleDeclaration::create(DOM::Element& element, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    auto& realm = element.realm();
    return realm.create<ElementInlineCSSStyleDeclaration>(element, move(properties), move(custom_properties));
}

ElementInlineCSSStyleDeclaration::ElementInlineCSSStyleDeclaration(DOM::Element& element, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
    : PropertyOwningCSSStyleDeclaration(element.realm(), move(properties), move(custom_properties))
{
    set_owner_node(DOM::ElementReference { element });
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
WebIDL::ExceptionOr<void> PropertyOwningCSSStyleDeclaration::set_property(StringView property_name, StringView value, StringView priority)
{
    auto maybe_property_id = property_id_from_string(property_name);
    if (!maybe_property_id.has_value())
        return {};
    auto property_id = maybe_property_id.value();

    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    // NOTE: This is handled by the virtual override in ResolvedCSSStyleDeclaration.

    // FIXME: 2. If property is not a custom property, follow these substeps:
    // FIXME:    1. Let property be property converted to ASCII lowercase.
    // FIXME:    2. If property is not a case-sensitive match for a supported CSS property, then return.
    // NOTE: This must be handled before we've turned the property string into a PropertyID.

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        MUST(remove_property(property_name));
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !Infra::is_ascii_case_insensitive_match(priority, "important"sv))
        return {};

    // 5. Let component value list be the result of parsing value for property property.
    auto component_value_list = owner_node().has_value()
        ? parse_css_value(CSS::Parser::ParsingParams { owner_node()->element().document() }, value, property_id)
        : parse_css_value(CSS::Parser::ParsingParams {}, value, property_id);

    // 6. If component value list is null, then return.
    if (!component_value_list)
        return {};

    // 7. Let updated be false.
    bool updated = false;

    // 8. If property is a shorthand property,
    if (property_is_shorthand(property_id)) {
        // then for each longhand property longhand that property maps to, in canonical order, follow these substeps:
        StyleComputer::for_each_property_expanding_shorthands(property_id, *component_value_list, StyleComputer::AllowUnresolved::Yes, [this, &updated, priority](PropertyID longhand_property_id, CSSStyleValue const& longhand_value) {
            // 1. Let longhand result be the result of set the CSS declaration longhand with the appropriate value(s) from component value list,
            //    with the important flag set if priority is not the empty string, and unset otherwise, and with the list of declarations being the declarations.
            // 2. If longhand result is true, let updated be true.
            updated |= set_a_css_declaration(longhand_property_id, longhand_value, !priority.is_empty() ? Important::Yes : Important::No);
        });
    }
    // 9. Otherwise,
    else {
        if (property_id == PropertyID::Custom) {
            auto custom_name = FlyString::from_utf8_without_validation(property_name.bytes());
            StyleProperty style_property {
                .important = !priority.is_empty() ? Important::Yes : Important::No,
                .property_id = property_id,
                .value = component_value_list.release_nonnull(),
                .custom_name = custom_name,
            };
            m_custom_properties.set(custom_name, style_property);
            updated = true;
        } else {
            // let updated be the result of set the CSS declaration property with value component value list,
            // with the important flag set if priority is not the empty string, and unset otherwise,
            // and with the list of declarations being the declarations.
            updated = set_a_css_declaration(property_id, *component_value_list, !priority.is_empty() ? Important::Yes : Important::No);
        }
    }

    // 10. If updated is true, update style attribute for the CSS declaration block.
    if (updated)
        update_style_attribute();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> PropertyOwningCSSStyleDeclaration::remove_property(StringView property_name)
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return String {};

    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    // NOTE: This is handled by the virtual override in ResolvedCSSStyleDeclaration.

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // NOTE: We've already converted it to a PropertyID enum value.

    // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
    auto value = get_property_value(property_name);

    // 4. Let removed be false.
    bool removed = false;

    // FIXME: 5. If property is a shorthand property, for each longhand property longhand that property maps to:
    //           1. If longhand is not a property name of a CSS declaration in the declarations, continue.
    //           2. Remove that CSS declaration and let removed be true.

    // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the declarations, remove that CSS declaration and let removed be true.
    if (property_id == PropertyID::Custom) {
        auto custom_name = FlyString::from_utf8_without_validation(property_name.bytes());
        removed = m_custom_properties.remove(custom_name);
    } else {
        removed = m_properties.remove_first_matching([&](auto& entry) { return entry.property_id == property_id; });
    }

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
    VERIFY(!is_computed());

    // 2. Let owner node be declaration block’s owner node.
    // 3. If owner node is null, then return.
    if (!owner_node().has_value())
        return;

    // 4. Set declaration block’s updating flag.
    set_is_updating(true);

    // 5. Set an attribute value for owner node using "style" and the result of serializing declaration block.
    MUST(owner_node()->element().set_attribute(HTML::AttributeNames::style, serialized()));

    // 6. Unset declaration block’s updating flag.
    set_is_updating(false);
}

// https://drafts.csswg.org/cssom/#set-a-css-declaration
bool PropertyOwningCSSStyleDeclaration::set_a_css_declaration(PropertyID property_id, NonnullRefPtr<CSSStyleValue const> value, Important important)
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

static Optional<StyleProperty> style_property_for_sided_shorthand(PropertyID property_id, Optional<StyleProperty> const& top, Optional<StyleProperty> const& right, Optional<StyleProperty> const& bottom, Optional<StyleProperty> const& left)
{
    if (!top.has_value() || !right.has_value() || !bottom.has_value() || !left.has_value())
        return {};

    if (top->important != right->important || top->important != bottom->important || top->important != left->important)
        return {};

    ValueComparingNonnullRefPtr<CSSStyleValue> const top_value { top->value };
    ValueComparingNonnullRefPtr<CSSStyleValue> const right_value { right->value };
    ValueComparingNonnullRefPtr<CSSStyleValue> const bottom_value { bottom->value };
    ValueComparingNonnullRefPtr<CSSStyleValue> const left_value { left->value };

    bool const top_and_bottom_same = top_value == bottom_value;
    bool const left_and_right_same = left_value == right_value;

    RefPtr<CSSStyleValue const> value;

    if (top_and_bottom_same && left_and_right_same && top_value == left_value) {
        value = top_value;
    } else if (top_and_bottom_same && left_and_right_same) {
        value = StyleValueList::create(StyleValueVector { top_value, right_value }, StyleValueList::Separator::Space);
    } else if (left_and_right_same) {
        value = StyleValueList::create(StyleValueVector { top_value, right_value, bottom_value }, StyleValueList::Separator::Space);
    } else {
        value = StyleValueList::create(StyleValueVector { top_value, right_value, bottom_value, left_value }, StyleValueList::Separator::Space);
    }

    return StyleProperty {
        .important = top->important,
        .property_id = property_id,
        .value = value.release_nonnull(),
    };
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
Optional<StyleProperty> CSSStyleDeclaration::get_property_internal(PropertyID property_id) const
{
    // 2. If property is a shorthand property, then follow these substeps:
    if (property_is_shorthand(property_id)) {

        // AD-HOC: Handle shorthands that require manual construction.
        switch (property_id) {
        case PropertyID::Border: {
            auto width = get_property_internal(PropertyID::BorderWidth);
            auto style = get_property_internal(PropertyID::BorderStyle);
            auto color = get_property_internal(PropertyID::BorderColor);
            // `border` only has a reasonable value if all four sides are the same.
            if (!width.has_value() || width->value->is_value_list() || !style.has_value() || style->value->is_value_list() || !color.has_value() || color->value->is_value_list())
                return {};
            if (width->important != style->important || width->important != color->important)
                return {};
            return StyleProperty {
                .important = width->important,
                .property_id = property_id,
                .value = ShorthandStyleValue::create(property_id,
                    { PropertyID::BorderWidth, PropertyID::BorderStyle, PropertyID::BorderColor },
                    { width->value, style->value, color->value })
            };
        }
        case PropertyID::BorderColor: {
            auto top = get_property_internal(PropertyID::BorderTopColor);
            auto right = get_property_internal(PropertyID::BorderRightColor);
            auto bottom = get_property_internal(PropertyID::BorderBottomColor);
            auto left = get_property_internal(PropertyID::BorderLeftColor);
            return style_property_for_sided_shorthand(property_id, top, right, bottom, left);
        }
        case PropertyID::BorderStyle: {
            auto top = get_property_internal(PropertyID::BorderTopStyle);
            auto right = get_property_internal(PropertyID::BorderRightStyle);
            auto bottom = get_property_internal(PropertyID::BorderBottomStyle);
            auto left = get_property_internal(PropertyID::BorderLeftStyle);
            return style_property_for_sided_shorthand(property_id, top, right, bottom, left);
        }
        case PropertyID::BorderWidth: {
            auto top = get_property_internal(PropertyID::BorderTopWidth);
            auto right = get_property_internal(PropertyID::BorderRightWidth);
            auto bottom = get_property_internal(PropertyID::BorderBottomWidth);
            auto left = get_property_internal(PropertyID::BorderLeftWidth);
            return style_property_for_sided_shorthand(property_id, top, right, bottom, left);
        }
        case PropertyID::FontVariant: {
            auto ligatures = get_property_internal(PropertyID::FontVariantLigatures);
            auto caps = get_property_internal(PropertyID::FontVariantCaps);
            auto alternates = get_property_internal(PropertyID::FontVariantAlternates);
            auto numeric = get_property_internal(PropertyID::FontVariantNumeric);
            auto east_asian = get_property_internal(PropertyID::FontVariantEastAsian);
            auto position = get_property_internal(PropertyID::FontVariantPosition);
            auto emoji = get_property_internal(PropertyID::FontVariantEmoji);

            if (!ligatures.has_value() || !caps.has_value() || !alternates.has_value() || !numeric.has_value() || !east_asian.has_value() || !position.has_value() || !emoji.has_value())
                return {};

            if (ligatures->important != caps->important || ligatures->important != alternates->important || ligatures->important != numeric->important || ligatures->important != east_asian->important || ligatures->important != position->important || ligatures->important != emoji->important)
                return {};

            // If ligatures is `none` and any other value isn't `normal`, that's invalid.
            if (ligatures->value->to_keyword() == Keyword::None
                && (caps->value->to_keyword() != Keyword::Normal
                    || alternates->value->to_keyword() != Keyword::Normal
                    || numeric->value->to_keyword() != Keyword::Normal
                    || east_asian->value->to_keyword() != Keyword::Normal
                    || position->value->to_keyword() != Keyword::Normal
                    || emoji->value->to_keyword() != Keyword::Normal)) {
                return {};
            }

            return StyleProperty {
                .important = ligatures->important,
                .property_id = property_id,
                .value = ShorthandStyleValue::create(property_id,
                    { PropertyID::FontVariantLigatures, PropertyID::FontVariantCaps, PropertyID::FontVariantAlternates, PropertyID::FontVariantNumeric, PropertyID::FontVariantEastAsian, PropertyID::FontVariantPosition, PropertyID::FontVariantEmoji },
                    { ligatures->value, caps->value, alternates->value, numeric->value, east_asian->value, position->value, emoji->value })
            };
        }
        case PropertyID::Margin: {
            auto top = get_property_internal(PropertyID::MarginTop);
            auto right = get_property_internal(PropertyID::MarginRight);
            auto bottom = get_property_internal(PropertyID::MarginBottom);
            auto left = get_property_internal(PropertyID::MarginLeft);
            return style_property_for_sided_shorthand(property_id, top, right, bottom, left);
        }
        case PropertyID::Padding: {
            auto top = get_property_internal(PropertyID::PaddingTop);
            auto right = get_property_internal(PropertyID::PaddingRight);
            auto bottom = get_property_internal(PropertyID::PaddingBottom);
            auto left = get_property_internal(PropertyID::PaddingLeft);
            return style_property_for_sided_shorthand(property_id, top, right, bottom, left);
        }
        default:
            break;
        }

        // 1. Let list be a new empty array.
        Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> list;
        Optional<Important> last_important_flag;

        // 2. For each longhand property longhand that property maps to, in canonical order, follow these substeps:
        Vector<PropertyID> longhand_ids = longhands_for_shorthand(property_id);
        for (auto longhand_property_id : longhand_ids) {
            // 1. If longhand is a case-sensitive match for a property name of a CSS declaration in the declarations,
            //    let declaration be that CSS declaration, or null otherwise.
            auto declaration = get_property_internal(longhand_property_id);

            // 2. If declaration is null, then return the empty string.
            if (!declaration.has_value())
                return {};

            // 3. Append the declaration to list.
            list.append(declaration->value);

            if (last_important_flag.has_value() && declaration->important != *last_important_flag)
                return {};
            last_important_flag = declaration->important;
        }

        // 3. If important flags of all declarations in list are same, then return the serialization of list.
        // NOTE: Currently we implement property-specific shorthand serialization in ShorthandStyleValue::to_string().
        return StyleProperty {
            .important = last_important_flag.value(),
            .property_id = property_id,
            .value = ShorthandStyleValue::create(property_id, longhand_ids, list),
        };

        // 4. Return the empty string.
        // NOTE: This is handled by the loop.
    }

    return property(property_id);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
String CSSStyleDeclaration::get_property_value(StringView property_name) const
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return {};

    if (property_id.value() == PropertyID::Custom) {
        auto maybe_custom_property = custom_property(FlyString::from_utf8_without_validation(property_name.bytes()));
        if (maybe_custom_property.has_value()) {
            return maybe_custom_property.value().value->to_string(
                is_computed() ? CSSStyleValue::SerializationMode::ResolvedValue
                              : CSSStyleValue::SerializationMode::Normal);
        }
        return {};
    }

    auto maybe_property = get_property_internal(property_id.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->value->to_string(
        is_computed() ? CSSStyleValue::SerializationMode::ResolvedValue
                      : CSSStyleValue::SerializationMode::Normal);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
StringView CSSStyleDeclaration::get_property_priority(StringView property_name) const
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return {};
    if (property_id.value() == PropertyID::Custom) {
        auto maybe_custom_property = custom_property(FlyString::from_utf8_without_validation(property_name.bytes()));
        if (!maybe_custom_property.has_value())
            return {};
        return maybe_custom_property.value().important == Important::Yes ? "important"sv : ""sv;
    }
    auto maybe_property = property(property_id.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->important == Important::Yes ? "important"sv : ""sv;
}

WebIDL::ExceptionOr<void> CSSStyleDeclaration::set_property(PropertyID property_id, StringView css_text, StringView priority)
{
    return set_property(string_from_property_id(property_id), css_text, priority);
}

WebIDL::ExceptionOr<String> CSSStyleDeclaration::remove_property(PropertyID property_name)
{
    return remove_property(string_from_property_id(property_name));
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
String CSSStyleDeclaration::css_text() const
{
    // 1. If the computed flag is set, then return the empty string.
    // NOTE: See ResolvedCSSStyleDeclaration::serialized()

    // 2. Return the result of serializing the declarations.
    return serialized();
}

// https://drafts.csswg.org/cssom/#dom-cssstyleproperties-cssfloat
String CSSStyleDeclaration::css_float() const
{
    // The cssFloat attribute, on getting, must return the result of invoking getPropertyValue() with float as argument.
    return get_property_value("float"sv);
}

WebIDL::ExceptionOr<void> CSSStyleDeclaration::set_css_float(StringView value)
{
    // On setting, the attribute must invoke setProperty() with float as first argument, as second argument the given value,
    // and no third argument. Any exceptions thrown must be re-thrown.
    return set_property("float"sv, value, ""sv);
}

Optional<JS::Value> CSSStyleDeclaration::item_value(size_t index) const
{
    auto value = item(index);
    if (value.is_empty())
        return {};

    return JS::PrimitiveString::create(vm(), value);
}

void CSSStyleDeclaration::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_rule);
    if (m_owner_node.has_value())
        m_owner_node->visit(visitor);
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
        auto value = declaration.value.value->to_string(Web::CSS::CSSStyleValue::SerializationMode::Normal);

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
        auto value = declaration.value->to_string(Web::CSS::CSSStyleValue::SerializationMode::Normal);

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

void ElementInlineCSSStyleDeclaration::set_declarations_from_text(StringView css_text)
{
    // FIXME: What do we do if the element is null?
    auto element = owner_node();
    if (!element.has_value()) {
        dbgln("FIXME: Returning from ElementInlineCSSStyleDeclaration::declarations_from_text as element is null.");
        return;
    }

    empty_the_declarations();
    auto style = parse_css_style_attribute(CSS::Parser::ParsingParams(element->element().document()), css_text, element->element());
    set_the_declarations(style->properties(), style->custom_properties());
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> ElementInlineCSSStyleDeclaration::set_css_text(StringView css_text)
{
    // FIXME: What do we do if the element is null?
    if (!owner_node().has_value()) {
        dbgln("FIXME: Returning from ElementInlineCSSStyleDeclaration::set_css_text as element is null.");
        return {};
    }

    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    // NOTE: See ResolvedCSSStyleDeclaration.

    // 2. Empty the declarations.
    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the declarations, in specified order.
    set_declarations_from_text(css_text);

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    return {};
}

}
