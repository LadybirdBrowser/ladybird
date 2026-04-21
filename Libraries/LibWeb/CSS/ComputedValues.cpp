/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OverflowClipMarginStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

template<typename T>
static consteval ComputedValuesFFI::StyleGroupLifecycle style_group_lifecycle_of()
{
    return T::style_group_lifecycle;
}

template<typename T>
static consteval ComputedValuesFFI::StyleGroupVTable make_style_group_vtable()
{
    return {
        .lifecycle = style_group_lifecycle_of<T>(),
        .size = sizeof(T),
        .align = alignof(T),
    };
}

// Builds the keyword-code table for one enum-typed group field: keyword code
// to enum code, 255 for keywords the converter rejects. The generic Rust
// group builder maps values through these tables.
template<auto converter>
static Array<u8, number_of_keywords> const& keyword_code_table()
{
    static auto const table = [] {
        Array<u8, number_of_keywords> built;
        built.fill(255);
        for (size_t keyword = 0; keyword < number_of_keywords; ++keyword) {
            if (auto value = converter(static_cast<Keyword>(keyword)); value.has_value())
                built[keyword] = static_cast<u8>(to_underlying(*value));
        }
        return built;
    }();
    return table;
}

// The appearance keyword mapping with the compatibility keywords excluded:
// they normalize to auto for the appearance field but stay raw for
// computed_appearance, so their pages take the C++ population path.
static Optional<Appearance> appearance_without_compat_from_keyword(Keyword keyword)
{
    auto appearance = keyword_to_appearance(keyword);
    if (!appearance.has_value())
        return {};
    switch (*appearance) {
    case Appearance::Searchfield:
    case Appearance::Textarea:
    case Appearance::PushButton:
    case Appearance::SliderHorizontal:
    case Appearance::Checkbox:
    case Appearance::Radio:
    case Appearance::SquareButton:
    case Appearance::Menulist:
    case Appearance::Listbox:
    case Appearance::Meter:
    case Appearance::ProgressBar:
    case Appearance::Button:
        return {};
    default:
        return appearance;
    }
}

// The properties the core's bespoke grid group build consumes from the
// longhand table.
static constexpr Array grid_group_properties {
    PropertyID::GridAutoColumns,
    PropertyID::GridAutoRows,
    PropertyID::GridTemplateColumns,
    PropertyID::GridTemplateRows,
    PropertyID::GridColumnEnd,
    PropertyID::GridColumnStart,
    PropertyID::GridRowEnd,
    PropertyID::GridRowStart,
    PropertyID::GridTemplateAreas,
};

// The properties feeding the animation group's descriptors, in registration
// order. Every field registers as an initial-value constraint: elements
// without animations, timelines or transitions adopt a shared payload.
static constexpr Array animation_group_properties {
    PropertyID::AnimationName,
    PropertyID::AnimationComposition,
    PropertyID::AnimationDelay,
    PropertyID::AnimationDirection,
    PropertyID::AnimationDuration,
    PropertyID::AnimationFillMode,
    PropertyID::AnimationIterationCount,
    PropertyID::AnimationPlayState,
    PropertyID::AnimationTimeline,
    PropertyID::AnimationTimingFunction,
    PropertyID::ScrollTimelineName,
    PropertyID::ScrollTimelineAxis,
    PropertyID::TimelineScope,
    PropertyID::ViewTimelineName,
    PropertyID::ViewTimelineAxis,
    PropertyID::ViewTimelineInset,
    PropertyID::TransitionProperty,
    PropertyID::TransitionDuration,
    PropertyID::TransitionTimingFunction,
    PropertyID::TransitionDelay,
    PropertyID::TransitionBehavior,
};

// The style group each longhand's computed value lives in, filled as the field descriptors are
// registered: the descriptors are what Rust builds the group payloads from, so a binding derived
// from them cannot drift from where the value actually lives. Groups whose payloads build through
// bespoke calls or in C++ bind their properties explicitly beside the descriptors, transcribed from
// the builder or setters that consume them. A longhand without a binding has no single known group,
// and callers treat it conservatively.
static Array<Optional<StyleGroupIndex>, number_of_longhand_properties>& style_group_by_property()
{
    static Array<Optional<StyleGroupIndex>, number_of_longhand_properties> map;
    return map;
}

