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

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleDeclaration);

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

// https://drafts.csswg.org/cssom/#update-style-attribute-for
void CSSStyleDeclaration::update_style_attribute()
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
    if (is_computed())
        return {};

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

}
