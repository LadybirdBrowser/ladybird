/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSStylePropertiesPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleProperties);

GC::Ref<CSSStyleProperties> CSSStyleProperties::create(JS::Realm& realm, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    // https://drafts.csswg.org/cssom/#dom-cssstylerule-style
    // The style attribute must return a CSSStyleProperties object for the style rule, with the following properties:
    //     computed flag: Unset.
    //     readonly flag: Unset.
    //     declarations: The declared declarations in the rule, in specified order.
    //     parent CSS rule: The context object.
    //     owner node: Null.
    return realm.create<CSSStyleProperties>(realm, Computed::No, Readonly::No, convert_declarations_to_specified_order(properties), move(custom_properties), OptionalNone {});
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_resolved_style(JS::Realm& realm, Optional<DOM::ElementReference> element_reference)
{
    // https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle
    // 6.  Return a live CSSStyleProperties object with the following properties:
    //     computed flag: Set.
    //     readonly flag: Set.
    //     declarations: decls.
    //     parent CSS rule: Null.
    //     owner node: obj.
    // AD-HOC: Rather than instantiate with a list of decls, they're generated on demand.
    return realm.create<CSSStyleProperties>(realm, Computed::Yes, Readonly::Yes, Vector<StyleProperty> {}, HashMap<FlyString, StyleProperty> {}, move(element_reference));
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_element_inline_style(DOM::ElementReference element_reference, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    // https://drafts.csswg.org/cssom/#dom-elementcssinlinestyle-style
    // The style attribute must return a CSS declaration block object whose readonly flag is unset, whose parent CSS
    // rule is null, and whose owner node is the context object.
    auto& realm = element_reference.element().realm();
    return realm.create<CSSStyleProperties>(realm, Computed::No, Readonly::No, convert_declarations_to_specified_order(properties), move(custom_properties), move(element_reference));
}

CSSStyleProperties::CSSStyleProperties(JS::Realm& realm, Computed computed, Readonly readonly, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties, Optional<DOM::ElementReference> owner_node)
    : CSSStyleDeclaration(realm, computed, readonly)
    , m_properties(move(properties))
    , m_custom_properties(move(custom_properties))
{
    set_owner_node(move(owner_node));
}

// https://drafts.csswg.org/cssom/#concept-declarations-specified-order
Vector<StyleProperty> CSSStyleProperties::convert_declarations_to_specified_order(Vector<StyleProperty>& declarations)
{
    // The specified order for declarations is the same as specified, but with shorthand properties expanded into their
    // longhand properties, in canonical order. If a property is specified more than once (after shorthand expansion), only
    // the one with greatest cascading order must be represented, at the same relative position as it was specified.
    Vector<StyleProperty> specified_order_declarations;

    for (auto declaration : declarations) {
        StyleComputer::for_each_property_expanding_shorthands(declaration.property_id, declaration.value, [&](CSS::PropertyID longhand_id, CSS::CSSStyleValue const& longhand_property_value) {
            auto existing_entry_index = specified_order_declarations.find_first_index_if([&](StyleProperty const& existing_declaration) { return existing_declaration.property_id == longhand_id; });

            if (existing_entry_index.has_value()) {
                // If there is an existing entry for this property and it is a higher cascading order than the current entry, skip the current entry.
                if (specified_order_declarations[existing_entry_index.value()].important == Important::Yes && declaration.important == Important::No)
                    return;

                // Otherwise the existing entry has a lower cascading order and is removed.
                specified_order_declarations.remove(existing_entry_index.value());
            }

            specified_order_declarations.append(StyleProperty {
                .important = declaration.important,
                .property_id = longhand_id,
                .value = longhand_property_value });
        });
    }

    return specified_order_declarations;
}

void CSSStyleProperties::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleProperties);
    Base::initialize(realm);
}

void CSSStyleProperties::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& property : m_properties) {
        property.value->visit_edges(visitor);
    }
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
size_t CSSStyleProperties::length() const
{
    // The length attribute must return the number of CSS declarations in the declarations.
    // FIXME: Include the number of custom properties.

    if (is_computed()) {
        if (!owner_node().has_value())
            return 0;
        return to_underlying(last_longhand_property_id) - to_underlying(first_longhand_property_id) + 1;
    }

    return m_properties.size();
}

String CSSStyleProperties::item(size_t index) const
{
    // The item(index) method must return the property name of the CSS declaration at position index.
    // FIXME: Include custom properties.

    if (index >= length())
        return {};

    if (is_computed()) {
        auto property_id = static_cast<PropertyID>(index + to_underlying(first_longhand_property_id));
        return string_from_property_id(property_id).to_string();
    }

    return CSS::string_from_property_id(m_properties[index].property_id).to_string();
}