static void register_style_group_field_descriptors()
{
    using namespace ComputedValuesFFI;
    static_assert(sizeof(FlexDirection) == 1 && sizeof(FlexWrap) == 1 && sizeof(AlignContent) == 1
        && sizeof(AlignItems) == 1 && sizeof(AlignSelf) == 1 && sizeof(JustifyContent) == 1
        && sizeof(JustifyItems) == 1 && sizeof(JustifySelf) == 1);

    Vector<FfiGroupFieldDescriptor> descriptors;
    auto bind_property_to_group = [](PropertyID property, size_t group_index) {
        auto group = static_cast<StyleGroupIndex>(group_index);
        auto& binding = style_group_by_property()[to_underlying(property) - to_underlying(first_longhand_property_id)];
        VERIFY(!binding.has_value() || binding.value() == group);
        binding = group;
    };
    auto add = [&](size_t group_index, PropertyID property, u32 offset, u8 kind, u16 keyword, Array<u8, number_of_keywords> const* keyword_table, double required_px = 0) {
        bind_property_to_group(property, group_index);
        descriptors.append({
            .group_index = static_cast<u32>(group_index),
            .property_id = static_cast<u16>(to_underlying(property)),
            .offset = offset,
            .kind = kind,
            .keyword = keyword,
            .required_px = required_px,
            .keyword_table = keyword_table ? keyword_table->data() : nullptr,
            .keyword_table_length = keyword_table ? keyword_table->size() : 0,
        });
    };

    static_assert(sizeof(Color) == sizeof(u32));
    using TextReset = ComputedValues::TextResetValues;
    constexpr auto text_reset = to_underlying(StyleGroupIndex::TextResetValues);
    add(text_reset, PropertyID::TextDecorationLine, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(text_reset, PropertyID::TextDecorationThickness, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(text_reset, PropertyID::TextDecorationStyle, offsetof(TextReset, text_decoration_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_decoration_style>());
    add(text_reset, PropertyID::TextDecorationColor, offsetof(TextReset, text_decoration_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(text_reset, PropertyID::WhiteSpaceTrim, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);

    using Effects = ComputedValues::EffectsValues;
    constexpr auto effects = to_underlying(StyleGroupIndex::EffectsValues);
    add(effects, PropertyID::Opacity, offsetof(Effects, opacity), GROUP_FIELD_RESOLVED_F32, 0, nullptr);
    add(effects, PropertyID::Filter, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(effects, PropertyID::BackdropFilter, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(effects, PropertyID::MixBlendMode, offsetof(Effects, mix_blend_mode), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_mix_blend_mode>());
    add(effects, PropertyID::Isolation, offsetof(Effects, isolation), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_isolation>());
    add(effects, PropertyID::BoxShadow, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(effects, PropertyID::Clip, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);

    using MiscReset = ComputedValuesFFI::MiscResetValues;
    constexpr auto misc_reset = to_underlying(StyleGroupIndex::MiscResetValues);
    for (auto property : { PropertyID::ScrollMarginTop, PropertyID::ScrollMarginRight, PropertyID::ScrollMarginBottom, PropertyID::ScrollMarginLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    for (auto property : { PropertyID::ScrollPaddingTop, PropertyID::ScrollPaddingRight, PropertyID::ScrollPaddingBottom, PropertyID::ScrollPaddingLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    for (auto property : { PropertyID::OverflowClipMarginTop, PropertyID::OverflowClipMarginRight, PropertyID::OverflowClipMarginBottom, PropertyID::OverflowClipMarginLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ColumnSpan, offsetof(MiscReset, column_span), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_column_span>());
    add(misc_reset, PropertyID::BreakBefore, offsetof(MiscReset, break_before), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_break_between>());
    add(misc_reset, PropertyID::BreakAfter, offsetof(MiscReset, break_after), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_break_between>());
    add(misc_reset, PropertyID::BreakInside, offsetof(MiscReset, break_inside), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_break_inside>());
    add(misc_reset, PropertyID::ColumnRuleStyle, offsetof(MiscReset, column_rule_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_line_style>());
    add(misc_reset, PropertyID::BoxDecorationBreak, offsetof(MiscReset, box_decoration_break), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_box_decoration_break>());
    add(misc_reset, PropertyID::ColumnFill, offsetof(MiscReset, column_fill), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_column_fill>());
    // NB: The compatibility keywords normalize to auto for the appearance field but stay raw for
    //     computed_appearance, which the plain enum-keyword decode cannot express: the core's
    //     misc-reset build lowers both fields, and the descriptor only constrains the generic
    //     path to the non-compat pages.
    add(misc_reset, PropertyID::Appearance, offsetof(MiscReset, appearance), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<appearance_without_compat_from_keyword>());
    add(misc_reset, PropertyID::Appearance, offsetof(MiscReset, computed_appearance), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_appearance>());
    add(misc_reset, PropertyID::OutlineStyle, offsetof(MiscReset, outline_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_outline_style>());
    add(misc_reset, PropertyID::ObjectFit, offsetof(MiscReset, object_fit), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_object_fit>());
    add(misc_reset, PropertyID::ColumnHeight, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(misc_reset, PropertyID::OutlineColor, offsetof(MiscReset, outline_color), GROUP_FIELD_COLOR_OR_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(misc_reset, PropertyID::OutlineOffset, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    add(misc_reset, PropertyID::OutlineWidth, offsetof(MiscReset, outline_width), GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE, 0, nullptr);
    add(misc_reset, PropertyID::ColumnRuleColor, offsetof(MiscReset, column_rule_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(misc_reset, PropertyID::ColumnRuleWidth, offsetof(MiscReset, column_rule_width), GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE, 0, nullptr);
    add(misc_reset, PropertyID::UserSelect, offsetof(MiscReset, user_select), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_user_select>());
    add(misc_reset, PropertyID::ObjectPosition, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ViewTransitionName, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(misc_reset, PropertyID::TouchAction, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ScrollBehavior, offsetof(MiscReset, scroll_behavior), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_scroll_behavior>());
    add(misc_reset, PropertyID::ScrollSnapAlign, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ScrollSnapStop, offsetof(MiscReset, scroll_snap_stop), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_scroll_snap_stop>());
    add(misc_reset, PropertyID::ScrollSnapType, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ScrollbarGutter, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ScrollbarWidth, offsetof(MiscReset, scrollbar_width), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_scrollbar_width>());
    add(misc_reset, PropertyID::ShapeImageThreshold, offsetof(MiscReset, shape_image_threshold), GROUP_FIELD_RESOLVED_F64, 0, nullptr);
    add(misc_reset, PropertyID::ShapeMargin, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    add(misc_reset, PropertyID::ShapeOutside, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(misc_reset, PropertyID::WillChange, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);

    using InheritedText = ComputedValues::InheritedTextValues;
    constexpr auto inherited_text = to_underlying(StyleGroupIndex::InheritedTextValues);
    add(inherited_text, PropertyID::Color, offsetof(InheritedText, color), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_text, PropertyID::Color, offsetof(InheritedText, color_style_value), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
    add(inherited_text, PropertyID::WebkitTextFillColor, offsetof(InheritedText, webkit_text_fill_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_text, PropertyID::WebkitTextFillColor, offsetof(InheritedText, webkit_text_fill_color_is_current_color), GROUP_FIELD_KEYWORD_EQUALS_BOOL, to_underlying(Keyword::Currentcolor), nullptr);
    add(inherited_text, PropertyID::TextShadow, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(inherited_text, PropertyID::TextAlign, offsetof(InheritedText, text_align), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_align>());
    add(inherited_text, PropertyID::TextJustify, offsetof(InheritedText, text_justify), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_justify>());
    add(inherited_text, PropertyID::TextTransform, offsetof(InheritedText, text_transform), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_transform>());
    add(inherited_text, PropertyID::TextWrapMode, offsetof(InheritedText, text_wrap_mode), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_wrap_mode>());
    add(inherited_text, PropertyID::TextWrapStyle, offsetof(InheritedText, text_wrap_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_wrap_style>());
    add(inherited_text, PropertyID::TextDecorationSkipInk, offsetof(InheritedText, text_decoration_skip_ink), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_decoration_skip_ink>());
    add(inherited_text, PropertyID::TextUnderlinePosition, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_text, PropertyID::TextUnderlineOffset, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_text, PropertyID::TextIndent, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_text, PropertyID::TabSize, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_text, PropertyID::WhiteSpaceCollapse, offsetof(InheritedText, white_space_collapse), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_white_space_collapse>());
    add(inherited_text, PropertyID::WordBreak, offsetof(InheritedText, word_break), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_word_break>());
    add(inherited_text, PropertyID::OverflowWrap, offsetof(InheritedText, overflow_wrap), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_overflow_wrap>());
    add(inherited_text, PropertyID::BlockEllipsis, offsetof(InheritedText, block_ellipsis), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
    add(inherited_text, PropertyID::WordSpacing, offsetof(InheritedText, word_spacing), GROUP_FIELD_CSS_PIXELS, 0, nullptr);
    add(inherited_text, PropertyID::WordSpacing, offsetof(InheritedText, word_spacing_style_value), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
    add(inherited_text, PropertyID::LetterSpacing, offsetof(InheritedText, letter_spacing), GROUP_FIELD_CSS_PIXELS, 0, nullptr);
    add(inherited_text, PropertyID::LetterSpacing, offsetof(InheritedText, letter_spacing_style_value), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
    add(inherited_text, PropertyID::Orphans, offsetof(InheritedText, orphans), GROUP_FIELD_U64, 0, nullptr);
    add(inherited_text, PropertyID::Widows, offsetof(InheritedText, widows), GROUP_FIELD_U64, 0, nullptr);

    using InheritedUI = ComputedValues::InheritedUIValues;
    constexpr auto inherited_ui = to_underlying(StyleGroupIndex::InheritedUIValues);
    add(inherited_ui, PropertyID::CaretColor, offsetof(InheritedUI, caret_color) + offsetof(ComputedValuesFFI::ComputedColorOrAuto, used_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_ui, PropertyID::CaretColor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::AccentColor, offsetof(InheritedUI, accent_color) + offsetof(ComputedValuesFFI::ComputedColorOrAuto, used_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_ui, PropertyID::AccentColor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::Cursor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::PointerEvents, offsetof(InheritedUI, pointer_events), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_pointer_events>());
    add(inherited_ui, PropertyID::ScrollbarColor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::ColorScheme, offsetof(InheritedUI, color_scheme), GROUP_FIELD_RESOLVED_U8, 0, nullptr);
    add(inherited_ui, PropertyID::ColorScheme, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    using Transform = ComputedValues::TransformValues;
    constexpr auto transform = to_underlying(StyleGroupIndex::TransformValues);
    add(transform, PropertyID::Transform, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::TransformBox, offsetof(Transform, transform_box), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_transform_box>());
    add(transform, PropertyID::TransformOrigin, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(transform, PropertyID::TransformStyle, offsetof(Transform, transform_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_transform_style>());
    add(transform, PropertyID::BackfaceVisibility, offsetof(Transform, backface_visibility), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_backface_visibility>());
    add(transform, PropertyID::Rotate, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::Translate, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::Scale, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::Perspective, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::PerspectiveOrigin, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    constexpr auto mask = to_underlying(StyleGroupIndex::MaskValues);
    add(mask, PropertyID::MaskImage, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(mask, PropertyID::MaskType, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::ClipPath, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(mask, PropertyID::MaskMode, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskRepeat, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskPosition, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskClip, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskOrigin, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskSize, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskComposite, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    // The grid group builds through the core's bespoke track-list walk, so its
    // properties bind without descriptors.
    for (auto property : grid_group_properties)
        bind_property_to_group(property, to_underlying(StyleGroupIndex::GridValues));

    constexpr auto animation = to_underlying(StyleGroupIndex::AnimationValues);
    for (auto property : animation_group_properties)
        add(animation, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    using InheritedSVG = ComputedValues::InheritedSVGValues;
    constexpr auto inherited_svg = to_underlying(StyleGroupIndex::InheritedSVGValues);
    add(inherited_svg, PropertyID::Fill, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_svg, PropertyID::Stroke, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_svg, PropertyID::FillRule, offsetof(InheritedSVG, fill_rule), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_fill_rule>());
    add(inherited_svg, PropertyID::ClipRule, offsetof(InheritedSVG, clip_rule), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_fill_rule>());
    add(inherited_svg, PropertyID::FillOpacity, offsetof(InheritedSVG, fill_opacity), GROUP_FIELD_RESOLVED_F32, 0, nullptr);
    add(inherited_svg, PropertyID::StrokeOpacity, offsetof(InheritedSVG, stroke_opacity), GROUP_FIELD_RESOLVED_F32, 0, nullptr);
    add(inherited_svg, PropertyID::StrokeLinecap, offsetof(InheritedSVG, stroke_linecap), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_stroke_linecap>());
    add(inherited_svg, PropertyID::StrokeLinejoin, offsetof(InheritedSVG, stroke_linejoin), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_stroke_linejoin>());
    add(inherited_svg, PropertyID::StrokeDasharray, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(inherited_svg, PropertyID::StrokeDashoffset, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_svg, PropertyID::StrokeMiterlimit, offsetof(InheritedSVG, stroke_miterlimit), GROUP_FIELD_F64, 0, nullptr);
    add(inherited_svg, PropertyID::StrokeWidth, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_svg, PropertyID::ColorInterpolation, offsetof(InheritedSVG, color_interpolation), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_color_interpolation>());
    add(inherited_svg, PropertyID::ColorInterpolationFilters, offsetof(InheritedSVG, color_interpolation_filters), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_color_interpolation>());
    add(inherited_svg, PropertyID::PaintOrder, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Normal), nullptr);
    add(inherited_svg, PropertyID::TextAnchor, offsetof(InheritedSVG, text_anchor), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_anchor>());
    add(inherited_svg, PropertyID::DominantBaseline, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_svg, PropertyID::ShapeRendering, offsetof(InheritedSVG, shape_rendering), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_shape_rendering>());

    using InheritedList = ComputedValuesFFI::InheritedListValues;
    constexpr auto inherited_list = to_underlying(StyleGroupIndex::InheritedListValues);
    add(inherited_list, PropertyID::ListStyleType, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(inherited_list, PropertyID::ListStylePosition, offsetof(InheritedList, list_style_position), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_list_style_position>());
    add(inherited_list, PropertyID::ListStyleImage, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(inherited_list, PropertyID::Quotes, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);

    constexpr auto content = to_underlying(StyleGroupIndex::ContentValues);
    add(content, PropertyID::Content, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Normal), nullptr);
    add(content, PropertyID::CounterIncrement, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(content, PropertyID::CounterReset, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(content, PropertyID::CounterSet, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);

    constexpr auto anchor = to_underlying(StyleGroupIndex::AnchorValues);
    add(anchor, PropertyID::AnchorName, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(anchor, PropertyID::AnchorScope, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(anchor, PropertyID::PositionAnchor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(anchor, PropertyID::PositionArea, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(anchor, PropertyID::PositionTryFallbacks, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(anchor, PropertyID::PositionTryOrder, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Normal), nullptr);
    add(anchor, PropertyID::PositionVisibility, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Always), nullptr);

    using Border = ComputedValuesFFI::BorderValues;
    constexpr auto border = to_underlying(StyleGroupIndex::BorderValues);
    struct BorderSide {
        PropertyID color;
        PropertyID style;
        PropertyID width;
        u32 data_offset;
        u32 data_handle_offset;
        u32 computed_width_offset;
    };
    for (auto const& side : {
             BorderSide { PropertyID::BorderLeftColor, PropertyID::BorderLeftStyle, PropertyID::BorderLeftWidth, offsetof(Border, border_left), offsetof(Border, border_left_color_style_value), offsetof(Border, border_left_computed_width) },
             BorderSide { PropertyID::BorderTopColor, PropertyID::BorderTopStyle, PropertyID::BorderTopWidth, offsetof(Border, border_top), offsetof(Border, border_top_color_style_value), offsetof(Border, border_top_computed_width) },
             BorderSide { PropertyID::BorderRightColor, PropertyID::BorderRightStyle, PropertyID::BorderRightWidth, offsetof(Border, border_right), offsetof(Border, border_right_color_style_value), offsetof(Border, border_right_computed_width) },
             BorderSide { PropertyID::BorderBottomColor, PropertyID::BorderBottomStyle, PropertyID::BorderBottomWidth, offsetof(Border, border_bottom), offsetof(Border, border_bottom_color_style_value), offsetof(Border, border_bottom_computed_width) },
         }) {
        add(border, side.color, side.data_offset + offsetof(ComputedValuesFFI::ComputedBorderSide, color), GROUP_FIELD_COLOR, 0, nullptr);
        add(border, side.color, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
        // NB: A none border-style keeps the used width at the constructor's zero;
        //     styled borders are completed by the Rust group builder.
        add(border, side.style, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
        add(border, side.width, side.computed_width_offset, GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE, 0, nullptr);
    }
    for (auto property : { PropertyID::BorderBottomLeftRadius, PropertyID::BorderBottomRightRadius, PropertyID::BorderTopLeftRadius, PropertyID::BorderTopRightRadius })
        add(border, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(border, PropertyID::CornerBottomLeftShape, offsetof(Border, corner_bottom_left_shape), GROUP_FIELD_RESOLVED_F64, 0, nullptr);
    add(border, PropertyID::CornerBottomRightShape, offsetof(Border, corner_bottom_right_shape), GROUP_FIELD_RESOLVED_F64, 0, nullptr);
    add(border, PropertyID::CornerTopLeftShape, offsetof(Border, corner_top_left_shape), GROUP_FIELD_RESOLVED_F64, 0, nullptr);
    add(border, PropertyID::CornerTopRightShape, offsetof(Border, corner_top_right_shape), GROUP_FIELD_RESOLVED_F64, 0, nullptr);
    add(border, PropertyID::BorderImageSource, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(border, PropertyID::BorderImageOutset, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(border, PropertyID::BorderImageRepeat, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(border, PropertyID::BorderImageSlice, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(border, PropertyID::BorderImageWidth, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    constexpr auto background = to_underlying(StyleGroupIndex::BackgroundValues);
    add(background, PropertyID::BackgroundColor, 0, GROUP_FIELD_COLOR, 0, nullptr);
    add(background, PropertyID::BackgroundColor, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    for (auto property : { PropertyID::BackgroundImage, PropertyID::BackgroundClip, PropertyID::BackgroundAttachment,
             PropertyID::BackgroundOrigin, PropertyID::BackgroundPositionX, PropertyID::BackgroundPositionY,
             PropertyID::BackgroundRepeat, PropertyID::BackgroundSize, PropertyID::BackgroundBlendMode })
        add(background, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    // Groups whose payloads build through bespoke calls rather than the descriptors above; the
    // property lists mirror those calls' signatures in create_internal.
    for (auto property : { PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left,
             PropertyID::MarginTop, PropertyID::MarginRight, PropertyID::MarginBottom, PropertyID::MarginLeft,
             PropertyID::PaddingTop, PropertyID::PaddingRight, PropertyID::PaddingBottom, PropertyID::PaddingLeft })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::SurroundValues));
    for (auto property : { PropertyID::Width, PropertyID::MinWidth, PropertyID::MaxWidth,
             PropertyID::Height, PropertyID::MinHeight, PropertyID::MaxHeight })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::SizingValues));
    for (auto property : { PropertyID::WebkitBoxOrient, PropertyID::FlexDirection, PropertyID::FlexWrap, PropertyID::FlexBasis,
             PropertyID::FlexGrow, PropertyID::FlexShrink, PropertyID::Order,
             PropertyID::AlignContent, PropertyID::AlignItems, PropertyID::AlignSelf,
             PropertyID::JustifyContent, PropertyID::JustifyItems, PropertyID::JustifySelf,
             PropertyID::ColumnGap, PropertyID::RowGap })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::AlignmentValues));
    for (auto property : { PropertyID::Visibility, PropertyID::Direction, PropertyID::WritingMode,
             PropertyID::ContentVisibility, PropertyID::ImageRendering })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::InheritedBoxValues));
    for (auto property : { PropertyID::BorderCollapse, PropertyID::CaptionSide, PropertyID::EmptyCells, PropertyID::BorderSpacing })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::InheritedTableValues));
    for (auto property : { PropertyID::Cx, PropertyID::Cy, PropertyID::D, PropertyID::R, PropertyID::Rx, PropertyID::Ry,
             PropertyID::X, PropertyID::Y, PropertyID::StopColor, PropertyID::StopOpacity,
             PropertyID::FloodColor, PropertyID::FloodOpacity, PropertyID::VectorEffect })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::SVGResetValues));
    // The box group builds from a hand-written FFI struct rather than from descriptors, so its
    // bindings are transcribed from that struct's fields.
    for (auto property : { PropertyID::Display, PropertyID::Float, PropertyID::Clear, PropertyID::Position,
             PropertyID::OverflowX, PropertyID::OverflowY, PropertyID::BoxSizing, PropertyID::Resize,
             PropertyID::TextOverflow, PropertyID::UnicodeBidi, PropertyID::TableLayout, PropertyID::GridAutoFlow,
             PropertyID::ColumnWidth, PropertyID::ColumnCount, PropertyID::Continue, PropertyID::MaxLines,
             PropertyID::ZIndex, PropertyID::VerticalAlign, PropertyID::AspectRatio,
             PropertyID::ContainIntrinsicWidth, PropertyID::ContainIntrinsicHeight, PropertyID::Contain,
             PropertyID::ContainerType, PropertyID::ContainerName })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::BoxValues));
    // The font group builds in C++; only bindings verified against its setters are made.
    for (auto property : { PropertyID::FontSize, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::LineHeight })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::FontValues));

    rust_style_group_register_field_descriptors(descriptors.data(), descriptors.size());

    // Double-entry bookkeeping: Properties.json declares each longhand's style group, and the
    // bindings above derive it from what actually builds the groups. The two must agree exactly,
    // so a new property cannot claim a group nothing builds it into, and a binding cannot go
    // undeclared.
    auto style_group_name = [](StyleGroupIndex group) -> StringView {
        switch (group) {
#define LIBWEB_STYLE_GROUP_NAME(name, path, sharing_name, affects_layout) \
    case StyleGroupIndex::name:                                           \
        return #name##sv;
            LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_NAME)
#undef LIBWEB_STYLE_GROUP_NAME
        case StyleGroupIndex::Count:
            break;
        }
        VERIFY_NOT_REACHED();
    };
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        auto binding = style_group_by_property()[i - to_underlying(first_longhand_property_id)];
        auto declared = style_group_name_of_property(property_id);
        VERIFY(binding.has_value() == declared.has_value());
        if (binding.has_value())
            VERIFY(declared.value() == style_group_name(binding.value()));
    }

    Array<u32, number_of_longhand_properties> output_masks;
    Array<u32, number_of_longhand_properties> dependency_masks;
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto binding = style_group_by_property()[i - to_underlying(first_longhand_property_id)];
        auto& output_mask = output_masks[i - to_underlying(first_longhand_property_id)];
        auto& mask = dependency_masks[i - to_underlying(first_longhand_property_id)];
        output_mask = binding.has_value() ? 1u << to_underlying(binding.value()) : 0;
        mask = output_mask;
    }

    // Direction and writing-mode select the physical winners for every logical property group.
    // Those aliases own no payload, so derive the exact output-group closure through their
    // authoritative physical mappings.
    u32 logical_mapping_dependency_mask = 0;
    constexpr size_t writing_mode_count = 5;
    constexpr size_t direction_count = 2;
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        if (!property_is_logical_alias(property_id))
            continue;
        u32 logical_alias_dependency_mask = 0;
        for (size_t writing_mode = 0; writing_mode < writing_mode_count; ++writing_mode) {
            for (size_t direction = 0; direction < direction_count; ++direction) {
                auto physical_property = map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) });
                auto binding = style_group_by_property()[to_underlying(physical_property) - to_underlying(first_longhand_property_id)];
                VERIFY(binding.has_value());
                auto group = 1u << to_underlying(binding.value());
                logical_alias_dependency_mask |= group;
                logical_mapping_dependency_mask |= group;
            }
        }
        dependency_masks[i - to_underlying(first_longhand_property_id)] |= logical_alias_dependency_mask;
    }
    dependency_masks[to_underlying(PropertyID::Direction) - to_underlying(first_longhand_property_id)] |= logical_mapping_dependency_mask;
    dependency_masks[to_underlying(PropertyID::WritingMode) - to_underlying(first_longhand_property_id)] |= logical_mapping_dependency_mask;
    rust_style_group_register_property_dependency_masks(to_underlying(first_longhand_property_id), dependency_masks.data(), output_masks.data(), dependency_masks.size());

#ifdef LIBWEB_DUMP_STYLE_GROUP_BINDINGS
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        auto binding = style_group_by_property()[i - to_underlying(first_longhand_property_id)];
        dbgln("style-group-binding: {} {}", string_from_property_id(property_id), binding.has_value() ? to_underlying(binding.value()) : 999);
    }
#endif
}

Optional<StyleGroupIndex> ComputedValues::style_group_of_property(PropertyID property_id)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    // The bindings are filled with the descriptor registration, which the default payloads trigger.
    style_group_default_payload(0);
    return style_group_by_property()[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
}

// Groups whose layout is defined in Rust must not change size or alignment when the C++
// side layers initial values and accessors on top of the mirrored layout.
static_assert(sizeof(ComputedValues::InheritedBoxValues) == sizeof(ComputedValuesFFI::InheritedBoxValues));
static_assert(alignof(ComputedValues::InheritedBoxValues) == alignof(ComputedValuesFFI::InheritedBoxValues));
static_assert(sizeof(ComputedValues::InheritedTableValues) == sizeof(ComputedValuesFFI::InheritedTableValues));
static_assert(alignof(ComputedValues::InheritedTableValues) == alignof(ComputedValuesFFI::InheritedTableValues));
static_assert(sizeof(ComputedValues::SizingValues) == sizeof(ComputedValuesFFI::SizingValues));
static_assert(alignof(ComputedValues::SizingValues) == alignof(ComputedValuesFFI::SizingValues));
static_assert(sizeof(ComputedValues::AlignmentValues) == sizeof(ComputedValuesFFI::AlignmentValues));
static_assert(alignof(ComputedValues::AlignmentValues) == alignof(ComputedValuesFFI::AlignmentValues));
static_assert(sizeof(ComputedValues::SVGResetValues) == sizeof(ComputedValuesFFI::SVGResetValues));
static_assert(alignof(ComputedValues::SVGResetValues) == alignof(ComputedValuesFFI::SVGResetValues));
static_assert(sizeof(ComputedValues::SurroundValues) == sizeof(ComputedValuesFFI::SurroundValues));
static_assert(alignof(ComputedValues::SurroundValues) == alignof(ComputedValuesFFI::SurroundValues));
static_assert(sizeof(ComputedValues::BoxValues) == sizeof(ComputedValuesFFI::BoxValues));
static_assert(alignof(ComputedValues::BoxValues) == alignof(ComputedValuesFFI::BoxValues));
static_assert(sizeof(ComputedValues::TransformValues) == sizeof(ComputedValuesFFI::TransformValues));
static_assert(alignof(ComputedValues::TransformValues) == alignof(ComputedValuesFFI::TransformValues));
static_assert(sizeof(ComputedValues::EffectsValues) == sizeof(ComputedValuesFFI::EffectsValues));
static_assert(alignof(ComputedValues::EffectsValues) == alignof(ComputedValuesFFI::EffectsValues));
static_assert(sizeof(ComputedValues::AnchorValues) == sizeof(ComputedValuesFFI::AnchorValues));
static_assert(alignof(ComputedValues::AnchorValues) == alignof(ComputedValuesFFI::AnchorValues));
static_assert(sizeof(ComputedValues::InheritedUIValues) == sizeof(ComputedValuesFFI::InheritedUIValues));
static_assert(alignof(ComputedValues::InheritedUIValues) == alignof(ComputedValuesFFI::InheritedUIValues));
static_assert(sizeof(ComputedValues::InheritedSVGValues) == sizeof(ComputedValuesFFI::InheritedSVGValues));
static_assert(alignof(ComputedValues::InheritedSVGValues) == alignof(ComputedValuesFFI::InheritedSVGValues));
static_assert(to_underlying(FillRule::Nonzero) == 0);
static_assert(to_underlying(StrokeLinecap::Butt) == 0);
static_assert(to_underlying(StrokeLinejoin::Miter) == 0);
static_assert(to_underlying(ColorInterpolation::Auto) == 0);
static_assert(to_underlying(ColorInterpolation::Linearrgb) == 1);
static_assert(to_underlying(PaintOrder::Fill) == 0);
static_assert(to_underlying(PaintOrder::Stroke) == 1);
static_assert(to_underlying(PaintOrder::Markers) == 2);
static_assert(to_underlying(TextAnchor::Start) == 0);
static_assert(to_underlying(ShapeRendering::Auto) == 0);
static_assert(sizeof(Size) == sizeof(ComputedValuesFFI::ComputedSize));
static_assert(alignof(Size) == alignof(ComputedValuesFFI::ComputedSize));
static_assert(sizeof(RustStyleValueHandle) == sizeof(StyleValueFFI::StyleValueData const*));
static_assert(alignof(RustStyleValueHandle) == alignof(StyleValueFFI::StyleValueData const*));

// The Rust border group's four leading side facts share BorderData's layout.
static_assert(sizeof(Gfx::Color) == sizeof(u32));
static_assert(sizeof(ShadowData) == sizeof(ComputedValuesFFI::ComputedShadow));
static_assert(alignof(ShadowData) == alignof(ComputedValuesFFI::ComputedShadow));
static_assert(offsetof(ShadowData, offset_x) == offsetof(ComputedValuesFFI::ComputedShadow, offset_x));
static_assert(offsetof(ShadowData, offset_y) == offsetof(ComputedValuesFFI::ComputedShadow, offset_y));
static_assert(offsetof(ShadowData, blur_radius) == offsetof(ComputedValuesFFI::ComputedShadow, blur_radius));
static_assert(offsetof(ShadowData, spread_distance) == offsetof(ComputedValuesFFI::ComputedShadow, spread_distance));
static_assert(offsetof(ShadowData, color) == offsetof(ComputedValuesFFI::ComputedShadow, color));
static_assert(offsetof(ShadowData, color_syntax) == offsetof(ComputedValuesFFI::ComputedShadow, color_syntax));
static_assert(offsetof(ShadowData, placement) == offsetof(ComputedValuesFFI::ComputedShadow, placement));
static_assert(to_underlying(ColorSyntax::Legacy) == 0);
static_assert(to_underlying(ColorSyntax::Modern) == 1);
static_assert(to_underlying(ShadowPlacement::Outer) == 0);
static_assert(to_underlying(ShadowPlacement::Inner) == 1);
static_assert(to_underlying(MixBlendMode::Normal) == 0);
static_assert(to_underlying(Isolation::Auto) == 0);
static_assert(sizeof(LineStyle) == sizeof(u8));
static_assert(sizeof(BorderData) == sizeof(ComputedValuesFFI::ComputedBorderSide));
static_assert(offsetof(BorderData, color) == offsetof(ComputedValuesFFI::ComputedBorderSide, color));
static_assert(offsetof(BorderData, line_style) == offsetof(ComputedValuesFFI::ComputedBorderSide, line_style));
static_assert(offsetof(BorderData, width) == offsetof(ComputedValuesFFI::ComputedBorderSide, width));
static_assert(offsetof(ComputedValues::BorderValues, border_left) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_left));
static_assert(offsetof(ComputedValues::BorderValues, border_top) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_top));
static_assert(offsetof(ComputedValues::BorderValues, border_right) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_right));
static_assert(offsetof(ComputedValues::BorderValues, border_bottom) == offsetof(ComputedValuesFFI::BorderLayoutFacts, border_bottom));
static_assert(sizeof(ComputedValuesFFI::BorderLayoutFacts) <= offsetof(ComputedValues::BorderValues, border_left_color_style_value));
static_assert(sizeof(ComputedValues::BorderValues) == sizeof(ComputedValuesFFI::BorderValues));
static_assert(alignof(ComputedValues::BorderValues) == alignof(ComputedValuesFFI::BorderValues));
static_assert(sizeof(ComputedValues::ContentValues) == sizeof(ComputedValuesFFI::ContentValues));
static_assert(alignof(ComputedValues::ContentValues) == alignof(ComputedValuesFFI::ContentValues));
static_assert(sizeof(ComputedValues::InheritedListValues) == sizeof(ComputedValuesFFI::InheritedListValues));
static_assert(alignof(ComputedValues::InheritedListValues) == alignof(ComputedValuesFFI::InheritedListValues));
static_assert(sizeof(ComputedValues::MiscResetValues) == sizeof(ComputedValuesFFI::MiscResetValues));
static_assert(alignof(ComputedValues::MiscResetValues) == alignof(ComputedValuesFFI::MiscResetValues));
static_assert(to_underlying(ListStylePosition::Outside) == 1);

static_assert(sizeof(TextIndentData) == sizeof(ComputedValuesFFI::ComputedTextIndent));
static_assert(offsetof(TextIndentData, length_percentage) == offsetof(ComputedValuesFFI::ComputedTextIndent, length_percentage));
static_assert(offsetof(TextIndentData, each_line) == offsetof(ComputedValuesFFI::ComputedTextIndent, each_line));
static_assert(offsetof(TextIndentData, hanging) == offsetof(ComputedValuesFFI::ComputedTextIndent, hanging));
static_assert(sizeof(ComputedValues::InheritedTextValues) == sizeof(ComputedValuesFFI::InheritedTextValues));
static_assert(alignof(ComputedValues::InheritedTextValues) == alignof(ComputedValuesFFI::InheritedTextValues));
static_assert(sizeof(ComputedValues::AnimationValues) == sizeof(ComputedValuesFFI::AnimationValues));
static_assert(alignof(ComputedValues::AnimationValues) == alignof(ComputedValuesFFI::AnimationValues));

static_assert(sizeof(ComputedValues::FontValues) == sizeof(ComputedValuesFFI::FontValues));
static_assert(alignof(ComputedValues::FontValues) == alignof(ComputedValuesFFI::FontValues));
static_assert(offsetof(ComputedValues::FontValues, font_size) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_size));
static_assert(offsetof(ComputedValues::FontValues, line_height_used) == offsetof(ComputedValuesFFI::FontLayoutFacts, line_height_used));
static_assert(offsetof(ComputedValues::FontValues, font_variant_emoji) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_variant_emoji));
static_assert(offsetof(ComputedValues::FontValues, font_ascent) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_ascent));
static_assert(offsetof(ComputedValues::FontValues, font_descent) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_descent));
static_assert(offsetof(ComputedValues::FontValues, font_x_height) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_x_height));
static_assert(offsetof(ComputedValues::FontValues, first_available_font) == offsetof(ComputedValuesFFI::FontLayoutFacts, first_available_font));
static_assert(offsetof(ComputedValues::FontValues, font_cascade_list) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_cascade_list));
static_assert(sizeof(ComputedValuesFFI::FontLayoutFacts) <= offsetof(ComputedValues::FontValues, font_weight));
static_assert(to_underlying(FontVariantEmoji::Normal) == 0);
static_assert(to_underlying(FontVariantEmoji::Text) == 1);
static_assert(to_underlying(FontVariantEmoji::Emoji) == 2);
static_assert(to_underlying(FontVariantEmoji::Unicode) == 3);
static_assert(to_underlying(MathShift::Normal) == 0);
static_assert(to_underlying(MathShift::Compact) == 1);
static_assert(to_underlying(MathStyle::Normal) == 0);
static_assert(to_underlying(MathStyle::Compact) == 1);

void const* style_group_default_payload(size_t group_index)
{
    StyleComputer::ensure_style_metadata_tables_installed();
    static auto const default_payloads = [] {
        constexpr auto group_count = to_underlying(StyleGroupIndex::Count);
        Array<ComputedValuesFFI::StyleGroupVTable, group_count> vtables;
#define LIBWEB_STYLE_GROUP_VTABLE(name, path, sharing_name, affects_layout) \
    vtables[to_underlying(StyleGroupIndex::name)] = make_style_group_vtable<ComputedValues::name>();
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_VTABLE)
#undef LIBWEB_STYLE_GROUP_VTABLE
        Array<void const*, group_count> payloads {};
        ComputedValuesFFI::rust_style_group_registry_register(vtables.data(), vtables.size(), payloads.data());
        register_style_group_field_descriptors();
        return payloads;
    }();
    return default_payloads[group_index];
}

bool ComputedValues::property_inheritance_is_standard() const
{
    static auto const standard_inheritance_bitmap = [] {
        AK::FixedBitmap<number_of_longhand_properties> bitmap { false };
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            if (is_inherited_property(property_id))
                bitmap.set(property_bitmap_index(property_id), true);
        }
        return bitmap;
    }();
    return m_property_inherited == standard_inheritance_bitmap;
}

HashMap<PropertyID, NonnullRefPtr<StyleValue const>> ComputedValues::inheritance_dependent_specified_values_snapshot() const
{
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> values;
    for (auto const& entry : m_inheritance_dependent_specified_values) {
        auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(entry.value);
        values.set(
            static_cast<PropertyID>(entry.property),
            StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data)));
    }
    return values;
}

bool ComputedValues::inheritance_dependent_specified_values_equal(ComputedValues const& other) const
{
    if (m_inheritance_dependent_specified_values.size() != other.m_inheritance_dependent_specified_values.size())
        return false;
    for (auto const& entry : m_inheritance_dependent_specified_values) {
        auto other_entry = find_if(other.m_inheritance_dependent_specified_values.begin(), other.m_inheritance_dependent_specified_values.end(), [&](auto const& candidate) {
            return candidate.property == entry.property;
        });
        if (other_entry == other.m_inheritance_dependent_specified_values.end())
            return false;
        auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(entry.value);
        auto const* other_value = static_cast<StyleValueFFI::StyleValueData const*>(other_entry->value);
        if (value != other_value && !StyleValueFFI::rust_style_value_equals(value, other_value))
            return false;
    }
    return true;
}

bool ComputedValues::adopt_identical_group_payloads(ComputedValues const& previous) const
{
    bool all_shared = true;
    auto adopt = [&]<typename T>(StyleStructRef<T> const& mine, StyleStructRef<T> const& theirs) {
        if (mine.ptr_equals(theirs))
            return;
        if (mine == theirs) {
            // StyleEngine retains the previously published payload independently, so adopting an
            // equal canonical payload changes this projection without moving the shared record.
            const_cast<StyleStructRef<T>&>(mine) = theirs;
            return;
        }
        all_shared = false;
    };
#define LIBWEB_ADOPT_STYLE_GROUP(name, path, sharing_name, affects_layout) adopt(path, previous.path);
    LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_ADOPT_STYLE_GROUP)
#undef LIBWEB_ADOPT_STYLE_GROUP
    if (all_shared)
        adopt_identical_computed_longhand_table(previous);
    return all_shared;
}

// The same canonicalization for the computed longhand table: when this style's table names
// value-equal data throughout, take the previous style's table so the next publication interns
// the same pointers and keeps the style-record identity, exactly like adopted group payloads do.
void ComputedValues::adopt_identical_computed_longhand_table(ComputedValues const& previous) const
{
    if (m_is_style_record_view)
        return;
    auto const* table = static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table);
    auto const* previous_table = static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(previous.m_computed_longhand_table);
    if (!table || !previous_table || table == previous_table)
        return;
    if (ComputedValuesFFI::rust_computed_longhand_tables_equal_for_publication(table, previous_table))
        const_cast<ComputedValues&>(*this).copy_computed_longhand_table_from(previous);
}

bool ComputedValues::differs_in_any_layout_affecting_group_payload_from(ComputedValues const& other) const
{
    auto differs = []<typename T>(StyleStructRef<T> const& mine, StyleStructRef<T> const& theirs) {
        return !mine.ptr_equals(theirs) && !(mine == theirs);
    };
#define LIBWEB_COMPARE_STYLE_GROUP(name, path, sharing_name, affects_layout) \
    if constexpr (affects_layout) {                                          \
        if (differs(path, other.path))                                       \
            return true;                                                     \
    }
    LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_COMPARE_STYLE_GROUP)
#undef LIBWEB_COMPARE_STYLE_GROUP
    return false;
}

void const* ComputedValues::style_group_payload(StyleGroupIndex group) const
{
    switch (group) {
#define LIBWEB_STYLE_GROUP_PAYLOAD_CASE(name, path, sharing_name, affects_layout) \
    case StyleGroupIndex::name:                                                   \
        return &*path;
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_PAYLOAD_CASE)
#undef LIBWEB_STYLE_GROUP_PAYLOAD_CASE
    case StyleGroupIndex::Count:
        break;
    }
    VERIFY_NOT_REACHED();
}

void ComputedValues::borrow_style_record_payloads(ReadonlySpan<void const*> payloads)
{
    VERIFY(payloads.size() == to_underlying(StyleGroupIndex::Count));
    size_t index = 0;
#define LIBWEB_BORROW_STYLE_GROUP(path) path.borrow(payloads[index++]);
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.table)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.list)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.ui)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.svg)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.text)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.box)
    LIBWEB_BORROW_STYLE_GROUP(m_inherited.font)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.animation)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.svg_reset)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.grid)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.anchor)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.effects)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.mask_data)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.text_reset)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.content_data)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.transform)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.background)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.border)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.alignment)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.misc)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.sizing)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.surround)
    LIBWEB_BORROW_STYLE_GROUP(m_noninherited.box)
