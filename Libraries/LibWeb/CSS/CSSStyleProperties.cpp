/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::CSS {

static Atomic<u64> s_next_css_style_properties_identity { 1 };

GC_DEFINE_ALLOCATOR(CSSStyleProperties);

GC::Ref<CSSStyleProperties> CSSStyleProperties::create(Vector<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties)
{
    // https://drafts.csswg.org/cssom/#dom-cssstylerule-style
    // The style attribute must return a CSSStyleProperties object for the style rule, with the following properties:
    //     computed flag: Unset.
    //     readonly flag: Unset.
    //     declarations: The declared declarations in the rule, in specified order.
    //     parent CSS rule: The context object.
    //     owner node: Null.
    return GC::Heap::the().allocate<CSSStyleProperties>(Computed::No, Readonly::No, convert_declarations_to_specified_order(properties), move(custom_properties), OptionalNone {});
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_resolved_style(Optional<DOM::AbstractElement> element_reference)
{
    // https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle
    // 6.  Return a live CSSStyleProperties object with the following properties:
    //     computed flag: Set.
    //     readonly flag: Set.
    //     declarations: decls.
    //     parent CSS rule: Null.
    //     owner node: obj.
    // AD-HOC: Rather than instantiate with a list of decls, they're generated on demand.
    return GC::Heap::the().allocate<CSSStyleProperties>(Computed::Yes, Readonly::Yes, Vector<StyleProperty> {}, OrderedHashMap<Utf16FlyString, StyleProperty> {}, move(element_reference));
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_element_inline_style(DOM::AbstractElement element_reference, Vector<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties)
{
    // https://drafts.csswg.org/cssom/#dom-elementcssinlinestyle-style
    // The style attribute must return a CSS declaration block object whose readonly flag is unset, whose parent CSS
    // rule is null, and whose owner node is the context object.
    return GC::Heap::the().allocate<CSSStyleProperties>(Computed::No, Readonly::No, convert_declarations_to_specified_order(properties), move(custom_properties), move(element_reference));
}

CSSStyleProperties::CSSStyleProperties(Computed computed, Readonly readonly, Vector<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties, Optional<DOM::AbstractElement> owner_node)
    : CSSStyleDeclaration(computed, readonly)
    , m_properties(move(properties))
    , m_custom_properties(move(custom_properties))
    , m_identity(s_next_css_style_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
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
        StyleComputer::for_each_property_expanding_shorthands(declaration.property_id, declaration.value, [&](CSS::PropertyID longhand_id, CSS::StyleValue const& longhand_property_value) {
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

size_t CSSStyleProperties::external_memory_size() const
{
    auto size = Base::external_memory_size();
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_properties));
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_custom_properties));
    return size;
}

// https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle
static bool element_exposes_computed_style(DOM::Element const& element)
{
    // NB: This is a partial enforcement of step 5:
    // If [...] elt is connected, part of the flat tree, and its shadow-including root has a browsing context which
    // either doesn't have a browsing context container, or whose browsing context container is being rendered.
    if (!element.is_connected())
        return false;
    if (!element.shadow_including_root().document().browsing_context())
        return false;
    // FIXME: Check if the element is part of the flat tree.
    // FIXME: Check that the browsing context either doesn't have a browsing context container, or that its
    //        browsing context container is being rendered.
    return true;
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
size_t CSSStyleProperties::length() const
{
    // The length attribute must return the number of CSS declarations in the declarations.
    if (is_computed()) {
        if (!owner_node().has_value())
            return 0;
        if (!element_exposes_computed_style(owner_node()->element()))
            return 0;
        return number_of_longhand_properties;
    }

    return m_properties.size() + m_custom_properties.size();
}

Utf16String CSSStyleProperties::item(size_t index) const
{
    // The item(index) method must return the property name of the CSS declaration at position index.
    // If there is no indexth object in the collection, then the method must return the empty string.
    auto custom_properties_count = m_custom_properties.size();

    if (index >= length())
        return {};

    if (is_computed()) {
        auto property_id = static_cast<PropertyID>(index + to_underlying(first_longhand_property_id));
        return string_from_property_id(property_id).to_utf16_string();
    }

    if (index < custom_properties_count)
        return m_custom_properties.keys()[index].to_utf16_string();

    return string_from_property_id(m_properties[index - custom_properties_count].property_id).to_utf16_string();
}

Optional<StyleProperty> CSSStyleProperties::get_property(PropertyID property_id) const
{
    return get_property_internal(PropertyNameAndID::from_id(property_id));
}

Optional<StyleProperty const&> CSSStyleProperties::custom_property(Utf16FlyString const& custom_property_name) const
{
    if (is_computed()) {
        if (!owner_node().has_value())
            return {};

        auto& element = owner_node()->element();
        auto pseudo_element = owner_node()->pseudo_element();

        element.document().update_style_for_element(*owner_node());

        auto data = element.custom_property_data(pseudo_element);
        if (!data)
            return {};

        if (auto const* property = data->get(custom_property_name))
            return *property;

        return {};
    }

    return m_custom_properties.get(custom_property_name);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> CSSStyleProperties::set_property(Utf16FlyString const& property_name, Utf16View value, Utf16View priority)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    if (is_computed())
        return WebIDL::NoModificationAllowedError::create("Cannot modify properties in result of getComputedStyle()"_utf16);

    // 2. If property is not a custom property, follow these substeps:
    //    1. Let property be property converted to ASCII lowercase.
    //    2. If property is not a case-sensitive match for a supported CSS property, then return.
    // NB: This is handled inside PropertyNameAndID::from_string().
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return {};

    if (value.is_empty() && is_readonly())
        return WebIDL::NoModificationAllowedError::create("Cannot remove property: CSSStyleProperties is read-only."_utf16);

    // NB: The remaining steps are implemented in set_property_internal().
    return set_property_internal(property.release_value(), value, priority);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> CSSStyleProperties::set_property_internal(PropertyNameAndID const& property, Utf16View value, Utf16View priority)
{
    // NB: Steps 1 and 2 only apply to the IDL method that invokes this.

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        auto removed_value = MUST(remove_property_internal(property));
        (void)removed_value;
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !priority.equals_ignoring_ascii_case(u"important"sv))
        return {};

    // 5. Let component value list be the result of parsing value for property property.
    auto component_value_list = owner_node().has_value()
        ? parse_css_value(Parser::ParsingParams { owner_node()->element().document() }, value, property.id())
        : parse_css_value(Parser::ParsingParams {}, value, property.id());

    // 6. If component value list is null, then return.
    if (!component_value_list)
        return {};

    prepare_to_update_style_attribute();

    // 7. Let updated be false.
    bool updated = false;

    // 8. If property is a shorthand property,
    if (property_is_shorthand(property.id())) {
        // then for each longhand property longhand that property maps to, in canonical order, follow these substeps:
        StyleComputer::for_each_property_expanding_shorthands(property.id(), *component_value_list, [this, &updated, priority](PropertyID longhand_property_id, StyleValue const& longhand_value) {
            // 1. Let longhand result be the result of set the CSS declaration longhand with the appropriate value(s) from component value list,
            //    with the important flag set if priority is not the empty string, and unset otherwise, and with the list of declarations being the declarations.
            // 2. If longhand result is true, let updated be true.
            updated |= set_a_css_declaration(longhand_property_id, longhand_value, !priority.is_empty() ? Important::Yes : Important::No);
        });
    }
    // 9. Otherwise,
    else {
        if (property.is_custom_property()) {
            auto important = !priority.is_empty() ? Important::Yes : Important::No;
            StyleProperty style_property {
                .important = important,
                .property_id = property.id(),
                .value = component_value_list.release_nonnull(),
            };
            if (auto existing_property = custom_property(property.name()); existing_property.has_value()
                && existing_property->important == important
                && *existing_property->value == *style_property.value) {
                updated = false;
            } else {
                m_custom_properties.set(property.name(), style_property);
                updated = true;
            }
        } else {
            // let updated be the result of set the CSS declaration property with value component value list,
            // with the important flag set if priority is not the empty string, and unset otherwise,
            // and with the list of declarations being the declarations.
            updated = set_a_css_declaration(property.id(), *component_value_list, !priority.is_empty() ? Important::Yes : Important::No);
        }
    }

    // 10. If updated is true, update style attribute for the CSS declaration block.
    if (updated) {
        ++m_revision;
        update_style_attribute();

        // Non-standard: Invalidate style for the owners of our containing sheet, if any.
        invalidate_owners();
    }

    return {};
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_property(PropertyID property_id, Utf16View css_text, Utf16View priority)
{
    VERIFY(!is_computed());
    return set_property_internal(PropertyNameAndID::from_id(property_id), css_text, priority);
}

static NonnullRefPtr<StyleValue const> style_value_for_css_pixels(CSSPixels css_pixels)
{
    return LengthStyleValue::create(Length::make_px(css_pixels));
}

// https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
static NonnullRefPtr<StyleValue const> style_value_for_used_grid_track_list(Painting::UsedGridTrackList const& used_values)
{
    auto result = used_values.is_subgrid ? GridTrackSizeList::make_subgrid() : GridTrackSizeList::make_none();
    for (size_t line_index = 0; line_index < used_values.lines.size(); ++line_index) {
        auto line_names = used_values.lines[line_index];
        if (used_values.is_subgrid || !line_names.is_empty())
            result.append(move(line_names));

        if (line_index < used_values.track_sizes.size()) {
            result.append(ExplicitGridTrack {
                GridSize { LengthStyleValue::create(Length::make_px(used_values.track_sizes[line_index])) },
            });
        }
    }
    return GridTrackSizeListStyleValue::create(move(result));
}

static NonnullRefPtr<StyleValue const> style_value_for_length_percentage(LengthPercentage const& length_percentage)
{
    if (length_percentage.is_percentage())
        return PercentageStyleValue::create(length_percentage.percentage());
    if (length_percentage.is_length())
        return LengthStyleValue::create(length_percentage.length());
    return length_percentage.calculated();
}

static NonnullRefPtr<StyleValue const> style_value_for_length_percentage_or_auto(LengthPercentageOrAuto const& length_percentage)
{
    if (length_percentage.is_auto())
        return KeywordStyleValue::create(Keyword::Auto);
    if (length_percentage.is_percentage())
        return PercentageStyleValue::create(length_percentage.percentage());
    if (length_percentage.is_length())
        return LengthStyleValue::create(length_percentage.length());
    return length_percentage.calculated();
}

static NonnullRefPtr<StyleValue const> style_value_for_transform_origin(TransformOrigin const& transform_origin, Optional<CSSPixelRect> const& reference_box)
{
    auto offset_in_px = [](LengthPercentage const& offset, CSSPixels reference_length) {
        if (offset.is_percentage())
            return reference_length.to_double() * offset.percentage().as_fraction();
        return offset.resolved(reference_length).absolute_length_to_px_without_rounding();
    };

    StyleValueVector offsets;
    if (reference_box.has_value()) {
        offsets.append(LengthStyleValue::create(Length::make_px(offset_in_px(transform_origin.x, reference_box->width()))));
        offsets.append(LengthStyleValue::create(Length::make_px(offset_in_px(transform_origin.y, reference_box->height()))));
    } else {
        offsets.append(style_value_for_length_percentage(transform_origin.x));
        offsets.append(style_value_for_length_percentage(transform_origin.y));
    }
    if (auto z_offset = offset_in_px(transform_origin.z, 0); z_offset != 0)
        offsets.append(LengthStyleValue::create(Length::make_px(z_offset)));
    return StyleValueList::create(move(offsets), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
}

static NonnullRefPtr<StyleValue const> style_value_for_size(Size const& size)
{
    if (size.is_none())
        return KeywordStyleValue::create(Keyword::None);
    if (size.is_percentage())
        return PercentageStyleValue::create(size.percentage());
    if (size.is_length())
        return LengthStyleValue::create(size.length());
    if (size.is_auto())
        return KeywordStyleValue::create(Keyword::Auto);
    if (size.is_calculated())
        return size.calculated();
    if (size.is_min_content())
        return KeywordStyleValue::create(Keyword::MinContent);
    if (size.is_max_content())
        return KeywordStyleValue::create(Keyword::MaxContent);
    if (size.is_fit_content()) {
        if (auto available_space = size.fit_content_available_space(); available_space.has_value())
            return FunctionStyleValue::create("fit-content"_utf16_fly_string, style_value_for_length_percentage(available_space.release_value()));
        return KeywordStyleValue::create(Keyword::FitContent);
    }
    TODO();
}

static RefPtr<StyleValue const> style_value_for_shadow(ShadowStyleValue::ShadowType shadow_type, ReadonlySpan<ShadowData> shadow_data)
{
    if (shadow_data.is_empty())
        return KeywordStyleValue::create(Keyword::None);

    auto make_shadow_style_value = [shadow_type](ShadowData const& shadow) {
        return ShadowStyleValue::create(
            shadow_type,
            ColorStyleValue::create_from_color(shadow.color, ColorSyntax::Modern),
            style_value_for_css_pixels(shadow.offset_x),
            style_value_for_css_pixels(shadow.offset_y),
            style_value_for_css_pixels(shadow.blur_radius),
            style_value_for_css_pixels(shadow.spread_distance),
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

// The stored computed filter value keeps currentcolor inside drop-shadow(); the resolved value
// serializes it against the element's used color, like the color-valued properties.
static NonnullRefPtr<StyleValue const> resolve_filter_style_value(NonnullRefPtr<StyleValue const> value, Color current_color)
{
    if (!value->is_value_list())
        return value;
    auto const& list = value->as_value_list();
    StyleValueVector filters;
    MUST(filters.try_ensure_capacity(list.size()));
    bool resolved_any = false;
    for (auto const& filter_value : list.values()) {
        if (filter_value->is_filter() && filter_value->as_filter().kind() == FilterStyleValue::Kind::DropShadow) {
            auto const& drop_shadow = static_cast<DropShadowFilterStyleValue const&>(filter_value->as_filter());
            auto const& drop_shadow_color = drop_shadow.color();
            if (!drop_shadow_color || drop_shadow_color->to_keyword() == Keyword::Currentcolor) {
                filters.unchecked_append(DropShadowFilterStyleValue::create(
                    drop_shadow.offset_x(),
                    drop_shadow.offset_y(),
                    drop_shadow.radius(),
                    ColorStyleValue::create_from_color(current_color, ColorSyntax::Modern)));
                resolved_any = true;
                continue;
            }
        }
        filters.unchecked_append(filter_value);
    }
    if (!resolved_any)
        return value;
    return StyleValueList::create(move(filters), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
}

// The canonical serialization of the computed touch-action flags; notably, allowing exactly
// panning and pinch-zoom serializes as `manipulation`, whatever form specified it.
static NonnullRefPtr<StyleValue const> style_value_for_touch_action(TouchActionData const& action)
{
    if (action.allow_left && action.allow_right && action.allow_up && action.allow_down && action.allow_pinch_zoom) {
        if (action.allow_other)
            return KeywordStyleValue::create(Keyword::Auto);
        return KeywordStyleValue::create(Keyword::Manipulation);
    }
    if (!action.allow_left && !action.allow_right && !action.allow_up && !action.allow_down && !action.allow_pinch_zoom)
        return KeywordStyleValue::create(Keyword::None);

    StyleValueVector values;
    if (action.allow_left && action.allow_right)
        values.append(KeywordStyleValue::create(Keyword::PanX));
    else if (action.allow_left)
        values.append(KeywordStyleValue::create(Keyword::PanLeft));
    else if (action.allow_right)
        values.append(KeywordStyleValue::create(Keyword::PanRight));
    if (action.allow_up && action.allow_down)
        values.append(KeywordStyleValue::create(Keyword::PanY));
    else if (action.allow_up)
        values.append(KeywordStyleValue::create(Keyword::PanUp));
    else if (action.allow_down)
        values.append(KeywordStyleValue::create(Keyword::PanDown));
    if (action.allow_pinch_zoom)
        values.append(KeywordStyleValue::create(Keyword::PinchZoom));
    return StyleValueList::create(move(values), StyleValueList::Separator::Space);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
Utf16String CSSStyleProperties::get_property_value(Utf16FlyString const& property_name) const
{
    if (auto property = PropertyNameAndID::from_name(property_name); property.has_value()) {
        if (is_computed() && !property->is_custom_property()) {
            if (auto serialized = serialized_computed_value_from_stored_handle(property->id()); serialized.has_value())
                return serialized.release_value();
        }
        if (auto style_property = get_property_internal(*property); style_property.has_value())
            return style_property->value->to_utf16_string(is_computed() ? SerializationMode::ResolvedValue : SerializationMode::Normal);
    }

    return {};
}
// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
Utf16String CSSStyleProperties::get_property_priority(Utf16FlyString const& property_name) const
{
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return {};
    if (property->is_custom_property()) {
        auto maybe_custom_property = custom_property(property_name);
        if (!maybe_custom_property.has_value())
            return {};
        return maybe_custom_property.value().important == Important::Yes ? "important"_utf16 : Utf16String {};
    }
    auto maybe_property = get_property_internal(property.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->important == Important::Yes ? "important"_utf16 : Utf16String {};
}

bool CSSStyleProperties::has_property(PropertyNameAndID const& property) const
{
    return get_property_internal(property).has_value();
}

bool CSSStyleProperties::has_property(PropertyID property_id) const
{
    return has_property(PropertyNameAndID::from_id(property_id));
}

RefPtr<StyleValue const> CSSStyleProperties::get_property_style_value(PropertyNameAndID const& property) const
{
    if (auto style_property = get_property_internal(property); style_property.has_value())
        return style_property->value;
    return nullptr;
}

RefPtr<StyleValue const> CSSStyleProperties::get_property_style_value(PropertyID property_id) const
{
    return get_property_style_value(PropertyNameAndID::from_id(property_id));
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_property_style_value(PropertyNameAndID const& property, NonnullRefPtr<StyleValue const> style_value)
{
    if (is_computed()) {
        return WebIDL::NoModificationAllowedError::create("Cannot modify properties in result of getComputedStyle()"_utf16);
    }

    if (property.is_custom_property()) {
        if (auto existing_property = custom_property(property.name()); existing_property.has_value()
            && existing_property->important == Important::No
            && *existing_property->value == *style_value) {
            return {};
        }

        prepare_to_update_style_attribute();

        m_custom_properties.set(property.name(),
            StyleProperty {
                Important::No,
                PropertyID::Custom,
                style_value });

        ++m_revision;
        update_style_attribute();
        invalidate_owners();

        return {};
    }

    // FIXME: This should have been rejected earlier, but property_accepts_type() is too basic for what we need.
    if (property_is_positional_value_list_shorthand(property.id())
        && !style_value->is_shorthand()
        && !style_value->is_unresolved()
        && !style_value->is_pending_substitution()
        && !style_value->is_guaranteed_invalid()
        && !style_value->is_css_wide_keyword()) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Setting {} to '{}' is not allowed.", property.name(), style_value->to_string(SerializationMode::Normal)) };
    }

    if (first_is_one_of(property.id(), PropertyID::BackdropFilter, PropertyID::Filter)
        && style_value->is_value_list()
        && !is_filter_style_value_list(*style_value)) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Setting {} to '{}' is not allowed.", property.name(), style_value->to_string(SerializationMode::Normal)) };
    }

    prepare_to_update_style_attribute();

    StyleComputer::for_each_property_expanding_shorthands(property.id(), style_value, [this](PropertyID longhand_id, StyleValue const& longhand_value) {
        m_properties.remove_first_matching([longhand_id](StyleProperty const& style_property) {
            return style_property.property_id == longhand_id;
        });
        m_properties.append(StyleProperty {
            .important = Important::No,
            .property_id = longhand_id,
            .value = longhand_value,
        });
    });

    ++m_revision;
    update_style_attribute();
    invalidate_owners();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
Optional<StyleProperty> CSSStyleProperties::get_property_internal(PropertyNameAndID const& property) const
{
    // NB: This is our own method to get a StyleProperty, but following the algorithm for getPropertyValue() which
    //     returns a String. (This way, we can use the same logic in other places.) That's why the spec steps talk
    //     about strings and then we do something different.

    // 1. If property is not a custom property, follow these substeps:
    if (!property.is_custom_property()) {
        // 1. Let property be property converted to ASCII lowercase.
        // NB: Done already by PropertyNameAndID.

        // 2. If property is a shorthand property, then follow these substeps:
        if (property_is_shorthand(property.id())) {
            // 1. Let list be a new empty array.
            Vector<ValueComparingNonnullRefPtr<StyleValue const>> list;
            Optional<Important> last_important_flag;

            // 2. For each longhand property longhand that property maps to, in canonical order, follow these substeps:
            Vector<PropertyID> longhand_ids = longhands_for_shorthand(property.id());
            for (auto longhand_property_id : longhand_ids) {
                // 1. If longhand is a case-sensitive match for a property name of a CSS declaration in the declarations,
                //    let declaration be that CSS declaration, or null otherwise.
                auto declaration = get_property_internal(PropertyNameAndID::from_id(longhand_property_id));

                // 2. If declaration is null, then return the empty string.
                if (!declaration.has_value())
                    return {};

                // 3. Append the declaration to list.
                list.append(declaration->value);

                if (last_important_flag.has_value() && declaration->important != *last_important_flag)
                    return {};
                last_important_flag = declaration->important;
            }

            // https://drafts.csswg.org/css-values-5/#pending-substitution-value
            // If all of the component longhand properties for a given shorthand are pending-substitution values from
            // the same original shorthand value, the shorthand property must serialize to that original
            // (arbitrary substitution function-containing) value.
            // Otherwise, if any of the component longhand properties for a given shorthand are pending-substitution
            // values, or contain arbitrary substitution functions of their own that have not yet been substituted, the
            // shorthand property must serialize to the empty string.
            if (list.first()->is_pending_substitution()) {
                auto original_shorthand_value = list.first()->as_pending_substitution().original_shorthand_value();
                auto all_from_same_original = all_of(list, [&](auto const& value) {
                    return value->is_pending_substitution()
                        && value->as_pending_substitution().original_shorthand_value()->rust_style_value_data() == original_shorthand_value->rust_style_value_data();
                });
                if (all_from_same_original) {
                    return StyleProperty {
                        .important = last_important_flag.value(),
                        .property_id = property.id(),
                        .value = original_shorthand_value,
                    };
                }
            }
            if (any_of(list, [](auto const& value) { return value->is_pending_substitution() || value->is_unresolved(); }))
                return {};

            // 3. If important flags of all declarations in list are same, then return the serialization of list.
            // NOTE: Currently we implement property-specific shorthand serialization in ShorthandStyleValue::to_string().
            return StyleProperty {
                .important = last_important_flag.value(),
                .property_id = property.id(),
                .value = ShorthandStyleValue::create(property.id(), longhand_ids, list),
            };

            // 4. Return the empty string.
            // NOTE: This is handled by the loop.
        }
    }

    // 2. If property is a case-sensitive match for a property name of a CSS declaration in the declarations, then
    //    return the result of invoking serialize a CSS value of that declaration.
    // 3. Return the empty string.
    return get_direct_property(property);
}

static void ensure_pseudo_element_style_for_cssom(DOM::AbstractElement abstract_element)
{
    auto pseudo_element = abstract_element.pseudo_element();
    if (!pseudo_element.has_value())
        return;
    if (!is_synthetic_pseudo_element(*pseudo_element))
        return;
    if (*pseudo_element != PseudoElement::Backdrop && abstract_element.computed_style())
        return;

    auto& document = abstract_element.document();
    document.begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        document.end_style_stabilization_epoch();
    };
    auto& style_computer = abstract_element.document().style_computer();

    bool did_change_custom_properties = false;
    StyleEngine::StyleRecordDelta style_record_delta {};
    auto style = style_computer.compute_pseudo_element_style_if_needed(abstract_element, did_change_custom_properties, nullptr, style_record_delta);
    if (style)
        abstract_element.element().set_computed_style(*pseudo_element, style_record_delta.new_style_record);
    else
        abstract_element.element().set_computed_style(*pseudo_element, 0);
}

static RefPtr<StyleValue const> resolve_color_style_value(StyleValue const&, Color, ColorResolutionContext const* = nullptr);

// Brings style (and, when the property needs it, layout) up to date for computed-style property
// access, and returns the layout node to read used values from (may be null). An empty Optional
// means the element cannot expose computed style at all (disconnected, or no browsing context).
static Optional<Layout::NodeWithStyle*> prepare_computed_style_and_layout_for_property(DOM::AbstractElement abstract_element, PropertyID property_id)
{
    if (!element_exposes_computed_style(abstract_element.element()))
        return {};

    // NB: We grab the layout node before deciding whether update_layout() is needed.
    //     For properties that don't need layout or a layout node (the else branch below),
    //     we skip update_layout() entirely and use whatever layout node already exists.
    //     For the other paths, we call update_layout() and re-fetch below.
    Layout::NodeWithStyle* layout_node = abstract_element.unsafe_layout_node();

    // Determine what work is needed for this property:
    // 1. Properties that need layout computation (used values) - always run update_layout()
    // 2. Properties that need a layout node for special resolution - ensure layout node exists
    // 3. Everything else - just update_style() and return computed value
    bool const needs_layout = property_needs_layout_for_getcomputedstyle(property_id);
    bool const needs_layout_node = property_needs_layout_node_for_resolved_value(property_id);

    if (needs_layout || needs_layout_node) {
        // Properties that need layout computation or layout node for special resolution
        // always need update_layout() to ensure both style and layout tree are up to date.
        abstract_element.document().update_layout(DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
        layout_node = abstract_element.layout_node();
    }
    // Ensure styles are up to date. update_layout()/update_style() skip display:none subtrees,
    // so the leaf and its inheritance ancestors may still be stale at this point.
    // NB: Only the style record's presence and its display:none-subtree bit matter here, so probe
    //     those directly instead of materializing a full style record view.
    auto style_record = abstract_element.style_record_identity();
    bool const style_is_in_display_none_subtree = !layout_node
        && !!style_record
        && (abstract_element.document().style_computer().style_engine().style_record_dependency_flags(style_record) & to_underlying(StyleRecordDependencyFlag::InDisplayNoneSubtree));
    if (!style_record || style_is_in_display_none_subtree)
        abstract_element.document().update_style_for_element(abstract_element);
    else
        abstract_element.document().update_style_for_element(abstract_element, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
    ensure_pseudo_element_style_for_cssom(abstract_element);

    // Container queries and container-relative units need layout to resolve. Avoid forcing layout for every
    // getComputedStyle() call; only elements that actually depend on a query container need the post-layout style.
    bool style_or_inheritance_ancestor_depends_on_size_container_query = abstract_element.element().style_depends_on_size_container_query();
    for (auto ancestor = abstract_element.element_to_inherit_style_from();
        ancestor.has_value() && !style_or_inheritance_ancestor_depends_on_size_container_query;
        ancestor = ancestor->element_to_inherit_style_from()) {
        style_or_inheritance_ancestor_depends_on_size_container_query = ancestor->element().style_depends_on_size_container_query();
    }
    bool const needs_layout_for_container_queries = style_or_inheritance_ancestor_depends_on_size_container_query
        && !abstract_element.document().layout_is_up_to_date();
    if (needs_layout_for_container_queries) {
        abstract_element.document().update_layout(DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
        layout_node = abstract_element.layout_node();
        // A synthetic pseudo which is not rendered is not part of the layout-driven pseudo
        // recomputation above. Refresh its CSSOM-only style against the settled container size.
        ensure_pseudo_element_style_for_cssom(abstract_element);
    }

    if (auto pseudo_element = abstract_element.pseudo_element(); layout_node && pseudo_element.has_value()) {
        auto pseudo_style = abstract_element.element().computed_style(*pseudo_element);
        if (!pseudo_style || pseudo_style->display().is_contents())
            layout_node = nullptr;
    }

    return layout_node;
}

Optional<RefPtr<StyleValue const>> CSSStyleProperties::resolved_value_read_from_computed_style(DOM::AbstractElement abstract_element, PropertyID property_id)
{
    // Only properties whose resolved value is the plain computed value qualify: no used-value substitution (see
    // property_needs_layout_for_getcomputedstyle() and property_needs_layout_node_for_resolved_value()): No color or
    // shadow resolution, no logical alias mapping, and no shorthand reconstruction. get_direct_property() and
    // style_value_for_computed_property() enumerate the properties that need such treatment.
    switch (property_id) {
    case PropertyID::Display:
    case PropertyID::FontFamily:
    case PropertyID::FontSize:
    case PropertyID::FontStyle:
    case PropertyID::FontWeight:
    case PropertyID::TextAlign:
    case PropertyID::TextDecorationLine:
    case PropertyID::WhiteSpaceCollapse:
        break;
    default:
        return OptionalNone {};
    }

    auto prepared = prepare_computed_style_and_layout_for_property(abstract_element, property_id);
    if (!prepared.has_value())
        return RefPtr<StyleValue const> {};

    if (auto style = abstract_element.computed_style())
        return RefPtr<StyleValue const> { style->computed_style_value(property_id) };

    // Only a synthetic pseudo-element with no matching rules lacks a style record after the preparation above; its
    // transient style needs the full declaration path.
    return OptionalNone {};
}

Optional<StyleProperty> CSSStyleProperties::get_direct_property(PropertyNameAndID const& property_name_and_id) const
{
    auto const property_id = property_name_and_id.id();

    if (is_computed()) {
        if (!owner_node().has_value())
            return {};

        auto abstract_element = *owner_node();

        auto maybe_layout_node = prepare_computed_style_and_layout_for_property(abstract_element, property_id);
        if (!maybe_layout_node.has_value())
            return {};
        auto* layout_node = *maybe_layout_node;

        // FIXME: Somehow get custom properties if there's no layout node.
        if (property_name_and_id.is_custom_property()) {
            if (auto maybe_value = abstract_element.get_custom_property(property_name_and_id.name())) {
                return StyleProperty {
                    .property_id = property_id,
                    .value = maybe_value.release_nonnull(),
                };
            }
            // Pseudo-elements may have no own custom-property data if the matching rule targeted the originating
            // element rather than the pseudo-element itself (for example `::slotted(...)`).
            // In that case, getComputedStyle(..., "::before") still needs to expose inherited custom properties from
            // the originating element.
            if (abstract_element.pseudo_element().has_value()) {
                if (auto inherit_from = abstract_element.element_to_inherit_style_from(); inherit_from.has_value()) {
                    if (auto maybe_value = inherit_from->get_custom_property(property_name_and_id.name())) {
                        return StyleProperty {
                            .property_id = property_id,
                            .value = maybe_value.release_nonnull(),
                        };
                    }
                }
            }
            // FIXME: Currently, to get the initial value for a registered custom property we have to look at the document.
            //        These should be cascaded like other properties.
            if (auto maybe_value = abstract_element.document().get_registered_custom_property(property_name_and_id.name()); maybe_value.has_value() && maybe_value->initial_value) {
                return StyleProperty {
                    .property_id = property_id,
                    .value = *maybe_value->initial_value,
                };
            }

            return {};
        }

        if (!layout_node) {
            auto style_record = abstract_element.computed_style();
            RefPtr<ComputedValues const> transient_style;
            if (!style_record) {
                // A synthetic pseudo-element without matching rules has no durable style.
                transient_style = abstract_element.document().style_computer().materialize_style_record(abstract_element);
            }
            auto const* computed_values = style_record ? &*style_record : transient_style.ptr();
            VERIFY(computed_values);

            auto computed_value_for_property = [&](PropertyID computed_property_id) -> NonnullRefPtr<StyleValue const> {
                if (property_is_logical_alias(computed_property_id))
                    computed_property_id = map_logical_alias_to_physical_property(computed_property_id, LogicalAliasMappingContext { computed_values->writing_mode(), computed_values->direction() });
                if (computed_property_id == PropertyID::BackgroundColor) {
                    if (auto style_value = computed_values->background_color_style_value(); style_value && !style_value->depends_on_current_color())
                        return style_value.release_nonnull();
                }
                auto computed_value = computed_values->computed_style_value(computed_property_id).release_nonnull();
                // The same resolved-value special cases the layout-node path applies, read from
                // the style's own resolved group data since there is no layout node to ask.
                switch (computed_property_id) {
                case PropertyID::Color: {
                    ColorResolutionContext color_resolution_context {
                        .color_scheme = computed_values->color_scheme(),
                        .current_color = computed_values->color(),
                        .current_color_style_value = computed_values->color_style_value(),
                        .calculation_resolution_context = { .length_resolution_context = Length::ResolutionContext::for_element(abstract_element, *computed_values) },
                    };
                    return resolve_color_style_value(*computed_value, computed_values->color(), &color_resolution_context).release_nonnull();
                }
                case PropertyID::CaretColor:
                    return resolve_color_style_value(*computed_value, computed_values->caret_color()).release_nonnull();
                case PropertyID::BackgroundColor:
                    return resolve_color_style_value(*computed_value, computed_values->background_color()).release_nonnull();
                case PropertyID::BorderBottomColor:
                    return resolve_color_style_value(*computed_value, computed_values->border_bottom().color).release_nonnull();
                case PropertyID::BorderLeftColor:
                    return resolve_color_style_value(*computed_value, computed_values->border_left().color).release_nonnull();
                case PropertyID::BorderRightColor:
                    return resolve_color_style_value(*computed_value, computed_values->border_right().color).release_nonnull();
                case PropertyID::BorderTopColor:
                    return resolve_color_style_value(*computed_value, computed_values->border_top().color).release_nonnull();
                case PropertyID::OutlineColor:
                    return resolve_color_style_value(*computed_value, computed_values->outline_color()).release_nonnull();
                case PropertyID::TextDecorationColor:
                    return resolve_color_style_value(*computed_value, computed_values->text_decoration_color()).release_nonnull();
                case PropertyID::WebkitTextFillColor:
                    return resolve_color_style_value(*computed_value, computed_values->webkit_text_fill_color()).release_nonnull();
                case PropertyID::BoxShadow:
                    return style_value_for_shadow(ShadowStyleValue::ShadowType::Normal, computed_values->box_shadow()).release_nonnull();
                case PropertyID::TextShadow:
                    return style_value_for_shadow(ShadowStyleValue::ShadowType::Text, computed_values->text_shadow()).release_nonnull();
                case PropertyID::BackdropFilter:
                case PropertyID::Filter:
                    return resolve_filter_style_value(move(computed_value), computed_values->color());
                case PropertyID::TouchAction:
                    return style_value_for_touch_action(computed_values->touch_action());
                case PropertyID::TransformOrigin:
                    return style_value_for_transform_origin(computed_values->transform_origin(), {});
                default:
                    return computed_value;
                }
            };

            if (property_is_shorthand(property_id)) {
                auto longhand_ids = longhands_for_shorthand(property_id);
                StyleValueVector longhand_values;
                longhand_values.ensure_capacity(longhand_ids.size());
                for (auto longhand_id : longhand_ids)
                    longhand_values.append(computed_value_for_property(longhand_id));
                return StyleProperty {
                    .property_id = property_id,
                    .value = ShorthandStyleValue::create(property_id, move(longhand_ids), move(longhand_values)),
                };
            }

            return StyleProperty {
                .property_id = property_id,
                .value = computed_value_for_property(property_id),
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

    if (property_name_and_id.is_custom_property())
        return custom_property(property_name_and_id.name()).copy();

    for (auto const& property : m_properties) {
        if (property.property_id == property_id)
            return property;
    }
    return {};
}

// The properties eligible for serializing straight from the stored Rust style value handle:
// their computed value is kept as a handle (see ComputedValues::stored_style_value_handle()),
// and their resolved value is the computed value, except for the guards applied in
// serialized_computed_value_from_stored_handle() below.
static bool property_computed_value_may_be_stored_as_style_value_handle(PropertyID property_id)
{
    switch (property_id) {
    case PropertyID::Cx:
    case PropertyID::Cy:
    case PropertyID::D:
    case PropertyID::GridAutoColumns:
    case PropertyID::GridAutoRows:
    case PropertyID::GridColumnEnd:
    case PropertyID::GridColumnStart:
    case PropertyID::GridRowEnd:
    case PropertyID::GridRowStart:
    case PropertyID::GridTemplateAreas:
    case PropertyID::GridTemplateColumns:
    case PropertyID::GridTemplateRows:
    case PropertyID::LetterSpacing:
    case PropertyID::R:
    case PropertyID::Rx:
    case PropertyID::Ry:
    case PropertyID::WordSpacing:
    case PropertyID::X:
    case PropertyID::Y:
        return true;
    default:
        return false;
    }
}

// Serializes the stored value through the Rust serializer. An empty Optional means Rust declined
// (Shorthand/Transformation/ValueList serialization is still property-aware C++) and the caller
// must take the wrapper path.
static Optional<Utf16String> serialize_style_value_handle(RustStyleValueHandle const& handle, SerializationMode mode)
{
    auto text = StyleValueFFI::rust_style_value_serialize(handle.data(), to_underlying(mode));
    if (!text.has_value)
        return {};
    return Utf16String::adopt_raw(text.raw);
}

Optional<Utf16String> CSSStyleProperties::serialized_computed_value_from_stored_handle(PropertyID property_id) const
{
    // Fast path for getComputedStyle() serialization: when the computed value already lives in the
    // style record as a Rust style value, serialize it straight from the stored handle instead of
    // reconstructing a style value wrapper for it. Every property that needs used-value or color
    // resolution stays on the value-building path; an empty Optional means "not handled here".
    VERIFY(is_computed());
    if (!property_computed_value_may_be_stored_as_style_value_handle(property_id))
        return {};
    if (!owner_node().has_value())
        return {};
    auto abstract_element = *owner_node();

    auto maybe_layout_node = prepare_computed_style_and_layout_for_property(abstract_element, property_id);
    if (!maybe_layout_node.has_value())
        return {};
    auto* layout_node = *maybe_layout_node;

    auto style = abstract_element.computed_style();
    if (!style)
        return {};

    // letter-spacing: a used value of zero resolves to `normal`; leave that to the value path.
    // https://drafts.csswg.org/css-text-4/#letter-spacing-property
    if (property_id == PropertyID::LetterSpacing && layout_node && layout_node->letter_spacing() == 0)
        return {};

    // grid-template-columns/rows: with a laid-out grid box, the resolved value reflects the used
    // track sizes, so only serialize the computed value when no used track list exists.
    // https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
    if (property_id == PropertyID::GridTemplateColumns || property_id == PropertyID::GridTemplateRows) {
        if (layout_node && Painting::has_committed_box(*layout_node)) {
            auto const& used_values = property_id == PropertyID::GridTemplateColumns
                ? Painting::used_values_for_grid_template_columns(*layout_node)
                : Painting::used_values_for_grid_template_rows(*layout_node);
            if (used_values.has_value())
                return {};
        }
    }

    auto const* handle = style->stored_style_value_handle(property_id);
    if (!handle)
        return {};
    return serialize_style_value_handle(*handle, SerializationMode::ResolvedValue);
}

static RefPtr<StyleValue const> resolve_color_style_value(StyleValue const& style_value, Color computed_color, ColorResolutionContext const* color_resolution_context)
{
    if (color_resolution_context && style_value.is_color_function()) {
        auto const& color_function = as<ColorFunctionStyleValue>(style_value);
        if (color_function.origin_color() && color_function.color_type().has_value()) {
            auto resolved = color_function.resolve_relative_form(*color_resolution_context);
            if (!resolved)
                return style_value;

            return as<ColorFunctionStyleValue>(*resolved).computed_value_form();
        }
    }

    if (style_value.is_color_function() && as<ColorFunctionStyleValue>(style_value).serializes_as_color_function())
        return style_value;
    if (style_value.is_color()) {
        auto& color_style_value = static_cast<ColorStyleValue const&>(style_value);
        if (auto color_type = color_style_value.color_type();
            color_type.has_value() && first_is_one_of(*color_type, ColorStyleValue::ColorType::Lab, ColorStyleValue::ColorType::OKLab, ColorStyleValue::ColorType::LCH, ColorStyleValue::ColorType::OKLCH))
            return style_value;
    }

    return ColorStyleValue::create_from_color(computed_color, ColorSyntax::Modern);
}

RefPtr<StyleValue const> CSSStyleProperties::style_value_for_computed_property(Layout::NodeWithStyle const& layout_node, PropertyID property_id) const
{
    if (!owner_node().has_value()) {
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for CSSStyleProperties without owner node was requested");
        return nullptr;
    }

    auto used_value_for_property = [&layout_node](Function<CSSPixels(Layout::Node const&)>&& used_value_getter) -> Optional<CSSPixels> {
        auto display = layout_node.display();
        if (!display.is_none() && !display.is_contents() && Painting::has_committed_box(layout_node))
            return used_value_getter(layout_node);
        return {};
    };

    auto used_size_for_property = [&layout_node, &used_value_for_property]<typename ContentBoxGetter, typename BorderBoxGetter>(ContentBoxGetter content_box_getter, BorderBoxGetter border_box_getter) -> Optional<CSSPixels> {
        return used_value_for_property([&layout_node, content_box_getter, border_box_getter](Layout::Node const& box_layout_node) {
            if (layout_node.box_sizing() == BoxSizing::BorderBox)
                return border_box_getter(box_layout_node);
            return content_box_getter(box_layout_node);
        });
    };

    auto& element = owner_node()->element();
    auto pseudo_element = owner_node()->pseudo_element();

    auto used_value_for_inset = [&layout_node, used_value_for_property](LengthPercentageOrAuto const& start_side, LengthPercentageOrAuto const& end_side, Function<CSSPixels(Layout::Node const&)>&& used_value_getter) -> Optional<CSSPixels> {
        if (!layout_node.is_positioned())
            return {};

        // FIXME: Support getting the used value when position is sticky.
        if (layout_node.is_sticky_position())
            return {};

        if (!start_side.is_percentage() && !start_side.is_calculated() && !start_side.is_auto() && !end_side.is_auto())
            return {};

        return used_value_for_property(move(used_value_getter));
    };

    auto get_computed_value = [&element, pseudo_element](PropertyID property_id) -> NonnullRefPtr<StyleValue const> {
        auto style = element.computed_style(pseudo_element);
        VERIFY(style);
        return style->computed_style_value(property_id).release_nonnull();
    };
    auto color_resolution_context = ColorResolutionContext::for_layout_node_with_style(layout_node);

    if (property_is_logical_alias(property_id)) {
        return style_value_for_computed_property(
            layout_node,
            map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { layout_node.writing_mode(), layout_node.direction() }));
    }

    // A limited number of properties have special rules for producing their "resolved value".
    // We also have to manually construct shorthands from their longhands here.
    // Everything else uses the computed value.
    // https://drafts.csswg.org/cssom/#resolved-values

    // AD-HOC: We don't resolve logical properties here as we have already handled above
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
    case PropertyID::BackgroundColor: {
        auto const* background_values = element.style_group<ComputedValues::BackgroundValues>(pseudo_element);
        VERIFY(background_values);
        auto const& handle = background_values->background_color_style_value;
        VERIFY(handle.pointer);
        auto style_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
            static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer)));
        return resolve_color_style_value(
            *style_value,
            background_values->background_color_value(),
            &color_resolution_context);
    }
    case PropertyID::BorderBottomColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.border_bottom().color, &color_resolution_context);
    case PropertyID::BorderLeftColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.border_left().color, &color_resolution_context);
    case PropertyID::BorderRightColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.border_right().color, &color_resolution_context);
    case PropertyID::BorderTopColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.border_top().color, &color_resolution_context);
    case PropertyID::BoxShadow:
        return style_value_for_shadow(ShadowStyleValue::ShadowType::Normal, layout_node.box_shadow());
    case PropertyID::CaretColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.caret_color(), &color_resolution_context);
    case PropertyID::Color: {
        auto style = element.computed_style(pseudo_element);
        VERIFY(style);
        auto current_color_resolution_context = ColorResolutionContext::for_element(*owner_node());
        return resolve_color_style_value(*get_computed_value(property_id), style->color(), &current_color_resolution_context);
    }
    case PropertyID::OutlineColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.outline_color(), &color_resolution_context);
    case PropertyID::TextDecorationColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.text_decoration_color(), &color_resolution_context);
    case PropertyID::TextShadow:
        return style_value_for_shadow(ShadowStyleValue::ShadowType::Text, layout_node.text_shadow());
    case PropertyID::BackdropFilter:
    case PropertyID::Filter:
        return resolve_filter_style_value(*get_computed_value(property_id), layout_node.color());
    case PropertyID::TouchAction:
        return style_value_for_touch_action(layout_node.style_group<ComputedValues::MiscResetValues>().touch_action_value());

        // -> line-height
        //    The resolved value is normal if the computed value is normal, or the used value otherwise.
    case PropertyID::LineHeight: {
        auto line_height = get_computed_value(property_id);
        if (line_height->is_keyword() && line_height->to_keyword() == Keyword::Normal)
            return line_height;
        auto const* font_values = element.style_group<ComputedValues::FontValues>(pseudo_element);
        VERIFY(font_values);
        return LengthStyleValue::create(Length::make_px(font_values->line_height_used));
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
    case PropertyID::Height: {
        auto maybe_used_height = used_size_for_property(
            [](auto const& box_layout_node) { return Painting::content_height(box_layout_node); },
            [](auto const& box_layout_node) { return Painting::absolute_border_box_rect(box_layout_node).height(); });
        if (maybe_used_height.has_value())
            return style_value_for_size(Size::make_px(maybe_used_height.release_value()));
        return style_value_for_size(layout_node.height());
    }
    case PropertyID::MarginBottom:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).margin.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.margin().bottom());
    case PropertyID::MarginLeft:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).margin.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.margin().left());
    case PropertyID::MarginRight:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).margin.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.margin().right());
    case PropertyID::MarginTop:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).margin.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.margin().top());
    case PropertyID::PaddingBottom:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).padding.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.padding().bottom());
    case PropertyID::PaddingLeft:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).padding.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.padding().left());
    case PropertyID::PaddingRight:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).padding.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.padding().right());
    case PropertyID::PaddingTop:
        if (auto maybe_used_value = used_value_for_property([](auto const& box_layout_node) { return Painting::box_model(box_layout_node).padding.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(layout_node.padding().top());
    case PropertyID::Width: {
        auto maybe_used_width = used_size_for_property(
            [](auto const& box_layout_node) { return Painting::content_width(box_layout_node); },
            [](auto const& box_layout_node) { return Painting::absolute_border_box_rect(box_layout_node).width(); });
        if (maybe_used_width.has_value())
            return style_value_for_size(Size::make_px(maybe_used_width.release_value()));
        return style_value_for_size(layout_node.width());
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
        auto inset = layout_node.inset();
        if (auto maybe_used_value = used_value_for_inset(inset.bottom(), inset.top(), [](auto const& box_layout_node) { return Painting::box_model(box_layout_node).inset.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));

        return style_value_for_length_percentage_or_auto(inset.bottom());
    }
    case PropertyID::Left: {
        auto inset = layout_node.inset();
        if (auto maybe_used_value = used_value_for_inset(inset.left(), inset.right(), [](auto const& box_layout_node) { return Painting::box_model(box_layout_node).inset.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage_or_auto(inset.left());
    }
    case PropertyID::Right: {
        auto inset = layout_node.inset();
        if (auto maybe_used_value = used_value_for_inset(inset.right(), inset.left(), [](auto const& box_layout_node) { return Painting::box_model(box_layout_node).inset.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));

        return style_value_for_length_percentage_or_auto(inset.right());
    }
    case PropertyID::Top: {
        auto inset = layout_node.inset();
        if (auto maybe_used_value = used_value_for_inset(inset.top(), inset.bottom(), [](auto const& box_layout_node) { return Painting::box_model(box_layout_node).inset.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));

        return style_value_for_length_percentage_or_auto(inset.top());
    }

        // -> A resolved value special case property defined in another specification
        //    As defined in the relevant specification.
    case PropertyID::Transform: {
        if (!layout_node.has_transformations())
            return KeywordStyleValue::create(Keyword::None);

        // https://drafts.csswg.org/css-transforms-2/#serialization-of-the-computed-value
        // The transform property is a resolved value special case property. [CSSOM]
        // When the computed value is a <transform-list>, the resolved value is one <matrix()> function or one <matrix3d()> function computed by the following algorithm:
        // 1. Let transform be a 4x4 matrix initialized to the identity matrix.
        //    The elements m11, m22, m33 and m44 of transform must be set to 1; all other elements of transform must be set to 0.
        auto transform = FloatMatrix4x4::identity();

        // 2. Post-multiply all <transform-function>s in <transform-list> to transform.
        VERIFY(Painting::has_committed_box(layout_node));
        layout_node.for_each_transformation([&](auto const& transformation) {
            transform = transform * transformation.to_matrix(&layout_node);
        });

        // https://drafts.csswg.org/css-transforms-1/#2d-matrix
        auto is_2d_matrix = [](Gfx::FloatMatrix4x4 const& matrix) -> bool {
            // A 3x2 transformation matrix,
            // or a 4x4 matrix where the items m31, m32, m13, m23, m43, m14, m24, m34 are equal to 0
            // and m33, m44 are equal to 1.
            // NB: We only care about 4x4 matrices here.
            // NB: Our elements are 0-indexed not 1-indexed, and in the opposite order.
            // NB: We use epsilon comparisons here to account for inaccuracies when doing trigonometric calculations.
            if (abs(matrix[0, 2]) > AK::NumericLimits<float>::epsilon()     // m31
                || abs(matrix[1, 2]) > AK::NumericLimits<float>::epsilon()  // m32
                || abs(matrix[2, 0]) > AK::NumericLimits<float>::epsilon()  // m13
                || abs(matrix[2, 1]) > AK::NumericLimits<float>::epsilon()  // m23
                || abs(matrix[2, 3]) > AK::NumericLimits<float>::epsilon()  // m43
                || abs(matrix[3, 0]) > AK::NumericLimits<float>::epsilon()  // m14
                || abs(matrix[3, 1]) > AK::NumericLimits<float>::epsilon()  // m24
                || abs(matrix[3, 2]) > AK::NumericLimits<float>::epsilon()) // m34
                return false;

            if (abs(matrix[2, 2]) - 1 > AK::NumericLimits<float>::epsilon()     // m33
                || abs(matrix[3, 3]) - 1 > AK::NumericLimits<float>::epsilon()) // m44
                return false;

            return true;
        };

        // 3. Chose between <matrix()> or <matrix3d()> serialization:
        // -> If transform is a 2D matrix
        //        Serialize transform to a <matrix()> function.
        if (is_2d_matrix(transform)) {
            StyleValueVector parameters {
                NumberStyleValue::create(transform[0, 0]),
                NumberStyleValue::create(transform[1, 0]),
                NumberStyleValue::create(transform[0, 1]),
                NumberStyleValue::create(transform[1, 1]),
                NumberStyleValue::create(transform[0, 3]),
                NumberStyleValue::create(transform[1, 3]),
            };
            return TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix, move(parameters));
        }
        // -> Otherwise
        //        Serialize transform to a <matrix3d()> function.
        else {
            StyleValueVector parameters {
                NumberStyleValue::create(transform[0, 0]),
                NumberStyleValue::create(transform[1, 0]),
                NumberStyleValue::create(transform[2, 0]),
                NumberStyleValue::create(transform[3, 0]),
                NumberStyleValue::create(transform[0, 1]),
                NumberStyleValue::create(transform[1, 1]),
                NumberStyleValue::create(transform[2, 1]),
                NumberStyleValue::create(transform[3, 1]),
                NumberStyleValue::create(transform[0, 2]),
                NumberStyleValue::create(transform[1, 2]),
                NumberStyleValue::create(transform[2, 2]),
                NumberStyleValue::create(transform[3, 2]),
                NumberStyleValue::create(transform[0, 3]),
                NumberStyleValue::create(transform[1, 3]),
                NumberStyleValue::create(transform[2, 3]),
                NumberStyleValue::create(transform[3, 3]),
            };
            return TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix3d, move(parameters));
        }
    }
    case PropertyID::TransformOrigin: {
        // https://drafts.csswg.org/css-transforms/#transform-origin-property
        // The transform-origin property is a resolved value special case property like height. [CSSOM]
        Optional<CSSPixelRect> reference_box;
        if (auto display = layout_node.display(); !display.is_none() && !display.is_contents()) {
            if (Painting::has_committed_box(layout_node))
                reference_box = Painting::transform_reference_box(layout_node);
        }
        return style_value_for_transform_origin(layout_node.transform_origin(), reference_box);
    }
    case PropertyID::AnimationDuration: {
        // https://drafts.csswg.org/css-animations-2/#animation-duration
        // For backwards-compatibility with Level 1, when the computed value of animation-timeline is auto (i.e. only
        // one list value, and that value being auto), the resolved value of auto for animation-duration is 0s whenever
        // its used value would also be 0s.
        auto animation_timeline_computed_value = get_computed_value(PropertyID::AnimationTimeline);
        auto animation_duration_computed_value = get_computed_value(PropertyID::AnimationDuration);

        if (animation_timeline_computed_value->as_value_list().size() == 1 && animation_timeline_computed_value->as_value_list().values()[0]->to_keyword() == Keyword::Auto) {
            StyleValueVector resolved_durations;

            for (auto const& duration : animation_duration_computed_value->as_value_list().values()) {
                if (duration->to_keyword() == Keyword::Auto) {
                    resolved_durations.append(TimeStyleValue::create(Time::make_seconds(0)));
                } else {
                    resolved_durations.append(duration);
                }
            }

            return StyleValueList::create(move(resolved_durations), StyleValueList::Separator::Comma);
        }

        return animation_duration_computed_value;
    }
        // If the border-style corresponding to a given border-width is none or hidden, then the used width is 0.
        // https://drafts.csswg.org/css-backgrounds/#border-width
        // NB: We do this adjustment when assigning to ComputedValues, so read from there.
    case PropertyID::BorderBottomWidth:
        return style_value_for_size(Size::make_px(layout_node.border_bottom().width));
    case PropertyID::BorderLeftWidth:
        return style_value_for_size(Size::make_px(layout_node.border_left().width));
    case PropertyID::BorderRightWidth:
        return style_value_for_size(Size::make_px(layout_node.border_right().width));
    case PropertyID::BorderTopWidth:
        return style_value_for_size(Size::make_px(layout_node.border_top().width));

        // -> Any other property
        //    The resolved value is the computed value.
    case PropertyID::WebkitTextFillColor:
        return resolve_color_style_value(*get_computed_value(property_id), layout_node.webkit_text_fill_color(), &color_resolution_context);
    case PropertyID::LetterSpacing: {
        // https://drafts.csswg.org/css-text-4/#letter-spacing-property
        // For legacy reasons, a computed letter-spacing of zero yields a resolved value (getComputedStyle() return value) of normal.
        if (layout_node.letter_spacing() == 0)
            return KeywordStyleValue::create(Keyword::Normal);
        return get_computed_value(property_id);
    }
    case PropertyID::Custom:
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for custom properties was requested (?)");
        return nullptr;
    default:
        // For grid-template-columns and grid-template-rows the resolved value is the used value.
        // https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
        if (property_id == PropertyID::GridTemplateColumns) {
            if (Painting::has_committed_box(layout_node)) {
                if (auto const& used_values = Painting::used_values_for_grid_template_columns(layout_node); used_values.has_value())
                    return style_value_for_used_grid_track_list(*used_values);
            }
        } else if (property_id == PropertyID::GridTemplateRows) {
            if (Painting::has_committed_box(layout_node)) {
                if (auto const& used_values = Painting::used_values_for_grid_template_rows(layout_node); used_values.has_value())
                    return style_value_for_used_grid_track_list(*used_values);
            }
        }

        if (!property_is_shorthand(property_id)) {
            return get_computed_value(property_id);
        }

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
WebIDL::ExceptionOr<Utf16String> CSSStyleProperties::remove_property(Utf16FlyString const& property_name)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create("Cannot remove property: CSSStyleProperties is read-only."_utf16);

    return remove_property_internal(PropertyNameAndID::from_name(property_name));
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<Utf16String> CSSStyleProperties::remove_property_internal(Optional<PropertyNameAndID> const& property)
{
    VERIFY(!is_readonly());

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // NB: Already done by creating a PropertyNameAndID.

    // NB: The spec doesn't reject invalid property names, it just lets them pass through.
    //     Attempting to remove a non-existent property is a no-op, so we can just skip over this section.
    Utf16String value;
    if (property.has_value()) {
        // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
        // FIXME: Add an overload that takes PropertyNameAndID?
        value = get_property_value(property->name());

        Function<bool(PropertyNameAndID const&)> remove_declaration = [&](PropertyNameAndID const& property_to_remove) {
            // 4. Let removed be false.
            bool removed = false;

            // 5. If property is a shorthand property, for each longhand property longhand that property maps to:
            if (property_is_shorthand(property_to_remove.id())) {
                for (auto longhand_property_id : longhands_for_shorthand(property_to_remove.id())) {
                    // 1. If longhand is not a property name of a CSS declaration in the declarations, continue.
                    // 2. Remove that CSS declaration and let removed be true.
                    removed |= remove_declaration(PropertyNameAndID::from_id(longhand_property_id));
                }
            } else {
                // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the declarations, remove that CSS declaration and let removed be true.
                if (property_to_remove.is_custom_property()) {
                    removed = m_custom_properties.remove(property_to_remove.name());
                } else {
                    removed = m_properties.remove_first_matching([&](auto& entry) { return entry.property_id == property_to_remove.id(); });
                }
            }

            return removed;
        };

        prepare_to_update_style_attribute();
        auto removed = remove_declaration(property.value());

        // 7. If removed is true, Update style attribute for the CSS declaration block.
        if (removed) {
            ++m_revision;
            update_style_attribute();

            // Non-standard: Invalidate style for the owners of our containing sheet, if any.
            invalidate_owners();
        }
    }

    // 8. Return value.
    return value;
}

WebIDL::ExceptionOr<Utf16String> CSSStyleProperties::remove_property(PropertyID property_name)
{
    return remove_property_internal(PropertyNameAndID::from_id(property_name));
}

// https://drafts.csswg.org/cssom/#dom-cssstyleproperties-cssfloat
Utf16String CSSStyleProperties::css_float() const
{
    // The cssFloat attribute, on getting, must return the result of invoking getPropertyValue() with float as argument.
    return get_property_value("float"_utf16_fly_string);
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_css_float(Utf16View value)
{
    // On setting, the attribute must invoke setProperty() with float as first argument, as second argument the given value,
    // and no third argument. Any exceptions thrown must be re-thrown.
    return set_property(PropertyID::Float, value, u""sv);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-declaration-block
Utf16String CSSStyleProperties::serialized() const
{
    // 1. Let list be an empty array.
    Vector<Utf16String> list;

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
    for (auto const& declaration : m_custom_properties) {
        // 1. Let property be declaration’s property name.
        auto const& property = declaration.key;

        // 2. If property is in already serialized, continue with the steps labeled declaration loop.
        // NB: It is never in already serialized, as there are no shorthands for custom properties.

        // 3. If property maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in preferred order.
        // NB: There are no shorthands for custom properties.

        // 4. Shorthand loop: For each shorthand in shorthands, follow these substeps: ...
        // NB: There are no shorthands for custom properties.

        // 5. Let value be the result of invoking serialize a CSS value of declaration.
        auto value = declaration.value.value->to_utf16_string(Web::CSS::SerializationMode::Normal);

        // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value,
        //    and the important flag set if declaration has its important flag set.
        auto serialized_declaration = serialize_a_css_declaration_to_utf16(property, value, declaration.value.important);

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

                // 6. If there is any declaration in declaration block in between the first and the last longhand
                //    in current longhands which belongs to the same logical property group, but has a different
                //    mapping logic as any of the longhands in current longhands, and is not in current
                //    longhands, continue with the steps labeled shorthand loop.
                auto first_current_longhand_index = m_properties.find_first_index_if([&](StyleProperty const& current_declaration) { return current_declaration.property_id == current_longhands[0].property_id; });
                auto last_current_longhand_index = m_properties.find_first_index_if([&](StyleProperty const& current_declaration) { return current_declaration.property_id == current_longhands[current_longhands.size() - 1].property_id; });

                VERIFY(first_current_longhand_index.has_value());
                VERIFY(last_current_longhand_index.has_value());

                bool should_continue = false;

                for (auto current_declaration_index = first_current_longhand_index.value(); current_declaration_index <= last_current_longhand_index.value(); ++current_declaration_index) {
                    // NB: Declaration is in current longhands
                    if (any_of(current_longhands, [&](auto const& current_longhand) { return current_longhand.property_id == m_properties[current_declaration_index].property_id; }))
                        continue;

                    auto logical_property_group_for_current_declaration = logical_property_group_for_property(m_properties[current_declaration_index].property_id);

                    if (!logical_property_group_for_current_declaration.has_value())
                        continue;

                    auto current_declaration_is_logical_alias = property_is_logical_alias(m_properties[current_declaration_index].property_id);

                    // NB: Declaration has any counterpart in current longhands with same logical property group but different mapping logic
                    if (any_of(current_longhands, [&](auto const& current_longhand) { return logical_property_group_for_property(current_longhand.property_id) == logical_property_group_for_current_declaration && property_is_logical_alias(current_longhand.property_id) != current_declaration_is_logical_alias; })) {
                        should_continue = true;
                        break;
                    }
                }

                if (should_continue)
                    continue;

                // 7. Let value be the result of invoking serialize a CSS value with current longhands.
                auto value = serialize_a_css_value_to_utf16(current_longhands);

                // 8. If value is the empty string, continue with the steps labeled shorthand loop.
                if (value.is_empty())
                    continue;

                // 9. Let serialized declaration be the result of invoking serialize a CSS declaration with property
                //    name shorthand, value value, and the important flag set if the CSS declarations in current
                //    longhands have their important flag set.
                auto serialized_declaration = serialize_a_css_declaration_to_utf16(string_from_property_id(shorthand), value, current_longhands.first().important);

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
            auto value = serialize_a_css_value_to_utf16(declaration);

            // 6. Let serialized declaration be the result of invoking serialize a CSS declaration with property name property, value value,
            //    and the important flag set if declaration has its important flag set.
            auto serialized_declaration = serialize_a_css_declaration_to_utf16(string_from_property_id(property), value, declaration.important);

            // 7. Append serialized declaration to list.
            list.append(move(serialized_declaration));

            // 8. Append property to already serialized.
            append_property_to_already_serialized(declaration.property_id);
        }
    }

    // 4. Return list joined with " " (U+0020).
    Utf16StringBuilder builder;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i != 0)
            builder.append_ascii(' ');
        builder.append(list[i]);
    }
    return builder.to_string();
}

// https://www.w3.org/TR/cssom/#serialize-a-css-value
Utf16String CSSStyleProperties::serialize_a_css_value_to_utf16(StyleProperty const& declaration) const
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
    return declaration.value->to_utf16_string(Web::CSS::SerializationMode::Normal);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-value
Utf16String CSSStyleProperties::serialize_a_css_value_to_utf16(Vector<StyleProperty> list) const
{
    if (list.is_empty())
        return {};

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
        return {};

    // 3. Otherwise, serialize a CSS value from a hypothetical declaration of the property shorthand with its value representing the combined values of the declarations in list.
    Function<ValueComparingNonnullRefPtr<ShorthandStyleValue const>(PropertyID)> make_shorthand_value = [&](PropertyID shorthand_id) {
        auto longhand_ids = longhands_for_shorthand(shorthand_id);
        Vector<ValueComparingNonnullRefPtr<StyleValue const>> longhand_values;

        for (auto longhand_id : longhand_ids) {
            if (property_is_shorthand(longhand_id))
                longhand_values.append(make_shorthand_value(longhand_id));
            else
                longhand_values.append(list.first_matching([&](auto declaration) { return declaration.property_id == longhand_id; })->value);
        }

        return ShorthandStyleValue::create(shorthand_id, longhand_ids, longhand_values);
    };

    return make_shorthand_value(shorthand.value())->to_utf16_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> CSSStyleProperties::set_css_text(Utf16View css_text)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly()) {
        return WebIDL::NoModificationAllowedError::create("Cannot modify properties: CSSStyleProperties is read-only."_utf16);
    }

    prepare_to_update_style_attribute();

    // 2. Empty the declarations.
    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the declarations, in specified order.
    set_declarations_from_text(css_text);

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    // Non-standard: Invalidate style for the owners of our containing sheet, if any.
    invalidate_owners();

    return {};
}

void CSSStyleProperties::invalidate_owners()
{
    if (auto rule = parent_rule()) {
        if (auto sheet = rule->parent_style_sheet()) {
            record_style_rule_declarations_changed(*rule);

            // What a style rule's declarations say is published for the rule itself, so the scope's rule cache is
            // still good: the rules it holds have not moved. Any other rule kind can change what the cascade builds
            // from, and the sheet has to say so.
            if (rule->type() == CSSRule::Type::Style || rule->type() == CSSRule::Type::NestedDeclarations)
                return;

            sheet->invalidate_owners();
            if (rule->type() == CSSRule::Type::FontFace)
                sheet->synchronize_fonts_after_rule_change();
        }
    }
}

// https://drafts.csswg.org/cssom/#set-a-css-declaration
bool CSSStyleProperties::set_a_css_declaration(PropertyID property_id, NonnullRefPtr<StyleValue const> value, Important important)
{
    VERIFY(!is_computed());

    // NOTE: The below algorithm is only suggested rather than required by the spec
    // https://drafts.csswg.org/cssom/#example-a40690cb
    // 1. If property is a case-sensitive match for a property name of a CSS declaration in declarations, follow these substeps:
    auto maybe_target_index = m_properties.find_first_index_if([&](auto declaration) { return declaration.property_id == property_id; });

    if (maybe_target_index.has_value()) {
        // 1. Let target declaration be such CSS declaration.
        auto target_declaration = m_properties[maybe_target_index.value()];

        // 2. Let needs append be false.
        bool needs_append = false;

        auto logical_property_group_for_set_property = logical_property_group_for_property(property_id);

        // NOTE: If the property of the declaration being set has no logical property group then it's not possible for
        //       one of the later declarations to share that logical property group so we can skip checking.
        if (logical_property_group_for_set_property.has_value()) {
            auto set_property_is_logical_alias = property_is_logical_alias(property_id);

            // 3. For each declaration in declarations after target declaration:
            for (size_t i = maybe_target_index.value() + 1; i < m_properties.size(); ++i) {
                // 1. If declaration’s property name is not in the same logical property group as property, then continue.
                if (logical_property_group_for_property(m_properties[i].property_id) != logical_property_group_for_set_property)
                    continue;

                // 2. If declaration’ property name has the same mapping logic as property, then continue.
                if (property_is_logical_alias(m_properties[i].property_id) == set_property_is_logical_alias)
                    continue;

                // 3. Let needs append be true.
                needs_append = true;

                // 4. Break.
                break;
            }
        }

        // 4. If needs append is false, then:
        if (!needs_append) {
            // 1. Let needs update be false.
            bool needs_update = false;

            // 2. If target declaration’s value is not equal to component value list, then let needs update be true.
            if (*target_declaration.value != *value)
                needs_update = true;

            // 3. If target declaration’s important flag is not equal to whether important flag is set, then let needs update be true.
            if (target_declaration.important != important)
                needs_update = true;

            // 4. If needs update is false, then return false.
            if (!needs_update)
                return false;

            // 5. Set target declaration’s value to component value list.
            m_properties[maybe_target_index.value()].value = move(value);

            // 6. If important flag is set, then set target declaration’s important flag, otherwise unset it.
            m_properties[maybe_target_index.value()].important = important;

            // 7. Return true.
            return true;
        }

        // 5. Otherwise, remove target declaration from declarations.
        m_properties.remove(maybe_target_index.value());
    }

    // 2. Append a new CSS declaration with property name property, value component value list, and important flag set
    //    if important flag is set to declarations.
    m_properties.append(StyleProperty {
        .important = important,
        .property_id = property_id,
        .value = move(value),
    });

    // 3. Return true
    return true;
}

void CSSStyleProperties::empty_the_declarations()
{
    m_properties.clear();
    m_custom_properties.clear();
}

void CSSStyleProperties::set_the_declarations(Vector<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties)
{
    m_properties = convert_declarations_to_specified_order(properties);
    m_custom_properties = move(custom_properties);
}

void CSSStyleProperties::set_declarations_from_text(Utf16View css_text)
{
    empty_the_declarations();
    auto parsing_params = owner_node().has_value()
        ? Parser::ParsingParams(owner_node()->element().document())
        : Parser::ParsingParams();
    parsing_params.rule_context.append(Parser::RuleContext::Style);

    auto style = parse_css_property_declaration_block(parsing_params, css_text);
    set_the_declarations(style.properties, style.custom_properties);
    ++m_revision;
}

}