Optional<StyleProperty> CSSStyleProperties::property(PropertyID property_id) const
{
    if (is_computed()) {
        if (!owner_node().has_value())
            return {};

        auto& element = owner_node()->element();
        auto pseudo_element = owner_node()->pseudo_element();

        // https://www.w3.org/TR/cssom-1/#dom-window-getcomputedstyle
        // NB: This is a partial enforcement of step 5 ("If elt is connected, ...")
        if (!element.is_connected())
            return {};

        auto get_layout_node = [&]() {
            if (pseudo_element.has_value())
                return element.get_pseudo_element_node(pseudo_element.value());
            return element.layout_node();
        };

        Layout::NodeWithStyle* layout_node = get_layout_node();

        // FIXME: Be smarter about updating layout if there's no layout node.
        //        We may legitimately have no layout node if we're not visible, but this protects against situations
        //        where we're requesting the computed style before layout has happened.
        if (!layout_node || property_affects_layout(property_id)) {
            element.document().update_layout(DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
            layout_node = get_layout_node();
        } else {
            // FIXME: If we had a way to update style for a single element, this would be a good place to use it.
            element.document().update_style();
        }

        if (!layout_node) {
            auto style = element.document().style_computer().compute_style(element, pseudo_element);

            // FIXME: This is a stopgap until we implement shorthand -> longhand conversion.
            auto const* value = style->maybe_null_property(property_id);
            if (!value) {
                dbgln("FIXME: CSSStyleProperties::property(property_id={:#x}) No value for property ID in newly computed style case.", to_underlying(property_id));
                return {};
            }
            return StyleProperty {
                .property_id = property_id,
                .value = *value,
            };
        }

        auto value = style_value_for_computed_property(*layout_node, property_id);
        if (!value)
            return {};
        return StyleProperty {
            .property_id = property_id,
            .value = *value,
        };
    }

    for (auto& property : m_properties) {
        if (property.property_id == property_id)
            return property;
    }
    return {};
}

Optional<StyleProperty const&> CSSStyleProperties::custom_property(FlyString const& custom_property_name) const
{
    if (is_computed()) {
        if (!owner_node().has_value())
            return {};

        auto& element = owner_node()->element();
        auto pseudo_element = owner_node()->pseudo_element();

        element.document().update_style();

        auto const* element_to_check = &element;
        while (element_to_check) {
            if (auto property = element_to_check->custom_properties(pseudo_element).get(custom_property_name); property.has_value())
                return *property;

            element_to_check = element_to_check->parent_element();
        }

        return {};
    }

    return m_custom_properties.get(custom_property_name);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> CSSStyleProperties::set_property(StringView property_name, StringView value, StringView priority)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    if (is_computed())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties in result of getComputedStyle()"_string);

    // FIXME: 2. If property is not a custom property, follow these substeps:
    // FIXME:    1. Let property be property converted to ASCII lowercase.
    // FIXME:    2. If property is not a case-sensitive match for a supported CSS property, then return.
    // NB: This must be handled before we've turned the property string into a PropertyID.

    auto maybe_property_id = property_id_from_string(property_name);
    if (!maybe_property_id.has_value())
        return {};
    auto property_id = maybe_property_id.value();

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        MUST(remove_property(property_name));
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !priority.equals_ignoring_ascii_case("important"sv))
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
        StyleComputer::for_each_property_expanding_shorthands(property_id, *component_value_list, [this, &updated, priority](PropertyID longhand_property_id, CSSStyleValue const& longhand_value) {
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
    if (updated) {
        update_style_attribute();

        // Non-standard: Invalidate style for the owners of our containing sheet, if any.
        invalidate_owners(DOM::StyleInvalidationReason::CSSStylePropertiesSetProperty);
    }

    return {};
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_property(PropertyID property_id, StringView css_text, StringView priority)
{
    return set_property(string_from_property_id(property_id), css_text, priority);
}

static NonnullRefPtr<CSSStyleValue const> style_value_for_length_percentage(LengthPercentage const& length_percentage)
{
    if (length_percentage.is_auto())
        return CSSKeywordValue::create(Keyword::Auto);
    if (length_percentage.is_percentage())
        return PercentageStyleValue::create(length_percentage.percentage());
    if (length_percentage.is_length())
        return LengthStyleValue::create(length_percentage.length());
    return length_percentage.calculated();
}

static NonnullRefPtr<CSSStyleValue const> style_value_for_size(Size const& size)
{
    if (size.is_none())
        return CSSKeywordValue::create(Keyword::None);
    if (size.is_percentage())
        return PercentageStyleValue::create(size.percentage());
    if (size.is_length())
        return LengthStyleValue::create(size.length());
    if (size.is_auto())
        return CSSKeywordValue::create(Keyword::Auto);
    if (size.is_calculated())
        return size.calculated();
    if (size.is_min_content())
        return CSSKeywordValue::create(Keyword::MinContent);
    if (size.is_max_content())
        return CSSKeywordValue::create(Keyword::MaxContent);
    if (size.is_fit_content())
        return FitContentStyleValue::create(size.fit_content_available_space());
    TODO();
}

enum class LogicalSide {
    BlockStart,
    BlockEnd,
    InlineStart,
    InlineEnd,
};
static RefPtr<CSSStyleValue const> style_value_for_length_box_logical_side(Layout::NodeWithStyle const&, LengthBox const& box, LogicalSide logical_side)
{
    // FIXME: Actually determine the logical sides based on layout_node's writing-mode and direction.
    switch (logical_side) {
    case LogicalSide::BlockStart:
        return style_value_for_length_percentage(box.top());
    case LogicalSide::BlockEnd:
        return style_value_for_length_percentage(box.bottom());
    case LogicalSide::InlineStart:
        return style_value_for_length_percentage(box.left());
    case LogicalSide::InlineEnd:
        return style_value_for_length_percentage(box.right());
    }
    VERIFY_NOT_REACHED();
}

static CSSPixels pixels_for_pixel_box_logical_side(Layout::NodeWithStyle const&, Painting::PixelBox const& box, LogicalSide logical_side)
{
    // FIXME: Actually determine the logical sides based on layout_node's writing-mode and direction.
    switch (logical_side) {
    case LogicalSide::BlockStart:
        return box.top;
    case LogicalSide::BlockEnd:
        return box.bottom;
    case LogicalSide::InlineStart:
        return box.left;
    case LogicalSide::InlineEnd:
        return box.right;
    }
    VERIFY_NOT_REACHED();
}

static RefPtr<CSSStyleValue const> style_value_for_shadow(Vector<ShadowData> const& shadow_data)
{
    if (shadow_data.is_empty())
        return CSSKeywordValue::create(Keyword::None);

    auto make_shadow_style_value = [](ShadowData const& shadow) {
        return ShadowStyleValue::create(
            CSSColorValue::create_from_color(shadow.color, ColorSyntax::Modern),
            style_value_for_length_percentage(shadow.offset_x),
            style_value_for_length_percentage(shadow.offset_y),
            style_value_for_length_percentage(shadow.blur_radius),
            style_value_for_length_percentage(shadow.spread_distance),
            shadow.placement);
    };

    if (shadow_data.size() == 1)
        return make_shadow_style_value(shadow_data.first());

    StyleValueVector style_values;
    style_values.ensure_capacity(shadow_data.size());
    for (auto& shadow : shadow_data)
        style_values.unchecked_append(make_shadow_style_value(shadow));

    return StyleValueList::create(move(style_values), StyleValueList::Separator::Comma);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
String CSSStyleProperties::get_property_value(StringView property_name) const
{
    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return {};

    if (property_id.value() == PropertyID::Custom) {
        auto maybe_custom_property = custom_property(FlyString::from_utf8_without_validation(property_name.bytes()));
        if (maybe_custom_property.has_value()) {
            return maybe_custom_property.value().value->to_string(
                is_computed() ? SerializationMode::ResolvedValue
                              : SerializationMode::Normal);
        }
        return {};
    }

    auto maybe_property = get_property_internal(property_id.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->value->to_string(
        is_computed() ? SerializationMode::ResolvedValue
                      : SerializationMode::Normal);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
StringView CSSStyleProperties::get_property_priority(StringView property_name) const
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

static Optional<StyleProperty> style_property_for_sided_shorthand(PropertyID property_id, Optional<StyleProperty> const& top, Optional<StyleProperty> const& right, Optional<StyleProperty> const& bottom, Optional<StyleProperty> const& left)
{
    if (!top.has_value() || !right.has_value() || !bottom.has_value() || !left.has_value())
        return {};

    if (top->important != right->important || top->important != bottom->important || top->important != left->important)
        return {};

    ValueComparingNonnullRefPtr<CSSStyleValue const> const top_value { top->value };
    ValueComparingNonnullRefPtr<CSSStyleValue const> const right_value { right->value };
    ValueComparingNonnullRefPtr<CSSStyleValue const> const bottom_value { bottom->value };
    ValueComparingNonnullRefPtr<CSSStyleValue const> const left_value { left->value };

    bool const top_and_bottom_same = top_value == bottom_value;
    bool const left_and_right_same = left_value == right_value;

    if ((top_value->is_css_wide_keyword() || right_value->is_css_wide_keyword() || bottom_value->is_css_wide_keyword() || left_value->is_css_wide_keyword()) && (!top_and_bottom_same || !left_and_right_same || top_value != left_value))
        return {};

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
Optional<StyleProperty> CSSStyleProperties::get_property_internal(PropertyID property_id) const
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
        case PropertyID::WhiteSpace: {
            auto white_space_collapse_property = get_property_internal(PropertyID::WhiteSpaceCollapse);
            auto text_wrap_mode_property = get_property_internal(PropertyID::TextWrapMode);
            auto white_space_trim_property = get_property_internal(PropertyID::WhiteSpaceTrim);

            if (!white_space_collapse_property.has_value() || !text_wrap_mode_property.has_value() || !white_space_trim_property.has_value())
                break;

            RefPtr<CSSStyleValue const> value;

            if (white_space_trim_property->value->is_keyword() && white_space_trim_property->value->as_keyword().keyword() == Keyword::None) {
                auto white_space_collapse_keyword = white_space_collapse_property->value->as_keyword().keyword();
                auto text_wrap_mode_keyword = text_wrap_mode_property->value->as_keyword().keyword();

                if (white_space_collapse_keyword == Keyword::Collapse && text_wrap_mode_keyword == Keyword::Wrap)
                    value = CSSKeywordValue::create(Keyword::Normal);

                if (white_space_collapse_keyword == Keyword::Preserve && text_wrap_mode_keyword == Keyword::Nowrap)
                    value = CSSKeywordValue::create(Keyword::Pre);

                if (white_space_collapse_keyword == Keyword::Preserve && text_wrap_mode_keyword == Keyword::Wrap)
                    value = CSSKeywordValue::create(Keyword::PreWrap);

                if (white_space_collapse_keyword == Keyword::PreserveBreaks && text_wrap_mode_keyword == Keyword::Wrap)
                    value = CSSKeywordValue::create(Keyword::PreLine);
            }

            if (!value)
                break;

            return StyleProperty {
                .important = white_space_collapse_property->important,
                .property_id = property_id,
                .value = value.release_nonnull(),
            };
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

static RefPtr<CSSStyleValue const> resolve_color_style_value(CSSStyleValue const& style_value, Color computed_color)
{
    if (style_value.is_color_function())
        return style_value;
    if (style_value.is_color()) {
        auto& color_style_value = static_cast<CSSColorValue const&>(style_value);
        if (first_is_one_of(color_style_value.color_type(), CSSColorValue::ColorType::Lab, CSSColorValue::ColorType::OKLab, CSSColorValue::ColorType::LCH, CSSColorValue::ColorType::OKLCH))
            return style_value;
    }

    return CSSColorValue::create_from_color(computed_color, ColorSyntax::Modern);
}

RefPtr<CSSStyleValue const> CSSStyleProperties::style_value_for_computed_property(Layout::NodeWithStyle const& layout_node, PropertyID property_id) const
{
    if (!owner_node().has_value()) {
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for CSSStyleProperties without owner node was requested");
        return nullptr;
    }

    auto used_value_for_property = [&layout_node, property_id](Function<CSSPixels(Painting::PaintableBox const&)>&& used_value_getter) -> Optional<CSSPixels> {
        auto const& display = layout_node.computed_values().display();
        if (!display.is_none() && !display.is_contents() && layout_node.first_paintable()) {
            if (layout_node.first_paintable()->is_paintable_box()) {
                auto const& paintable_box = static_cast<Painting::PaintableBox const&>(*layout_node.first_paintable());
                return used_value_getter(paintable_box);
            }
            dbgln("FIXME: Support getting used value for property `{}` on {}", string_from_property_id(property_id), layout_node.debug_description());
        }
        return {};
    };

    auto& element = owner_node()->element();
    auto pseudo_element = owner_node()->pseudo_element();

    auto used_value_for_inset = [&layout_node, used_value_for_property](LengthPercentage const& start_side, LengthPercentage const& end_side, Function<CSSPixels(Painting::PaintableBox const&)>&& used_value_getter) -> Optional<CSSPixels> {
        if (!layout_node.is_positioned())
            return {};

        // FIXME: Support getting the used value when position is sticky.
        if (layout_node.is_sticky_position())
            return {};

        if (!start_side.is_percentage() && !start_side.is_calculated() && !start_side.is_auto() && !end_side.is_auto())
            return {};

        return used_value_for_property(move(used_value_getter));
    };

    auto get_computed_value = [&element, pseudo_element](PropertyID property_id) -> auto const& {
        if (pseudo_element.has_value())
            return element.pseudo_element_computed_properties(pseudo_element.value())->property(property_id);
        return element.computed_properties()->property(property_id);
    };

    // A limited number of properties have special rules for producing their "resolved value".
    // We also have to manually construct shorthands from their longhands here.
    // Everything else uses the computed value.
    // https://drafts.csswg.org/cssom/#resolved-values

    // The resolved value for a given longhand property can be determined as follows:
    switch (property_id) {
        // -> background-color
        // -> border-block-end-color
        // -> border-block-start-color
        // -> border-bottom-color
        // -> border-inline-end-color
        // -> border-inline-start-color
        // -> border-left-color
        // -> border-right-color
        // -> border-top-color
        // -> box-shadow
        // -> caret-color
        // -> color
        // -> outline-color
        // -> A resolved value special case property like color defined in another specification
        //    The resolved value is the used value.
    case PropertyID::BackgroundColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().background_color());
    case PropertyID::BorderBlockEndColor:
        // FIXME: Honor writing-mode, direction and text-orientation.
        return style_value_for_computed_property(layout_node, PropertyID::BorderBottomColor);
    case PropertyID::BorderBlockStartColor:
        // FIXME: Honor writing-mode, direction and text-orientation.
        return style_value_for_computed_property(layout_node, PropertyID::BorderTopColor);
    case PropertyID::BorderBottomColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().border_bottom().color);
    case PropertyID::BorderInlineEndColor:
        // FIXME: Honor writing-mode, direction and text-orientation.
        return style_value_for_computed_property(layout_node, PropertyID::BorderRightColor);
    case PropertyID::BorderInlineStartColor:
        // FIXME: Honor writing-mode, direction and text-orientation.
        return style_value_for_computed_property(layout_node, PropertyID::BorderLeftColor);
    case PropertyID::BorderLeftColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().border_left().color);
    case PropertyID::BorderRightColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().border_right().color);
    case PropertyID::BorderTopColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().border_top().color);
    case PropertyID::BoxShadow:
        return style_value_for_shadow(layout_node.computed_values().box_shadow());
    case PropertyID::CaretColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().caret_color());
    case PropertyID::Color:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().color());
    case PropertyID::OutlineColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().outline_color());
    case PropertyID::TextDecorationColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().text_decoration_color());
        // NB: text-shadow isn't listed, but is computed the same as box-shadow.
    case PropertyID::TextShadow:
        return style_value_for_shadow(layout_node.computed_values().text_shadow());

        // -> line-height
        //    The resolved value is normal if the computed value is normal, or the used value otherwise.
    case PropertyID::LineHeight: {
        auto const& line_height = get_computed_value(property_id);
        if (line_height.is_keyword() && line_height.to_keyword() == Keyword::Normal)
            return line_height;
        return LengthStyleValue::create(Length::make_px(layout_node.computed_values().line_height()));
    }

        // -> block-size
        // -> height
        // -> inline-size
        // -> margin-block-end
        // -> margin-block-start
        // -> margin-bottom
        // -> margin-inline-end
        // -> margin-inline-start
        // -> margin-left
        // -> margin-right
        // -> margin-top
        // -> padding-block-end
        // -> padding-block-start
        // -> padding-bottom
        // -> padding-inline-end
        // -> padding-inline-start
        // -> padding-left
        // -> padding-right
        // -> padding-top
        // -> width
        // If the property applies to the element or pseudo-element and the resolved value of the
        // display property is not none or contents, then the resolved value is the used value.
        // Otherwise the resolved value is the computed value.
    case PropertyID::BlockSize: {
        auto writing_mode = layout_node.computed_values().writing_mode();
        auto is_vertically_oriented = first_is_one_of(writing_mode, WritingMode::VerticalLr, WritingMode::VerticalRl);
        if (is_vertically_oriented)
            return style_value_for_computed_property(layout_node, PropertyID::Width);
        return style_value_for_computed_property(layout_node, PropertyID::Height);
    }
    case PropertyID::Height: {
        auto maybe_used_height = used_value_for_property([](auto const& paintable_box) { return paintable_box.content_height(); });
        if (maybe_used_height.has_value())
            return style_value_for_size(Size::make_px(maybe_used_height.release_value()));
        return style_value_for_size(layout_node.computed_values().height());
    }
    case PropertyID::InlineSize: {
        auto writing_mode = layout_node.computed_values().writing_mode();
        auto is_vertically_oriented = first_is_one_of(writing_mode, WritingMode::VerticalLr, WritingMode::VerticalRl);
        if (is_vertically_oriented)
            return style_value_for_computed_property(layout_node, PropertyID::Height);
        return style_value_for_computed_property(layout_node, PropertyID::Width);
    }
    case PropertyID::MarginBlockEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::BlockEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::BlockEnd);
    case PropertyID::MarginBlockStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::BlockStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::BlockStart);
    case PropertyID::MarginBottom:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().bottom());
    case PropertyID::MarginInlineEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::InlineEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::InlineEnd);
    case PropertyID::MarginInlineStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::InlineStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::InlineStart);
    case PropertyID::MarginLeft:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().left());
    case PropertyID::MarginRight:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().right());
    case PropertyID::MarginTop:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().top());
    case PropertyID::PaddingBlockEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::BlockEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::BlockEnd);
    case PropertyID::PaddingBlockStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::BlockStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::BlockStart);
    case PropertyID::PaddingBottom:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().bottom());
    case PropertyID::PaddingInlineEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::InlineEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::InlineEnd);
    case PropertyID::PaddingInlineStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::InlineStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::InlineStart);
    case PropertyID::PaddingLeft:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().left());
    case PropertyID::PaddingRight:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().right());
    case PropertyID::PaddingTop:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().top());
    case PropertyID::Width: {
        auto maybe_used_width = used_value_for_property([](auto const& paintable_box) { return paintable_box.content_width(); });
        if (maybe_used_width.has_value())
            return style_value_for_size(Size::make_px(maybe_used_width.release_value()));
        return style_value_for_size(layout_node.computed_values().width());
    }

        // -> bottom
        // -> left
        // -> inset-block-end
        // -> inset-block-start
        // -> inset-inline-end
        // -> inset-inline-start
        // -> right
        // -> top
        // -> A resolved value special case property like top defined in another specification
        //    If the property applies to a positioned element and the resolved value of the display property is not
        //    none or contents, and the property is not over-constrained, then the resolved value is the used value.
        //    Otherwise the resolved value is the computed value.
    case PropertyID::Bottom: {
        auto& inset = layout_node.computed_values().inset();
        if (auto maybe_used_value = used_value_for_inset(inset.bottom(), inset.top(), [](auto const& paintable_box) { return paintable_box.box_model().inset.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));

        return style_value_for_length_percentage(inset.bottom());
    }
    case PropertyID::InsetBlockEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::BlockEnd);
    case PropertyID::InsetBlockStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::BlockStart);
    case PropertyID::InsetInlineEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::InlineEnd);
    case PropertyID::InsetInlineStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::InlineStart);
    case PropertyID::Left: {
        auto& inset = layout_node.computed_values().inset();
        if (auto maybe_used_value = used_value_for_inset(inset.left(), inset.right(), [](auto const& paintable_box) { return paintable_box.box_model().inset.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(inset.left());
    }
    case PropertyID::Right: {
        auto& inset = layout_node.computed_values().inset();
        if (auto maybe_used_value = used_value_for_inset(inset.right(), inset.left(), [](auto const& paintable_box) { return paintable_box.box_model().inset.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));

        return style_value_for_length_percentage(inset.right());
    }
    case PropertyID::Top: {
        auto& inset = layout_node.computed_values().inset();
        if (auto maybe_used_value = used_value_for_inset(inset.top(), inset.bottom(), [](auto const& paintable_box) { return paintable_box.box_model().inset.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));

        return style_value_for_length_percentage(inset.top());
    }

        // -> A resolved value special case property defined in another specification
        //    As defined in the relevant specification.
    case PropertyID::Transform: {
        auto transformations = layout_node.computed_values().transformations();
        if (transformations.is_empty())
            return CSSKeywordValue::create(Keyword::None);

        // https://drafts.csswg.org/css-transforms-2/#serialization-of-the-computed-value
        // The transform property is a resolved value special case property. [CSSOM]
        // When the computed value is a <transform-list>, the resolved value is one <matrix()> function or one <matrix3d()> function computed by the following algorithm:
        // 1. Let transform be a 4x4 matrix initialized to the identity matrix.
        //    The elements m11, m22, m33 and m44 of transform must be set to 1; all other elements of transform must be set to 0.
        auto transform = FloatMatrix4x4::identity();

        // 2. Post-multiply all <transform-function>s in <transform-list> to transform.
        VERIFY(layout_node.first_paintable());
        auto const& paintable_box = as<Painting::PaintableBox const>(*layout_node.first_paintable());
        for (auto transformation : transformations) {
            transform = transform * transformation.to_matrix(paintable_box).release_value();
        }

        // https://drafts.csswg.org/css-transforms-1/#2d-matrix
        auto is_2d_matrix = [](Gfx::FloatMatrix4x4 const& matrix) -> bool {
            // A 3x2 transformation matrix,
            // or a 4x4 matrix where the items m31, m32, m13, m23, m43, m14, m24, m34 are equal to 0
            // and m33, m44 are equal to 1.
            // NB: We only care about 4x4 matrices here.
            // NB: Our elements are 0-indexed not 1-indexed, and in the opposite order.
            if (matrix.elements()[0][2] != 0     // m31
                || matrix.elements()[1][2] != 0  // m32
                || matrix.elements()[2][0] != 0  // m13
                || matrix.elements()[2][1] != 0  // m23
                || matrix.elements()[2][3] != 0  // m43
                || matrix.elements()[3][0] != 0  // m14
                || matrix.elements()[3][1] != 0  // m24
                || matrix.elements()[3][2] != 0) // m34
                return false;

            if (matrix.elements()[2][2] != 1     // m33
                || matrix.elements()[3][3] != 1) // m44
                return false;

            return true;
        };

        // 3. Chose between <matrix()> or <matrix3d()> serialization:
        // -> If transform is a 2D matrix
        //        Serialize transform to a <matrix()> function.
        if (is_2d_matrix(transform)) {
            StyleValueVector parameters {
                NumberStyleValue::create(transform.elements()[0][0]),
                NumberStyleValue::create(transform.elements()[1][0]),
                NumberStyleValue::create(transform.elements()[0][1]),
                NumberStyleValue::create(transform.elements()[1][1]),
                NumberStyleValue::create(transform.elements()[0][3]),
                NumberStyleValue::create(transform.elements()[1][3]),
            };
            return TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix, move(parameters));
        }
        // -> Otherwise
        //        Serialize transform to a <matrix3d()> function.
        else {
            StyleValueVector parameters {
                NumberStyleValue::create(transform.elements()[0][0]),
                NumberStyleValue::create(transform.elements()[1][0]),
                NumberStyleValue::create(transform.elements()[2][0]),
                NumberStyleValue::create(transform.elements()[3][0]),
                NumberStyleValue::create(transform.elements()[0][1]),
                NumberStyleValue::create(transform.elements()[1][1]),
                NumberStyleValue::create(transform.elements()[2][1]),
                NumberStyleValue::create(transform.elements()[3][1]),
                NumberStyleValue::create(transform.elements()[0][2]),
                NumberStyleValue::create(transform.elements()[1][2]),
                NumberStyleValue::create(transform.elements()[2][2]),
                NumberStyleValue::create(transform.elements()[3][2]),
                NumberStyleValue::create(transform.elements()[0][3]),
                NumberStyleValue::create(transform.elements()[1][3]),
                NumberStyleValue::create(transform.elements()[2][3]),
                NumberStyleValue::create(transform.elements()[3][3]),
            };
            return TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix3d, move(parameters));
        }
    }

        // -> Any other property
        //    The resolved value is the computed value.
        //    NOTE: This is handled inside the `default` case.
    case PropertyID::BorderBottomWidth: {
        auto border_bottom_width = layout_node.computed_values().border_bottom().width;
        return LengthStyleValue::create(Length::make_px(border_bottom_width));
    }
    case PropertyID::BorderLeftWidth: {
        auto border_left_width = layout_node.computed_values().border_left().width;
        return LengthStyleValue::create(Length::make_px(border_left_width));
    }
    case PropertyID::BorderRightWidth: {
        auto border_right_width = layout_node.computed_values().border_right().width;
        return LengthStyleValue::create(Length::make_px(border_right_width));
    }
    case PropertyID::BorderTopWidth: {
        auto border_top_width = layout_node.computed_values().border_top().width;
        return LengthStyleValue::create(Length::make_px(border_top_width));
    }
    case PropertyID::Contain: {
        auto const& contain = layout_node.computed_values().contain();
        if (contain.layout_containment && contain.style_containment && contain.paint_containment) {
            if (contain.size_containment)
                return CSSKeywordValue::create(Keyword::Strict);
            if (!contain.inline_size_containment)
                return CSSKeywordValue::create(Keyword::Content);
        }

        return get_computed_value(property_id);
    }
    case PropertyID::OutlineWidth: {
        auto outline_width = layout_node.computed_values().outline_width();
        return LengthStyleValue::create(outline_width);
    }
    case PropertyID::WebkitTextFillColor:
        return resolve_color_style_value(get_computed_value(property_id), layout_node.computed_values().webkit_text_fill_color());
    case PropertyID::Invalid:
        return CSSKeywordValue::create(Keyword::Invalid);
    case PropertyID::Custom:
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for custom properties was requested (?)");
        return nullptr;
    default:
        // For grid-template-columns and grid-template-rows the resolved value is the used value.
        // https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
        if (property_id == PropertyID::GridTemplateColumns) {
            if (layout_node.first_paintable() && layout_node.first_paintable()->is_paintable_box()) {
                auto const& paintable_box = as<Painting::PaintableBox const>(*layout_node.first_paintable());
                if (auto used_values_for_grid_template_columns = paintable_box.used_values_for_grid_template_columns()) {
                    return used_values_for_grid_template_columns;
                }
            }
        } else if (property_id == PropertyID::GridTemplateRows) {
            if (layout_node.first_paintable() && layout_node.first_paintable()->is_paintable_box()) {
                auto const& paintable_box = as<Painting::PaintableBox const>(*layout_node.first_paintable());
                if (auto used_values_for_grid_template_rows = paintable_box.used_values_for_grid_template_rows()) {
                    return used_values_for_grid_template_rows;
                }
            }
        } else if (property_id == PropertyID::ZIndex) {
            if (auto z_index = layout_node.computed_values().z_index(); z_index.has_value()) {
                return NumberStyleValue::create(z_index.value());
            }
        }

        if (!property_is_shorthand(property_id))
            return get_computed_value(property_id);

        // Handle shorthands in a generic way
        auto longhand_ids = longhands_for_shorthand(property_id);
        StyleValueVector longhand_values;
        longhand_values.ensure_capacity(longhand_ids.size());
        for (auto longhand_id : longhand_ids)
            longhand_values.append(style_value_for_computed_property(layout_node, longhand_id).release_nonnull());
        return ShorthandStyleValue::create(property_id, move(longhand_ids), move(longhand_values));
    }
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> CSSStyleProperties::remove_property(StringView property_name)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot remove property: CSSStyleProperties is read-only."_string);

    auto property_id = property_id_from_string(property_name);
    if (!property_id.has_value())
        return String {};

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // NB: We've already converted it to a PropertyID enum value.

    // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
    auto value = get_property_value(property_name);

    Function<bool(PropertyID)> remove_declaration = [&](auto property_id) {
        // 4. Let removed be false.
        bool removed = false;

        // 5. If property is a shorthand property, for each longhand property longhand that property maps to:
        if (property_is_shorthand(property_id)) {
            for (auto longhand_property_id : longhands_for_shorthand(property_id)) {
                // 1. If longhand is not a property name of a CSS declaration in the declarations, continue.
                // 2. Remove that CSS declaration and let removed be true.
                removed |= remove_declaration(longhand_property_id);
            }
        } else {
            // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the declarations, remove that CSS declaration and let removed be true.
            if (property_id == PropertyID::Custom) {
                auto custom_name = FlyString::from_utf8_without_validation(property_name.bytes());
                removed = m_custom_properties.remove(custom_name);
            } else {
                removed = m_properties.remove_first_matching([&](auto& entry) { return entry.property_id == property_id; });
            }
        }

        return removed;
    };

    auto removed = remove_declaration(property_id.value());

    // 7. If removed is true, Update style attribute for the CSS declaration block.
    if (removed) {
        update_style_attribute();

        // Non-standard: Invalidate style for the owners of our containing sheet, if any.
        invalidate_owners(DOM::StyleInvalidationReason::CSSStylePropertiesRemoveProperty);
    }

    // 8. Return value.
    return value;
}

WebIDL::ExceptionOr<String> CSSStyleProperties::remove_property(PropertyID property_name)
{
    return remove_property(string_from_property_id(property_name));
}

// https://drafts.csswg.org/cssom/#dom-cssstyleproperties-cssfloat
String CSSStyleProperties::css_float() const
{
    // The cssFloat attribute, on getting, must return the result of invoking getPropertyValue() with float as argument.
    return get_property_value("float"sv);
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_css_float(StringView value)
{
    // On setting, the attribute must invoke setProperty() with float as first argument, as second argument the given value,
    // and no third argument. Any exceptions thrown must be re-thrown.
    return set_property("float"sv, value, ""sv);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-declaration-block
String CSSStyleProperties::serialized() const
{
    // 1. Let list be an empty array.
    Vector<String> list;

    // 2. Let already serialized be an empty array.
    HashTable<PropertyID> already_serialized;

    Function<void(PropertyID)> append_property_to_already_serialized = [&](auto property) {
        already_serialized.set(property);

        // AD-HOC: The spec assumes that we only store values against expanded longhands, there are however limited
        //         circumstances where we store against shorthands directly in addition to the expanded longhands. For
        //         example if the value of the shorthand is unresolved we store an UnresolvedStyleValue against the
        //         shorthand directly and a PendingSubstitutionStyleValue against each of the longhands. In the case we
        //         serialize a shorthand directly we should also mark it's longhands as serialized to avoid serializing
        //         them separately.
        if (property_is_shorthand(property)) {
            for (auto longhand : longhands_for_shorthand(property))
                append_property_to_already_serialized(longhand);
        }
    };

    // NB: The spec treats custom properties the same as any other property, and expects the above loop to handle them.
    //       However, our implementation separates them from regular properties, so we need to handle them separately here.
    // FIXME: Is the relative order of custom properties and regular properties supposed to be preserved?
    for (auto& declaration : m_custom_properties) {
        // 1. Let property be declaration’s property name.
        auto const& property = declaration.key;

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        // NB: It is never in already serialized, as there are no shorthands for custom properties.

        // 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in preferred order.
        // NB: There are no shorthands for custom properties.

        // 4. Shorthand loop: For each shorthand in shorthands, follow these substeps: ...
        // NB: There are no shorthands for custom properties.

        // 5. Let value be the result of invoking serialize a CSS value of declaration.
        auto value = declaration.value.value->to_string(Web::CSS::SerializationMode::Normal);

        // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value,
        //    and the important flag set if declaration has its important flag set.
        // NB: We have to inline this here as the actual implementation does not accept custom properties.
        String serialized_declaration = serialize_a_css_declaration(property, value, declaration.value.important);

        // 7. Append serialized declaration to list.
        list.append(move(serialized_declaration));

        // 8. Append property to already serialized.
        // NB: We don't need to do this, as we don't have shorthands for custom properties.
    }

    // 3. Declaration loop: For each CSS declaration declaration in declaration block’s declarations, follow these substeps:
    for (auto& declaration : m_properties) {
        // 1. Let property be declaration’s property name.
        auto property = declaration.property_id;

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        if (already_serialized.contains(property))
            continue;

        // 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in preferred order.
        if (property_maps_to_shorthand(property)) {
            auto shorthands = shorthands_for_longhand(property);

            // 4. Shorthand loop: For each shorthand in shorthands, follow these substeps:
            for (auto shorthand : shorthands) {
                // 1. Let longhands be an array consisting of all CSS declarations in declaration block’s declarations
                //    that are not in already serialized and have a property name that maps to one of the shorthand
                //    properties in shorthands.
                Vector<StyleProperty> longhands;

                for (auto const& longhand_declaration : m_properties) {
                    if (!already_serialized.contains(longhand_declaration.property_id) && shorthands_for_longhand(longhand_declaration.property_id).contains_slow(shorthand))
                        longhands.append(longhand_declaration);
                }

                // 2. If not all properties that map to shorthand are present in longhands, continue with the steps labeled shorthand loop.
                if (any_of(expanded_longhands_for_shorthand(shorthand), [&](auto longhand_id) { return !any_of(longhands, [&](auto const& longhand_declaration) { return longhand_declaration.property_id == longhand_id; }); }))
                    continue;

                // 3. Let current longhands be an empty array.
                Vector<StyleProperty> current_longhands;

                // 4. Append all CSS declarations in longhands that have a property name that maps to shorthand to current longhands.
                for (auto const& longhand : longhands) {
                    if (shorthands_for_longhand(longhand.property_id).contains_slow(shorthand))
                        current_longhands.append(longhand);
                }

                // 5. If there are one or more CSS declarations in current longhands have their important flag set and
                //    one or more with it unset, continue with the steps labeled shorthand loop.
                auto all_declarations_have_same_important_flag = true;

                for (size_t i = 1; i < current_longhands.size(); ++i) {
                    if (current_longhands[i].important != current_longhands[0].important) {
                        all_declarations_have_same_important_flag = false;
                        break;
                    }
                }

                if (!all_declarations_have_same_important_flag)
                    continue;

                // FIXME: 6. If there is any declaration in declaration block in between the first and the last longhand
                //           in current longhands which belongs to the same logical property group, but has a different
                //           mapping logic as any of the longhands in current longhands, and is not in current
                //           longhands, continue with the steps labeled shorthand loop.

                // 7. Let value be the result of invoking serialize a CSS value with current longhands.
                auto value = serialize_a_css_value(current_longhands);

                // 8. If value is the empty string, continue with the steps labeled shorthand loop.
                if (value.is_empty())
                    continue;

                // 9. Let serialized declaration be the result of invoking serialize a CSS declaration with property
                //    name shorthand, value value, and the important flag set if the CSS declarations in current
                //    longhands have their important flag set.
                auto serialized_declaration = serialize_a_css_declaration(string_from_property_id(shorthand), move(value), current_longhands.first().important);

                // 10. Append serialized declaration to list.
                list.append(move(serialized_declaration));

                // 11. Append the property names of all items of current longhands to already serialized.
                for (auto const& longhand : current_longhands)
                    append_property_to_already_serialized(longhand.property_id);

                // 12. Continue with the steps labeled declaration loop.
            }
        }

        // FIXME: File spec issue that this should only be run if we haven't serialized this declaration in the above shorthand loop.
        if (!already_serialized.contains(declaration.property_id)) {
            // 5. Let value be the result of invoking serialize a CSS value of declaration.
            auto value = serialize_a_css_value(declaration);

            // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value,
            //    and the important flag set if declaration has its important flag set.
            auto serialized_declaration = serialize_a_css_declaration(string_from_property_id(property), move(value), declaration.important);

            // 7. Append serialized declaration to list.
            list.append(move(serialized_declaration));

            // 8. Append property to already serialized.
            append_property_to_already_serialized(declaration.property_id);
        }
    }

    // 4. Return list joined with " " (U+0020).
    StringBuilder builder;
    builder.join(' ', list);
    return MUST(builder.to_string());
}

// https://www.w3.org/TR/cssom/#serialize-a-css-value
String CSSStyleProperties::serialize_a_css_value(StyleProperty const& declaration) const
{
    // 1. If If this algorithm is invoked with a list list:
    // NOTE: This is handled in other other overload of this method

    // 2. Represent the value of the declaration as a list of CSS component values components that, when parsed
    //    according to the property’s grammar, would represent that value. Additionally:
    //    - If certain component values can appear in any order without changing the meaning of the value (a pattern
    //      typically represented by a double bar || in the value syntax), reorder the component values to use the
    //      canonical order of component values as given in the property definition table.
    //    - If component values can be omitted or replaced with a shorter representation without changing the meaning
    //      of the value, omit/replace them.
    //    - If either of the above syntactic translations would be less backwards-compatible, do not perform them.

    // Spec Note: The rules described here outlines the general principles of serialization. For legacy reasons, some
    //            properties serialize in a different manner, which is intentionally undefined here due to lack of
    //            resources. Please consult your local reverse-engineer for details.

    // 3. Remove any <whitespace-token>s from components.
    // 4. Replace each component value in components with the result of invoking serialize a CSS component value.
    // 5. Join the items of components into a single string, inserting " " (U+0020 SPACE) between each pair of items
    //    unless the second item is a "," (U+002C COMMA) Return the result.

    // AD-HOC: As the spec is vague we don't follow it exactly here.
    return declaration.value->to_string(Web::CSS::SerializationMode::Normal);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-value
String CSSStyleProperties::serialize_a_css_value(Vector<StyleProperty> list) const
{
    if (list.is_empty())
        return String {};

    // 1. Let shorthand be the first shorthand property, in preferred order, that exactly maps to all of the longhand properties in list.
    Optional<PropertyID> shorthand = shorthands_for_longhand(list.first().property_id).first_matching([&](PropertyID shorthand) {
        auto longhands_for_potential_shorthand = expanded_longhands_for_shorthand(shorthand);

        // The potential shorthand exactly maps to all of the longhand properties in list if:
        // a. The number of longhand properties in the list is equal to the number of longhand properties that the potential shorthand maps to.
        if (longhands_for_potential_shorthand.size() != list.size())
            return false;

        // b. All longhand properties in the list are contained in the list of longhands for the potential shorthand.
        return all_of(longhands_for_potential_shorthand, [&](auto longhand) { return any_of(list, [&](auto const& declaration) { return declaration.property_id == longhand; }); });
    });

    // 2. If there is no such shorthand or shorthand cannot exactly represent the values of all the properties in list, return the empty string.
    if (!shorthand.has_value())
        return String {};

    // 3. Otherwise, serialize a CSS value from a hypothetical declaration of the property shorthand with its value representing the combined values of the declarations in list.
    Function<ValueComparingNonnullRefPtr<ShorthandStyleValue const>(PropertyID)> make_shorthand_value = [&](PropertyID shorthand_id) {
        auto longhand_ids = longhands_for_shorthand(shorthand_id);
        Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> longhand_values;

        for (auto longhand_id : longhand_ids) {
            if (property_is_shorthand(longhand_id))
                longhand_values.append(make_shorthand_value(longhand_id));
            else
                longhand_values.append(list.first_matching([&](auto declaration) { return declaration.property_id == longhand_id; })->value);
        }

        return ShorthandStyleValue::create(shorthand_id, longhand_ids, longhand_values);
    };

    // FIXME: Not all shorthands are represented by ShorthandStyleValue, we still need to add support for those that don't.
    return make_shorthand_value(shorthand.value())->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> CSSStyleProperties::set_css_text(StringView css_text)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly()) {
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties: CSSStyleProperties is read-only."_string);
    }

    // 2. Empty the declarations.
    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the declarations, in specified order.
    set_declarations_from_text(css_text);

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    // Non-standard: Invalidate style for the owners of our containing sheet, if any.
    invalidate_owners(DOM::StyleInvalidationReason::CSSStylePropertiesTextChange);

    return {};
}

void CSSStyleProperties::invalidate_owners(DOM::StyleInvalidationReason reason)
{
    if (auto rule = parent_rule()) {
        if (auto sheet = rule->parent_style_sheet()) {
            sheet->invalidate_owners(reason);
        }
    }
}

// https://drafts.csswg.org/cssom/#set-a-css-declaration
bool CSSStyleProperties::set_a_css_declaration(PropertyID property_id, NonnullRefPtr<CSSStyleValue const> value, Important important)
{
    VERIFY(!is_computed());

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

    m_properties.append(StyleProperty {
        .important = important,
        .property_id = property_id,
        .value = move(value),
    });
    return true;
}

void CSSStyleProperties::empty_the_declarations()
{
    m_properties.clear();
    m_custom_properties.clear();
}

void CSSStyleProperties::set_the_declarations(Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties)
{
    m_properties = convert_declarations_to_specified_order(properties);
    m_custom_properties = move(custom_properties);
}

void CSSStyleProperties::set_declarations_from_text(StringView css_text)
{
    empty_the_declarations();
    auto parsing_params = owner_node().has_value()
        ? Parser::ParsingParams(owner_node()->element().document())
        : Parser::ParsingParams();
    parsing_params.rule_context.append(Parser::RuleContext::Style);

    auto style = parse_css_property_declaration_block(parsing_params, css_text);
    set_the_declarations(style.properties, style.custom_properties);
}

}