#undef LIBWEB_BORROW_STYLE_GROUP
    VERIFY(index == payloads.size());
}

ComputedStyleRecordView::ComputedStyleRecordView(StyleEngineFFI::FfiStyleRecordView const& view, StyleComputer const& style_computer, StyleRecordID style_record_identity, bool owns_style_record_pin)
    : m_style_computer(&style_computer)
    , m_style_record_identity(style_record_identity)
    , m_owns_style_record_pin(owns_style_record_pin)
{
    VERIFY(view.present);
    VERIFY(style_record_identity);
    VERIFY(view.payload_count == to_underlying(StyleGroupIndex::Count));
    VERIFY(view.payloads);
    VERIFY(view.base_payloads);
    auto payloads = ReadonlySpan<void const*> { view.payloads, view.payload_count };
    auto base_payloads = ReadonlySpan<void const*> { view.base_payloads, view.payload_count };
    m_values.borrow_style_record_payloads(payloads);
    if (view.animation_overlay_identity != 0) {
        m_base_values.emplace(ComputedValues::BorrowedStyleRecord::Yes);
        m_base_values->borrow_style_record_payloads(base_payloads);
        m_values.m_borrowed_base_values = &*m_base_values;
    }

    m_values.m_pseudo_element_styles = view.pseudo_element_styles;
    m_values.m_depends_on_viewport_metrics = view.dependency_flags & to_underlying(StyleRecordDependencyFlag::DependsOnViewportMetrics);
    m_values.m_font_metrics_depend_on_viewport_metrics = view.dependency_flags & to_underlying(StyleRecordDependencyFlag::FontMetricsDependOnViewportMetrics);
    m_values.m_in_display_none_subtree = view.dependency_flags & to_underlying(StyleRecordDependencyFlag::InDisplayNoneSubtree);
    m_values.m_computed_longhand_table = view.longhand_table;
    if (m_values.m_computed_longhand_table)
        m_values.refresh_computed_longhand_table_views();
    if (view.animated_overlay)
        m_values.m_animated_properties = adopt_ref(*new AnimatedProperties(static_cast<ComputedValuesFFI::AnimatedOverlay const*>(view.animated_overlay)));
    if (view.animation_overlay_identity != 0) {
        m_base_values->m_property_important = m_values.m_property_important;
        m_base_values->m_property_inherited = m_values.m_property_inherited;
        m_base_values->m_pseudo_element_styles = m_values.m_pseudo_element_styles;
        m_base_values->m_depends_on_viewport_metrics = m_values.m_depends_on_viewport_metrics;
        m_base_values->m_font_metrics_depend_on_viewport_metrics = m_values.m_font_metrics_depend_on_viewport_metrics;
        m_base_values->m_in_display_none_subtree = m_values.m_in_display_none_subtree;
        m_base_values->m_inheritance_dependent_specified_values = m_values.m_inheritance_dependent_specified_values;
        m_base_values->m_computed_longhand_table = m_values.m_computed_longhand_table;
        if (m_base_values->m_computed_longhand_table)
            m_base_values->refresh_computed_longhand_table_views();
    }
    m_present = true;
}

ComputedStyleRecordView::~ComputedStyleRecordView()
{
    if (m_style_computer && m_owns_style_record_pin)
        m_style_computer->unpin_style_record(m_style_record_identity);
}

// The table-driven build and the marshalled build must stay on one numbering with the Rust
// mirror in table_group_builder.rs.
static_assert(to_underlying(StyleGroupIndex::InheritedTableValues) == 0);
static_assert(to_underlying(StyleGroupIndex::InheritedListValues) == 1);
static_assert(to_underlying(StyleGroupIndex::InheritedUIValues) == 2);
static_assert(to_underlying(StyleGroupIndex::InheritedSVGValues) == 3);
static_assert(to_underlying(StyleGroupIndex::InheritedTextValues) == 4);
static_assert(to_underlying(StyleGroupIndex::InheritedBoxValues) == 5);
static_assert(to_underlying(StyleGroupIndex::FontValues) == 6);
static_assert(to_underlying(StyleGroupIndex::SVGResetValues) == 8);
static_assert(to_underlying(StyleGroupIndex::GridValues) == 9);
static_assert(to_underlying(StyleGroupIndex::EffectsValues) == 11);
static_assert(to_underlying(StyleGroupIndex::MaskValues) == 12);
static_assert(to_underlying(StyleGroupIndex::ContentValues) == 14);
static_assert(to_underlying(StyleGroupIndex::TransformValues) == 15);
static_assert(to_underlying(StyleGroupIndex::MiscResetValues) == 19);
static_assert(to_underlying(StyleGroupIndex::BackgroundValues) == 16);
static_assert(to_underlying(StyleGroupIndex::BorderValues) == 17);
static_assert(to_underlying(StyleGroupIndex::AlignmentValues) == 18);
static_assert(to_underlying(StyleGroupIndex::SizingValues) == 20);
static_assert(to_underlying(StyleGroupIndex::SurroundValues) == 21);
static_assert(to_underlying(StyleGroupIndex::BoxValues) == 22);
static_assert(to_underlying(StyleGroupIndex::Count) == 23);

// The enum codes the core's transform and effects lowering mirrors.
static_assert(to_underlying(TransformBox::ViewBox) == 4);
static_assert(to_underlying(TransformStyle::Flat) == 0);
static_assert(to_underlying(BackfaceVisibility::Visible) == 0);
static_assert(to_underlying(TransformFunctionParameterType::Angle) == 0);
static_assert(to_underlying(TransformFunctionParameterType::Length) == 1);
static_assert(to_underlying(TransformFunctionParameterType::LengthNone) == 2);
static_assert(to_underlying(TransformFunctionParameterType::LengthPercentage) == 3);
static_assert(to_underlying(TransformFunctionParameterType::Number) == 4);
static_assert(to_underlying(TransformFunctionParameterType::NumberPercentage) == 5);
static_assert(to_underlying(FilterStyleValue::Kind::Blur) == 0);
static_assert(to_underlying(FilterStyleValue::Kind::DropShadow) == 1);
static_assert(to_underlying(FilterStyleValue::Kind::HueRotate) == 2);
static_assert(to_underlying(FilterStyleValue::Kind::Color) == 3);
static_assert(to_underlying(ColorStyleValue::ColorType::RGB) == 0);
static_assert(to_underlying(ColorStyleValue::ColorType::HSL) == 4);
static_assert(to_underlying(ColorStyleValue::ColorType::HWB) == 5);
static_assert(to_underlying(ColorSyntax::Legacy) == 0);
static_assert(to_underlying(ColorSyntax::Modern) == 1);
static_assert(to_underlying(StyleValueList::Separator::Space) == 0);
static_assert(to_underlying(StyleValueList::Separator::Comma) == 1);
static_assert(to_underlying(TimeUnit::Ms) == 0);
static_assert(to_underlying(TimeUnit::S) == 1);
static_assert(to_underlying(AnimationTimelineData::Type::Auto) == 0);
static_assert(to_underlying(AnimationTimelineData::Type::None) == 1);
static_assert(to_underlying(AnimationTimelineData::Type::Name) == 2);
static_assert(to_underlying(AnimationTimelineData::Type::Scroll) == 3);
static_assert(to_underlying(AnimationTimelineData::Type::View) == 4);
static_assert(to_underlying(PositionAnchor::Type::Normal) == 0);
static_assert(to_underlying(PositionAnchor::Type::None) == 1);
static_assert(to_underlying(PositionAnchor::Type::Auto) == 2);
static_assert(to_underlying(PositionAnchor::Type::Name) == 3);
static_assert(to_underlying(BackgroundSize::Contain) == 0);
static_assert(to_underlying(BackgroundSize::Cover) == 1);
static_assert(to_underlying(BackgroundSize::LengthPercentage) == 2);
static_assert(to_underlying(PaintOrder::Fill) == 0);
static_assert(to_underlying(PaintOrder::Stroke) == 1);
static_assert(to_underlying(PaintOrder::Markers) == 2);

static NonnullRefPtr<StyleValue const> animation_style_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    VERIFY(handle.pointer);
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
        static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer)));
}

Gfx::FontCascadeList const& ComputedValues::FontValues::font_list_value() const
{
    VERIFY(font_cascade_list);
    return *static_cast<Gfx::FontCascadeList const*>(font_cascade_list);
}

RefPtr<StyleValue const> ComputedValues::FontValues::font_family_style_value() const
{
    return animation_style_value(font_family);
}

FontStyleKeyword ComputedValues::FontValues::font_style_keyword() const
{
    if (!font_style.pointer)
        return FontStyleKeyword::Normal;
    return animation_style_value(font_style)->as_font_style().font_style();
}

Optional<Utf16FlyString> ComputedValues::MiscResetValues::view_transition_name_value() const
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(view_transition_name.pointer);
    VERIFY(value);
    if (value->tag != StyleValueFFI::StyleValueData::Tag::CustomIdent)
        return {};
    return Utf16FlyString::from_raw(value->custom_ident.custom_ident.raw);
}

TouchActionData ComputedValues::MiscResetValues::touch_action_value() const
{
    return {
        .allow_left = touch_action_allow_left,
        .allow_right = touch_action_allow_right,
        .allow_up = touch_action_allow_up,
        .allow_down = touch_action_allow_down,
        .allow_pinch_zoom = touch_action_allow_pinch_zoom,
        .allow_other = touch_action_allow_other,
    };
}

ScrollSnapAlignData ComputedValues::MiscResetValues::scroll_snap_align_value() const
{
    return {
        .block_alignment = static_cast<ScrollSnapAlign>(scroll_snap_align_block),
        .inline_alignment = static_cast<ScrollSnapAlign>(scroll_snap_align_inline),
    };
}

ScrollSnapType ComputedValues::MiscResetValues::scroll_snap_type_value() const
{
    return {
        .axis = static_cast<ScrollSnapAxis>(scroll_snap_axis),
        .strictness = static_cast<ScrollSnapStrictness>(scroll_snap_strictness),
    };
}

WillChange ComputedValues::MiscResetValues::will_change_value() const
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(will_change.pointer);
    VERIFY(value);
    if (value->tag == StyleValueFFI::StyleValueData::Tag::Keyword) {
        VERIFY(static_cast<Keyword>(value->keyword.keyword) == Keyword::Auto);
        return WillChange::make_auto();
    }
    VERIFY(value->tag == StyleValueFFI::StyleValueData::Tag::ValueList);
    Vector<WillChange::WillChangeEntry> entries;
    entries.ensure_capacity(value->value_list.values.length);
    for (size_t i = 0; i < value->value_list.values.length; ++i) {
        auto const* item = static_cast<StyleValueFFI::StyleValueData const*>(value->value_list.values.pointer[i].pointer);
        VERIFY(item);
        if (item->tag == StyleValueFFI::StyleValueData::Tag::Keyword && static_cast<Keyword>(item->keyword.keyword) == Keyword::Contents) {
            entries.append(WillChange::Type::Contents);
        } else if (item->tag == StyleValueFFI::StyleValueData::Tag::Keyword && static_cast<Keyword>(item->keyword.keyword) == Keyword::ScrollPosition) {
            entries.append(WillChange::Type::ScrollPosition);
        } else if (item->tag == StyleValueFFI::StyleValueData::Tag::CustomIdent) {
            auto custom_ident = Utf16FlyString::from_raw(item->custom_ident.custom_ident.raw);
            if (auto property_id = property_id_from_string(custom_ident); property_id.has_value())
                entries.append(property_id.release_value());
        }
    }
    return WillChange(move(entries));
}

static StyleValueVector style_value_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle, Optional<StyleValueList::Separator> separator)
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer);
    VERIFY(value);
    if (value->tag == StyleValueFFI::StyleValueData::Tag::ValueList
        && (!separator.has_value() || value->value_list.separator == to_underlying(*separator))) {
        StyleValueVector items;
        items.ensure_capacity(value->value_list.values.length);
        for (size_t i = 0; i < value->value_list.values.length; ++i)
            items.unchecked_append(StyleValue::wrap_rust_child(value->value_list.values.pointer[i]));
        return items;
    }
    return { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value)) };
}

static StyleValueVector animation_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    return style_value_items(handle, StyleValueList::Separator::Comma);
}

static StyleValueFFI::StyleValueData const* first_animation_item_data(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer);
    VERIFY(value);
    if (value->tag == StyleValueFFI::StyleValueData::Tag::ValueList
        && value->value_list.separator == to_underlying(StyleValueList::Separator::Comma)) {
        VERIFY(value->value_list.values.length > 0);
        return static_cast<StyleValueFFI::StyleValueData const*>(value->value_list.values.pointer[0].pointer);
    }
    return value;
}

static RefPtr<AbstractImageStyleValue const> first_abstract_image_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    auto const* image_data = first_animation_item_data(handle);
    if (!AK::first_is_one_of(image_data->tag,
            StyleValueFFI::StyleValueData::Tag::Image,
            StyleValueFFI::StyleValueData::Tag::ImageSet,
            StyleValueFFI::StyleValueData::Tag::LinearGradient,
            StyleValueFFI::StyleValueData::Tag::ConicGradient,
            StyleValueFFI::StyleValueData::Tag::RadialGradient))
        return nullptr;
    auto image = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(image_data));
    return image->as_abstract_image();
}

static StyleValueVector component_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    return style_value_items(handle, {});
}

ListStyleType ComputedValues::InheritedListValues::list_style_type_value(StyleScope const& style_scope) const
{
    auto value = animation_style_value(list_style_type);
    if (value->to_keyword() == Keyword::None)
        return Empty {};
    if (value->is_string())
        return value->as_string().string_value().to_utf16_string();

    auto const& counter_style_value = value->as_counter_style();
    auto counter_style_descriptor = counter_style_value.value();
    auto counter_style = counter_style_value.resolve_counter_style(style_scope);
    if (!counter_style) {
        VERIFY(counter_style_descriptor.has<Utf16FlyString>());
        return UnresolvedCounterStyleName { counter_style_descriptor.get<Utf16FlyString>() };
    }
    if (!counter_style_descriptor.has<CounterStyleStyleValue::SymbolsFunction>())
        return counter_style;

    auto const& symbols = counter_style_descriptor.get<CounterStyleStyleValue::SymbolsFunction>();
    return ListStyleSymbols {
        .counter_style = counter_style.release_nonnull(),
        .type = symbols.type,
        .symbols = symbols.symbols,
    };
}

bool ComputedValues::InheritedListValues::list_style_type_depends_on_counter_style_environment() const
{
    auto value = animation_style_value(list_style_type);
    return value->is_counter_style() && value->as_counter_style().value().has<Utf16FlyString>();
}

bool ComputedValues::InheritedListValues::list_style_type_uses_non_overridable_counter_style() const
{
    auto value = animation_style_value(list_style_type);
    return value->is_counter_style()
        && value->as_counter_style().value().has<Utf16FlyString>()
        && CSSCounterStyleRule::matches_non_overridable_counter_style_name(
            value->as_counter_style().value().get<Utf16FlyString>());
}

bool marker_text_depends_on_list_item_counter_value(ListStyleType const& list_style_type)
{
    return list_style_type.visit(
        [](Empty const&) {
            return false;
        },
        [](RefPtr<CounterStyle const> const& counter_style) {
            return !counter_style || !counter_style->representation_is_constant();
        },
        [](Utf16String const&) {
            // A string literal marker is the same for every item, regardless of the counter value.
            return false;
        },
        [](UnresolvedCounterStyleName const&) {
            // The name of a counter style that could not be resolved renders the counter value as `decimal`.
            return true;
        },
        [](ListStyleSymbols const& symbols) {
            return !symbols.counter_style->representation_is_constant();
        });
}

RefPtr<AbstractImageStyleValue const> ComputedValues::InheritedListValues::list_style_image_value() const
{
    auto value = animation_style_value(list_style_image);
    if (!value->is_abstract_image())
        return nullptr;
    return value->as_abstract_image();
}

QuotesData ComputedValues::InheritedListValues::quotes_value() const
{
    auto value = animation_style_value(quotes);
    QuotesData result { .type = QuotesData::Type::Auto };
    if (value->is_keyword()) {
        if (value->to_keyword() == Keyword::None)
            result.type = QuotesData::Type::None;
        return result;
    }

    result.type = QuotesData::Type::Specified;
    auto const& items = value->as_value_list().values();
    VERIFY(items.size() % 2 == 0);
    for (size_t index = 0; index < items.size(); index += 2) {
        result.strings.empend(
            items[index]->as_string().string_value(),
            items[index + 1]->as_string().string_value());
    }
    return result;
}

NonnullRefPtr<StyleValue const> ComputedValues::ContentValues::computed_content_value() const
{
    return animation_style_value(content);
}

bool ComputedValues::ContentValues::content_is_normal() const
{
    auto value = animation_style_value(content);
    return value->is_keyword() && value->to_keyword() == Keyword::Normal;
}

bool ComputedValues::ContentValues::content_uses_list_item_counter() const
{
    auto value = animation_style_value(content);
    if (!value->is_content())
        return false;
    auto item_uses_list_item_counter = [](auto const& item) {
        return item->is_counter() && item->as_counter().counter_name() == list_item_counter_name();
    };
    auto const& content_value = value->as_content();
    if (any_of(content_value.content().values(), item_uses_list_item_counter))
        return true;
    return content_value.alt_text() && any_of(content_value.alt_text()->values(), item_uses_list_item_counter);
}

static Vector<CounterData, 0> counter_data_from_handle(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    auto value = animation_style_value(handle);
    if (value->is_keyword()) {
        VERIFY(value->to_keyword() == Keyword::None);
        return {};
    }

    Vector<CounterData, 0> result;
    auto definitions = value->as_counter_definitions().counter_definitions();
    result.ensure_capacity(definitions.size());
    for (auto const& definition : definitions) {
        Optional<CounterValue> counter_value;
        if (definition.value)
            counter_value = int_from_style_value(NonnullRefPtr<StyleValue const> { *definition.value });
        result.unchecked_append({ definition.name, definition.is_reversed, counter_value });
    }
    return result;
}

Vector<CounterData, 0> ComputedValues::ContentValues::counter_increment_value() const
{
    return counter_data_from_handle(counter_increment);
}

Vector<CounterData, 0> ComputedValues::ContentValues::counter_reset_value() const
{
    return counter_data_from_handle(counter_reset);
}

Vector<CounterData, 0> ComputedValues::ContentValues::counter_set_value() const
{
    return counter_data_from_handle(counter_set);
}

BorderImageData ComputedValues::BorderValues::border_image_value() const
{
    BorderImageData result;
    auto source = animation_style_value(border_image_source);
    if (source->is_abstract_image())
        result.source = source->as_abstract_image();

    auto slice_style_value = animation_style_value(border_image_slice);
    auto const& slice = slice_style_value->as_border_image_slice();
    auto slice_value = [](NonnullRefPtr<StyleValue const> value) -> BorderImageSliceValue {
        if (value->is_number())
            return value->as_number().number();
        if (value->is_integer())
            return static_cast<double>(value->as_integer().integer());
        if (value->is_percentage())
            return value->as_percentage().percentage();
        return NonnullRefPtr<CalculatedStyleValue const> { value->as_calculated() };
    };
    result.slice = {
        slice_value(slice.top()),
        slice_value(slice.right()),
        slice_value(slice.bottom()),
        slice_value(slice.left()),
    };
    result.fill = slice.fill();

    auto width_items = component_items(border_image_width);
    auto width_value = [](NonnullRefPtr<StyleValue const> value) -> BorderImageWidthValue {
        if (value->is_integer())
            return static_cast<double>(value->as_integer().integer());
        if (value->is_number() || (value->is_calculated() && value->as_calculated().resolves_to_number()))
            return number_from_style_value(value, {});
        if (value->is_keyword()) {
            VERIFY(value->to_keyword() == Keyword::Auto);
            return BorderImageWidthAuto {};
        }
        return LengthPercentage::from_style_value(value);
    };
    result.width = {
        width_value(width_items[0]),
        width_value(width_items[1 % width_items.size()]),
        width_value(width_items[2 % width_items.size()]),
        width_value(width_items[3 % width_items.size()]),
    };
    result.width_value_count = static_cast<u8>(width_items.size());

    auto outset_items = component_items(border_image_outset);
    auto outset_value = [](NonnullRefPtr<StyleValue const> value) -> BorderImageOutsetValue {
        if (value->is_integer())
            return static_cast<double>(value->as_integer().integer());
        if (value->is_number() || (value->is_calculated() && value->as_calculated().resolves_to_number()))
            return number_from_style_value(value, {});
        return Length::from_style_value(value, {});
    };
    result.outset = {
        outset_value(outset_items[0]),
        outset_value(outset_items[1 % outset_items.size()]),
        outset_value(outset_items[2 % outset_items.size()]),
        outset_value(outset_items[3 % outset_items.size()]),
    };
    result.outset_value_count = static_cast<u8>(outset_items.size());

    auto repeat_items = component_items(border_image_repeat);
    result.repeat_x = keyword_to_border_image_repeat(repeat_items[0]->to_keyword()).value_or(BorderImageRepeat::Stretch);
    result.repeat_y = keyword_to_border_image_repeat(repeat_items[1 % repeat_items.size()]->to_keyword()).value_or(BorderImageRepeat::Stretch);
    return result;
}

Vector<BackgroundLayerData> ComputedValues::BackgroundValues::background_layers_value() const
{
    auto image_items = animation_items(background_image);
    auto attachment_items = animation_items(background_attachment);
    auto blend_mode_items = animation_items(background_blend_mode);
    auto clip_items = animation_items(background_clip);
    auto origin_items = animation_items(background_origin);
    auto position_x_items = animation_items(background_position_x);
    auto position_y_items = animation_items(background_position_y);
    auto repeat_items = animation_items(background_repeat);
    auto size_items = animation_items(background_size);

    Vector<BackgroundLayerData> layers;
    layers.ensure_capacity(image_items.size());
    for (size_t index = 0; index < image_items.size(); ++index) {
        auto const& image = image_items[index];
        auto const& repeat = repeat_items[index % repeat_items.size()]->as_repeat_style();
        auto const& size = size_items[index % size_items.size()];

        BackgroundLayerData layer;
        layer.image_style_value = image;
        if (image->is_abstract_image())
            layer.background_image = image->as_abstract_image();
        layer.attachment = keyword_to_background_attachment(attachment_items[index % attachment_items.size()]->to_keyword()).release_value();
        layer.blend_mode = keyword_to_mix_blend_mode(blend_mode_items[index % blend_mode_items.size()]->to_keyword()).release_value();
        layer.clip = keyword_to_background_box(clip_items[index % clip_items.size()]->to_keyword()).release_value();
        layer.origin = keyword_to_background_box(origin_items[index % origin_items.size()]->to_keyword()).release_value();
        layer.position_x = LengthPercentage::from_style_value(position_x_items[index % position_x_items.size()]->as_edge().offset());
        layer.position_y = LengthPercentage::from_style_value(position_y_items[index % position_y_items.size()]->as_edge().offset());
        layer.repeat_x = repeat.repeat_x();
        layer.repeat_y = repeat.repeat_y();

        if (size->is_keyword()) {
            switch (size->to_keyword()) {
            case Keyword::Contain:
                layer.size_type = BackgroundSize::Contain;
                break;
            case Keyword::Cover:
                layer.size_type = BackgroundSize::Cover;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        } else {
            auto const& background_size = size->as_background_size();
            layer.size_type = BackgroundSize::LengthPercentage;
            layer.size_x = LengthPercentageOrAuto::from_style_value(background_size.size_x());
            layer.size_y = LengthPercentageOrAuto::from_style_value(background_size.size_y());
        }
        layers.unchecked_append(move(layer));
    }
    return layers;
}

Optional<MaskReference> ComputedValues::MaskValues::mask_value() const
{
    auto const* image = first_animation_item_data(mask_image);
    if (image->tag == StyleValueFFI::StyleValueData::Tag::Url)
        return MaskReference { url_from_rust_data(image->url.url, image->url.url_type, image->url.modifiers) };
    return {};
}

MaskType ComputedValues::MaskValues::mask_type_value() const
{
    return keyword_to_mask_type(animation_style_value(mask_type)->to_keyword()).release_value();
}

RefPtr<AbstractImageStyleValue const> ComputedValues::MaskValues::mask_image_value() const
{
    return first_abstract_image_value(mask_image);
}

Optional<ClipPathReference> ComputedValues::MaskValues::clip_path_value() const
{
    auto const* value_data = static_cast<StyleValueFFI::StyleValueData const*>(clip_path.pointer);
    VERIFY(value_data);
    if (value_data->tag == StyleValueFFI::StyleValueData::Tag::Url)
        return ClipPathReference { url_from_rust_data(value_data->url.url, value_data->url.url_type, value_data->url.modifiers) };
    if (value_data->tag == StyleValueFFI::StyleValueData::Tag::BasicShape) {
        auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value_data));
        return ClipPathReference { value->as_basic_shape() };
    }
    return {};
}

Vector<BackgroundLayerData> ComputedValues::MaskValues::mask_layers_value() const
{
    auto image_items = animation_items(mask_image);
    auto clip_items = animation_items(mask_clip);
    auto composite_items = animation_items(mask_composite);
    auto mode_items = animation_items(mask_mode);
    auto origin_items = animation_items(mask_origin);
    auto position_items = animation_items(mask_position);
    auto repeat_items = animation_items(mask_repeat);
    auto size_items = animation_items(mask_size);

    Vector<BackgroundLayerData> layers;
    layers.ensure_capacity(image_items.size());
    for (size_t index = 0; index < image_items.size(); ++index) {
        auto const& image = image_items[index];
        auto const& clip = clip_items[index % clip_items.size()];
        auto const& composite = composite_items[index % composite_items.size()];
        auto const& mode = mode_items[index % mode_items.size()];
        auto const& origin = origin_items[index % origin_items.size()];
        auto const& position = position_items[index % position_items.size()]->as_position();
        auto const& repeat = repeat_items[index % repeat_items.size()]->as_repeat_style();
        auto const& size = size_items[index % size_items.size()];

        BackgroundLayerData layer;
        layer.origin = BackgroundBox::BorderBox;
        layer.clip = BackgroundBox::BorderBox;
        layer.image_style_value = image;
        if (image->is_abstract_image())
            layer.background_image = image->as_abstract_image();

        auto clip_keyword = clip->to_keyword();
        if (clip_keyword == Keyword::NoClip) {
            layer.mask_clip_is_no_clip = true;
        } else {
            layer.mask_clip = keyword_to_coord_box(clip_keyword).release_value();
            if (auto background_box = keyword_to_background_box(clip_keyword); background_box.has_value())
                layer.clip = background_box.release_value();
        }
        layer.mask_composite = keyword_to_compositing_operator(composite->to_keyword()).release_value();
        layer.mask_mode = keyword_to_masking_mode(mode->to_keyword()).release_value();

        auto origin_keyword = origin->to_keyword();
        layer.mask_origin = keyword_to_coord_box(origin_keyword).release_value();
        if (auto background_box = keyword_to_background_box(origin_keyword); background_box.has_value())
            layer.origin = background_box.release_value();

        layer.position_x = LengthPercentage::from_style_value(position.edge_x()->offset());
        layer.position_y = LengthPercentage::from_style_value(position.edge_y()->offset());
        layer.repeat_x = repeat.repeat_x();
        layer.repeat_y = repeat.repeat_y();

        if (size->is_keyword()) {
            switch (size->to_keyword()) {
            case Keyword::Contain:
                layer.size_type = BackgroundSize::Contain;
                break;
            case Keyword::Cover:
                layer.size_type = BackgroundSize::Cover;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        } else {
            auto const& background_size = size->as_background_size();
            layer.size_type = BackgroundSize::LengthPercentage;
            layer.size_x = LengthPercentageOrAuto::from_style_value(background_size.size_x());
            layer.size_y = LengthPercentageOrAuto::from_style_value(background_size.size_y());
        }
        layers.unchecked_append(move(layer));
    }
    return layers;
}

template<typename T, typename Mapper>
static Vector<T> animation_keyword_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle, Mapper mapper)
{
    Vector<T> result;
    for (auto const& item : animation_items(handle))
        result.append(mapper(item->to_keyword()).release_value());
    return result;
}

static Vector<Time> animation_time_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    Vector<Time> result;
    for (auto const& item : animation_items(handle))
        result.append(Time::from_style_value(item, {}));
    return result;
}

static Vector<Optional<Utf16FlyString>> animation_optional_name_items(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
{
    Vector<Optional<Utf16FlyString>> result;
    for (auto const& item : animation_items(handle)) {
        if (item->is_custom_ident())
            result.append(item->as_custom_ident().custom_ident());
        else
            result.empend();
    }
    return result;
}

Vector<ComputedAnimationName> ComputedValues::AnimationValues::animation_names_value() const
{
    Vector<ComputedAnimationName> result;
    auto value = animation_style_value(animation_name);
    auto const& items = value->as_value_list().values();
    result.ensure_capacity(items.size());
    for (auto const& item : items) {
        if (item->is_keyword()) {
            VERIFY(item->to_keyword() == Keyword::None);
            result.empend();
        } else {
            result.append({
                .name = string_from_style_value(item),
                .syntax = item->is_string() ? ComputedAnimationNameSyntax::String : ComputedAnimationNameSyntax::CustomIdent,
            });
        }
    }
    return result;
}

Vector<Optional<Utf16FlyString>> ComputedValues::AnimationValues::transition_properties_value() const
{
    return animation_optional_name_items(transition_property);
}

Vector<Time> ComputedValues::AnimationValues::transition_durations_value() const
{
    return animation_time_items(transition_duration);
}

Vector<EasingFunction> ComputedValues::AnimationValues::transition_timing_functions_value() const
{
    Vector<EasingFunction> result;
    for (auto const& item : animation_items(transition_timing_function))
        result.append(EasingFunction::from_style_value(item));
    return result;
}

Vector<Time> ComputedValues::AnimationValues::transition_delays_value() const
{
    return animation_time_items(transition_delay);
}

Vector<TransitionBehavior> ComputedValues::AnimationValues::transition_behaviors_value() const
{
    return animation_keyword_items<TransitionBehavior>(transition_behavior, keyword_to_transition_behavior);
}

NonnullRefPtr<ComputedValues const> ComputedValues::create(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const* inherit_parent)
{
    return create_internal(computed_style, document, style_scope, move(color_resolution_context), inherit_parent, nullptr, all_style_groups);
}

NonnullRefPtr<ComputedValues const> ComputedValues::create_over_base(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const& base, u32 groups_to_apply)
{
    return create_internal(computed_style, document, style_scope, move(color_resolution_context), nullptr, &base, groups_to_apply);
}

NonnullRefPtr<ComputedValues const> ComputedValues::create_internal(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const&, ColorResolutionContext color_resolution_context, ComputedValues const* inherit_parent, ComputedValues const* base, u32 groups_to_apply)
{
    // A group outside `groups_to_apply` keeps the base's payload: its build is skipped and it counts
    // as adopted, so the guarded setters below leave it alone. The caller warrants that every
    // property in a skipped group computes to the same value in `computed_style` as it did when
    // `base` was built. Unguarded setters still run, and their value-equality checks are what keeps
    // a skipped group's payload shared rather than cloned.
    // The Rust surround payload duplicates position-anchor for layout, so rebuilding the anchor
    // group must also refresh that payload.
    if ((groups_to_apply >> to_underlying(StyleGroupIndex::AnchorValues)) & 1u)
        groups_to_apply |= 1u << to_underlying(StyleGroupIndex::SurroundValues);
    auto applies = [&](StyleGroupIndex group) { return ((groups_to_apply >> to_underlying(group)) & 1u) != 0; };
    auto builder = base ? Builder { *base } : Builder {};
    auto& computed_values = *builder.operator->();

    // NOTE: color-scheme must resolve first to ensure system colors can be resolved correctly,
    //       and the element's own color right after it, so currentColor can resolve in every
    //       other property (e.g. background-color). Both resolve against the caller's context,
    //       so resolving them up front is order-equivalent to the setters below.
    auto color_scheme = computed_style.color_scheme(document.page().preferred_color_scheme(), document.supported_color_schemes());
    color_resolution_context.color_scheme = color_scheme;
    // FIXME: We should resolve colors to their absolute forms at compute time (i.e. by implementing the relevant absolutized methods)
    auto color = computed_style.color(CSS::PropertyID::Color, color_resolution_context);
    color_resolution_context.current_color = color;

    // Build every group payload the core can map straight from the drive's longhand table.
    auto const* longhand_table = computed_style.computed_longhand_table();
    auto animated_properties = computed_style.animated_properties_snapshot();
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_context_storage;
    auto ffi_color_input = make_rust_color_resolution_input(color_resolution_context, length_context_storage);
    Optional<ComputedValuesFFI::FfiFontGroupBuildInputs> font_group_inputs;
    if (applies(StyleGroupIndex::FontValues)) {
        auto font_list = computed_style.computed_font_list(document.font_computer());
        document.font_computer().pin_font_list_for_style_record(font_list);
        auto const& first_available_font = font_list->first_available_font();
        auto const metrics = first_available_font.pixel_metrics();
        auto math_shift = keyword_to_math_shift(computed_style.property(PropertyID::MathShift).to_keyword()).release_value();
        auto math_style = keyword_to_math_style(computed_style.property(PropertyID::MathStyle).to_keyword()).release_value();
        font_group_inputs = ComputedValuesFFI::FfiFontGroupBuildInputs {
            .font_size_raw = computed_style.font_size().raw_value(),
            .line_height_used_raw = computed_style.line_height(document.font_computer()).raw_value(),
            .font_variant_emoji = to_underlying(computed_style.font_variant_emoji()),
            .font_ascent = metrics.ascent,
            .font_descent = metrics.descent,
            .font_x_height = metrics.x_height,
            .font_zero_advance = metrics.advance_of_ascii_zero,
            .first_available_font = &first_available_font,
            .font_cascade_list = font_list.ptr(),
            .font_weight = computed_style.font_weight(),
            .font_width = computed_style.font_width().value(),
            .math_shift = to_underlying(math_shift),
            .math_style = to_underlying(math_style),
            .math_depth = computed_style.math_depth(),
        };
    }
    ComputedValuesFFI::FfiTableGroupBuildInputs table_build_inputs {
        .color_input = &ffi_color_input,
        .used_color_scheme = static_cast<u8>(to_underlying(color_scheme)),
        .animated_overlay = animated_properties ? animated_properties->overlay() : nullptr,
        .box_display_before_transformation_raw = bit_cast<u32>(computed_style.display_before_box_type_transformation()),
        .font = font_group_inputs.has_value() ? &font_group_inputs.value() : nullptr,
    };
    Array<void const*, to_underlying(StyleGroupIndex::Count)> parent_group_payloads {};
    if (inherit_parent) {
        for (size_t group = 0; group < parent_group_payloads.size(); ++group)
            parent_group_payloads[group] = inherit_parent->style_group_payload(static_cast<StyleGroupIndex>(group));
    }
    Array<void const*, to_underlying(StyleGroupIndex::Count)> table_group_payloads {};
    ComputedValuesFFI::rust_build_group_payloads_from_table(longhand_table, groups_to_apply, parent_group_payloads.data(), &table_build_inputs, table_group_payloads.data(), table_group_payloads.size());
    computed_values.adopt_style_group_payloads(table_group_payloads);
    computed_values.set_property_flag_bitmaps(computed_style.property_importance_bitmap(), computed_style.property_inheritance_bitmap());
    computed_values.set_depends_on_viewport_metrics(computed_style.depends_on_viewport_metrics());
    computed_values.set_font_metrics_depend_on_viewport_metrics(computed_style.font_metrics_depend_on_viewport_metrics());
    computed_values.set_in_display_none_subtree(computed_style.in_display_none_subtree());
    u64 pseudo_element_styles = 0;
    for (auto i = 0; i < to_underlying(PseudoElement::KnownPseudoElementCount); ++i) {
        auto pseudo_element = static_cast<PseudoElement>(i);
        if (computed_style.has_pseudo_element_style(pseudo_element))
            pseudo_element_styles |= 1ull << i;
    }
    computed_values.set_pseudo_element_styles(pseudo_element_styles);
    computed_values.set_computed_longhand_table(computed_style.computed_longhand_table());

    return move(builder).build();
}

ComputedValues::Statistics ComputedValues::s_statistics;

ComputedValues::ComputedValues()
{
    ++s_statistics.live_instance_count;
    ++s_statistics.total_instances_created;
}

ComputedValues::ComputedValues(BorrowedStyleRecord)
    : m_is_style_record_view(true)
{
    m_ref_count = 0;
}

ComputedValues::~ComputedValues()
{
    if (m_is_style_record_view)
        m_computed_longhand_table = nullptr;
    else {
        clear_computed_longhand_table();
        --s_statistics.live_instance_count;
    }
}

void ComputedValues::adopt_computed_longhand_table(void const* table)
{
    if (!table) {
        clear_computed_longhand_table();
        return;
    }
    // NB: Retain before releasing, so adopting the table this style already holds stays safe.
    auto const* typed_table = ComputedValuesFFI::rust_computed_longhand_table_retain(static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(table));
    clear_computed_longhand_table();
    m_computed_longhand_table = typed_table;
    refresh_computed_longhand_table_views();
}

void ComputedValues::refresh_computed_longhand_table_views()
{
    VERIFY(m_computed_longhand_table);
    auto const* table = static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table);
    m_longhand_values = { ComputedValuesFFI::rust_computed_longhand_table_values(table), number_of_longhand_properties };
    m_property_important.copy_from({ ComputedValuesFFI::rust_computed_longhand_table_importance_bits(table), m_property_important.size_in_bytes() });
    m_property_inherited.copy_from({ ComputedValuesFFI::rust_computed_longhand_table_inheritance_bits(table), m_property_inherited.size_in_bytes() });
    size_t inheritance_dependent_value_count = 0;
    auto const* inheritance_dependent_values = ComputedValuesFFI::rust_computed_longhand_table_inheritance_dependent_values(table, &inheritance_dependent_value_count);
    m_inheritance_dependent_specified_values = { inheritance_dependent_values, inheritance_dependent_value_count };
}

void ComputedValues::clear_computed_longhand_table()
{
    if (m_computed_longhand_table)
        ComputedValuesFFI::rust_computed_longhand_table_release(const_cast<ComputedValuesFFI::ComputedLonghandTable*>(static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table)));
    m_computed_longhand_table = nullptr;
    m_longhand_values = {};
    m_inheritance_dependent_specified_values = {};
}

void ComputedValues::copy_computed_longhand_table_from(ComputedValues const& other)
{
    if (other.m_computed_longhand_table) {
        adopt_computed_longhand_table(other.m_computed_longhand_table);
        return;
    }
    clear_computed_longhand_table();
    if (other.m_longhand_values.is_empty())
        return;
    auto* table = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_copy_from_values(table, other.m_longhand_values.data(), other.m_longhand_values.size());
    for (auto const& entry : other.m_inheritance_dependent_specified_values)
        ComputedValuesFFI::rust_computed_longhand_table_add_inheritance_dependent_value(table, entry.property, entry.value);
    ComputedValuesFFI::rust_computed_longhand_table_freeze(table);
    // The freshly created table already carries the one reference this style owns.
    m_computed_longhand_table = table;
    refresh_computed_longhand_table_views();
}

void ComputedValues::adopt_swapped_computed_longhand_table(ComputedValues const& old_values, ComputedValues const& inherited_source)
{
    auto const* old_table = static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(old_values.computed_longhand_table());
    auto const* inherited_source_table = static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(inherited_source.computed_longhand_table());
    if (!old_table || !inherited_source_table) {
        clear_computed_longhand_table();
        return;
    }
    auto* table = ComputedValuesFFI::rust_computed_longhand_table_create_with_inherited_values(old_table, inherited_source_table);
    clear_computed_longhand_table();
    // The freshly created table already carries the one reference this style owns.
    m_computed_longhand_table = table;
    refresh_computed_longhand_table_views();
}

void ComputedValues::Mutator::set_animated_properties(AnimatedProperties const* value)
{
    m_values.m_animated_properties = value;
}

RefPtr<AnimatedProperties const> ComputedValues::animated_properties_snapshot() const
{
    return m_animated_properties;
}

RefPtr<StyleValue const> ComputedValues::style_value_from_handle(PropertyID property_id, RustStyleValueHandle const& handle) const
{
    if (!handle) {
        if (m_style_value_cache)
            m_style_value_cache->remove(property_id);
        return nullptr;
    }
    if (m_style_value_cache) {
        if (auto it = m_style_value_cache->find(property_id); it != m_style_value_cache->end() && it->value->rust_style_value_data() == handle.data())
            return it->value;
    }
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(handle.data()));
    count_longhand_wrapper_mint();
    if (!m_style_value_cache)
        m_style_value_cache = make<HashMap<PropertyID, NonnullRefPtr<StyleValue const>>>();
    m_style_value_cache->set(property_id, value);
    return value;
}

RustStyleValueHandle const* ComputedValues::stored_style_value_handle(PropertyID property_id) const
{
    auto from_ffi_handle = [](ComputedValuesFFI::ComputedStyleValueHandle const& handle) -> RustStyleValueHandle const* {
        static_assert(sizeof(RustStyleValueHandle) == sizeof(handle));
        return reinterpret_cast<RustStyleValueHandle const*>(&handle);
    };
    auto non_empty = [](RustStyleValueHandle const* handle) -> RustStyleValueHandle const* {
        return (handle && *handle) ? handle : nullptr;
    };
    switch (property_id) {
    case PropertyID::Cx:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->cx));
    case PropertyID::Cy:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->cy));
    case PropertyID::D:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->d));
    case PropertyID::GridAutoColumns:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_auto_columns_style_value));
    case PropertyID::GridAutoRows:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_auto_rows_style_value));
    case PropertyID::GridColumnEnd:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_column_end_style_value));
    case PropertyID::GridColumnStart:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_column_start_style_value));
    case PropertyID::GridRowEnd:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_row_end_style_value));
    case PropertyID::GridRowStart:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_row_start_style_value));
    case PropertyID::GridTemplateAreas:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_template_areas_style_value));
    case PropertyID::GridTemplateColumns:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_template_columns_style_value));
    case PropertyID::GridTemplateRows:
        return non_empty(from_ffi_handle(m_noninherited.grid->grid_template_rows_style_value));
    case PropertyID::LetterSpacing:
        return non_empty(from_ffi_handle(m_inherited.text->letter_spacing_style_value));
    case PropertyID::R:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->r));
    case PropertyID::Rx:
        if (m_noninherited.svg_reset->rx.is_auto)
            return nullptr;
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->rx.value));
    case PropertyID::Ry:
        if (m_noninherited.svg_reset->ry.is_auto)
            return nullptr;
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->ry.value));
    case PropertyID::WordSpacing:
        return non_empty(from_ffi_handle(m_inherited.text->word_spacing_style_value));
    case PropertyID::X:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->x));
    case PropertyID::Y:
        return non_empty(from_ffi_handle(m_noninherited.svg_reset->y));
    default:
        return nullptr;
    }
}

RefPtr<StyleValue const> ComputedValues::color_style_value() const
{
    if (m_inherited.text->color_style_value.pointer) {
        auto handle = RustStyleValueHandle::retained(
            static_cast<StyleValueFFI::StyleValueData const*>(m_inherited.text->color_style_value.pointer));
        return style_value_from_handle(PropertyID::Color, handle);
    }
    return computed_style_value(PropertyID::Color);
}

RefPtr<StyleValue const> ComputedValues::raw_cascaded_font_size() const
{
    if (!m_computed_longhand_table)
        return {};
    auto const* data = ComputedValuesFFI::rust_computed_longhand_table_raw_cascaded_font_size(
        static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table));
    if (!data)
        return {};
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data)));
}

RefPtr<StyleValue const> ComputedValues::background_color_style_value() const
{
    auto const& handle = m_noninherited.background->background_color_style_value;
    static_assert(sizeof(RustStyleValueHandle) == sizeof(handle));
    return style_value_from_handle(PropertyID::BackgroundColor, reinterpret_cast<RustStyleValueHandle const&>(handle));
}

static bool style_value_contains_anchor_function(StyleValue const& value)
{
    if (value.is_anchor())
        return true;
    if (value.is_calculated())
        return value.as_calculated().contains_anchor_function();
    return false;
}

bool ComputedValues::inset_properties_contain_anchor_functions() const
{
    // A bare anchor function is not stored in the inset length box at all: it lives in the
    // per-side anchor inset handles kept next to it.
    if (has_anchor_inset(PropertyID::Top) || has_anchor_inset(PropertyID::Right)
        || has_anchor_inset(PropertyID::Bottom) || has_anchor_inset(PropertyID::Left))
        return true;
    // Anchor functions inside expressions survive to used-value time as calculated values, so
    // when no inset is calculated (the common case), skip reconstructing the style values.
    auto const& inset_box = inset();
    if (!inset_box.top().is_calculated() && !inset_box.right().is_calculated() && !inset_box.bottom().is_calculated() && !inset_box.left().is_calculated())
        return false;
    auto top = computed_style_value(PropertyID::Top);
    auto right = computed_style_value(PropertyID::Right);
    auto bottom = computed_style_value(PropertyID::Bottom);
    auto left = computed_style_value(PropertyID::Left);
    VERIFY(top && right && bottom && left);
    return style_value_contains_anchor_function(*top)
        || style_value_contains_anchor_function(*right)
        || style_value_contains_anchor_function(*bottom)
        || style_value_contains_anchor_function(*left);
}

RefPtr<StyleValue const> ComputedValues::computed_style_value(PropertyID property_id, WithAnimationsApplied with_animations_applied) const
{
    if (with_animations_applied == WithAnimationsApplied::No && has_animated_values())
        return base_values().computed_style_value(property_id);

    if (property_is_logical_alias(property_id))
        property_id = map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { writing_mode(), direction() });

    if (property_id < first_longhand_property_id || property_id > last_longhand_property_id)
        return {};

    if (auto inset = anchor_inset(property_id))
        return inset;

    // The animated overlay first, under the overlay read rule the Rust side implements once:
    // important base values override animated but not transitioned properties.
    if (with_animations_applied == WithAnimationsApplied::Yes && m_animated_properties
        && ComputedValuesFFI::rust_animated_overlay_effective_value(m_animated_properties->overlay(), to_underlying(property_id), is_property_important(property_id)))
        return m_animated_properties->property(property_id);

    if (m_longhand_values.is_empty())
        return {};
    auto const* stored = m_longhand_values[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
    if (!stored)
        return {};
    if (m_style_value_cache) {
        if (auto it = m_style_value_cache->find(property_id); it != m_style_value_cache->end() && it->value->rust_style_value_data() == stored)
            return it->value;
    }
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(stored)));
    count_longhand_wrapper_mint();
    if (!m_style_value_cache)
        m_style_value_cache = make<HashMap<PropertyID, NonnullRefPtr<StyleValue const>>>();
    m_style_value_cache->set(property_id, value);
    return value;
}

RefPtr<StyleValue const> ComputedValues::computed_style_value_for_inheritance(PropertyID property_id, WithAnimationsApplied with_animations_applied) const
{
    if (with_animations_applied == WithAnimationsApplied::No && has_animated_values())
        return base_values().computed_style_value_for_inheritance(property_id);

    for (auto const& entry : m_inheritance_dependent_specified_values) {
        if (entry.property != to_underlying(property_id))
            continue;
        auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(entry.value);
        auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data));
        if (value->depends_on_current_color())
            return value;
        break;
    }

    return computed_style_value(property_id, with_animations_applied);
}

static ContentDataAndQuoteNestingLevel resolve_content(StyleValue const& value, QuotesData const& quotes_data, DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level, NotifyListItemCounterRendered notify_list_item_counter_rendered)
{
    auto quote_nesting_level = initial_quote_nesting_level;

    auto get_quote_string = [&](bool open, auto depth) {
        switch (quotes_data.type) {
        case QuotesData::Type::None:
            return Utf16FlyString {};
        case QuotesData::Type::Auto:
            // FIXME: "A typographically appropriate used value for quotes is automatically chosen by the UA
            //        based on the content language of the element and/or its parent."
            if (open)
                return depth == 0 ? u"“"_utf16_fly_string : u"‘"_utf16_fly_string;
            return depth == 0 ? u"”"_utf16_fly_string : u"’"_utf16_fly_string;
        case QuotesData::Type::Specified:
            // If the depth is greater than the number of pairs, the last pair is repeated.
            auto& level = quotes_data.strings[min(depth, quotes_data.strings.size() - 1)];
            return open ? level[0] : level[1];
        }
        VERIFY_NOT_REACHED();
    };

    if (value.is_content()) {
        auto& content_style_value = value.as_content();

        ContentData content_data;

        Utf16StringBuilder pending_text;
        bool has_pending_text = false;
        auto append_text = [&](Utf16View const& text) {
            pending_text.append(text);
            has_pending_text = true;
        };
        auto flush_pending_text = [&] {
            if (!has_pending_text)
                return;
            content_data.data.append(pending_text.to_string());
            pending_text.clear();
            has_pending_text = false;
        };

        for (auto const& item : content_style_value.content().values()) {
            if (item->is_string()) {
                append_text(item->as_string().string_value().view());
            } else if (item->is_keyword()) {
                switch (item->to_keyword()) {
                case Keyword::OpenQuote:
                    append_text(get_quote_string(true, quote_nesting_level++).view());
                    break;
                case Keyword::CloseQuote:
                    // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                    // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                    // 'content' property's value is still inserted).
                    // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                    // (This is missing from the CONTENT-3 spec.)
                    if (quote_nesting_level > 0)
                        append_text(get_quote_string(false, --quote_nesting_level).view());
                    break;
                case Keyword::NoOpenQuote:
                    quote_nesting_level++;
                    break;
                case Keyword::NoCloseQuote:
                    // NOTE: See CloseQuote
                    if (quote_nesting_level > 0)
                        quote_nesting_level--;
                    break;
                default:
                    dbgln("`{}` is not supported in `content` (yet?)", item->to_string(SerializationMode::Normal));
                    break;
                }
            } else if (item->is_counter()) {
                flush_pending_text();
                if (notify_list_item_counter_rendered == NotifyListItemCounterRendered::Yes && item->as_counter().counter_name() == list_item_counter_name())
                    element_reference.element().document().did_render_list_item_counter_value(element_reference.element());
                content_data.counter_style_dependencies.append(item->as_counter().counter_style()->as_counter_style().resolve_counter_style(element_reference.style_scope()));
                content_data.data.append(item->as_counter().resolve(element_reference));
            } else if (item->is_image() || item->is_image_set()) {
                // https://drafts.csswg.org/css-content-3/#typedef-content-list
                // https://drafts.csswg.org/css-images-4/#typedef-image
                // <content-list> accepts <image>, and image-set() is an <image>.
                flush_pending_text();
                content_data.data.append(NonnullRefPtr { const_cast<AbstractImageStyleValue&>(item->as_abstract_image()) });
            } else {
                // TODO: Implement images, and other things.
                dbgln("`{}` is not supported in `content` (yet?)", item->to_string(SerializationMode::Normal));
            }
        }
        flush_pending_text();
        content_data.type = ContentData::Type::List;

        if (auto alt_text = content_style_value.alt_text()) {
            Utf16StringBuilder alt_text_builder;
            for (auto const& item : alt_text->values()) {
                if (item->is_string()) {
                    alt_text_builder.append(item->as_string().string_value().view());
                } else if (item->is_counter()) {
                    if (notify_list_item_counter_rendered == NotifyListItemCounterRendered::Yes && item->as_counter().counter_name() == list_item_counter_name())
                        element_reference.element().document().did_render_list_item_counter_value(element_reference.element());
                    content_data.counter_style_dependencies.append(item->as_counter().counter_style()->as_counter_style().resolve_counter_style(element_reference.style_scope()));
                    alt_text_builder.append(item->as_counter().resolve(element_reference));
                } else {
                    dbgln("`{}` is not supported in `content` alt-text (yet?)", item->to_string(SerializationMode::Normal));
                }
            }
            content_data.alt_text = alt_text_builder.to_string();
        }

        return { content_data, quote_nesting_level };
    }

    switch (value.to_keyword()) {
    case Keyword::None:
        return { { ContentData::Type::None, {}, {} }, quote_nesting_level };
    case Keyword::Normal:
        return { { ContentData::Type::Normal, {}, {} }, quote_nesting_level };
    default:
        break;
    }

    return { {}, quote_nesting_level };
}

ContentDataAndQuoteNestingLevel ComputedValues::resolved_content(DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level, NotifyListItemCounterRendered notify_list_item_counter_rendered) const
{
    // Read the content group's value directly, including the resource context attached to its images.
    auto value = computed_content();
    return resolve_content(value, quotes(), element_reference, initial_quote_nesting_level, notify_list_item_counter_rendered);
}

}
