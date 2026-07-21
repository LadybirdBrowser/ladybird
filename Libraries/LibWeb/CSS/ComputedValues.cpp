/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OverflowClipMarginStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

template<typename T>
static consteval ComputedValuesFFI::StyleGroupVTable make_style_group_vtable()
{
    return {
        .size = sizeof(T),
        .align = alignof(T),
        .default_construct = [](void* payload) {
            if constexpr (requires { T::make_default_payload_value(); })
                new (payload) T(T::make_default_payload_value());
            else
                new (payload) T(); },
        .copy_construct = [](void* payload, void const* source) { new (payload) T(*static_cast<T const*>(source)); },
        .destruct = [](void* payload) { static_cast<T*>(payload)->~T(); },
        .equals = [](void const* a, void const* b) {
            if constexpr (requires(T const& value) { value == value; })
                return *static_cast<T const*>(a) == *static_cast<T const*>(b);
            else
                return false; },
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

// The properties feeding the alignment group's descriptors, in registration
// order; create() gathers their computed values in the same order.
static constexpr Array alignment_group_properties {
    PropertyID::FlexDirection,
    PropertyID::FlexWrap,
    PropertyID::FlexBasis,
    PropertyID::FlexGrow,
    PropertyID::FlexShrink,
    PropertyID::Order,
    PropertyID::AlignContent,
    PropertyID::AlignItems,
    PropertyID::AlignSelf,
    PropertyID::JustifyContent,
    PropertyID::JustifyItems,
    PropertyID::JustifySelf,
    PropertyID::ColumnGap,
    PropertyID::RowGap,
};

// The properties feeding the text reset group's descriptors, in registration
// order.
static constexpr Array text_reset_group_properties {
    PropertyID::TextDecorationLine,
    PropertyID::TextDecorationThickness,
    PropertyID::TextDecorationStyle,
    PropertyID::TextDecorationColor,
    PropertyID::TextOverflow,
    PropertyID::UnicodeBidi,
    PropertyID::WhiteSpaceTrim,
};

// The properties feeding the effects group's descriptors, in registration
// order.
static constexpr Array effects_group_properties {
    PropertyID::Opacity,
    PropertyID::Filter,
    PropertyID::BackdropFilter,
    PropertyID::MixBlendMode,
    PropertyID::Isolation,
    PropertyID::BoxShadow,
    PropertyID::Clip,
};

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

// The properties feeding the misc reset group's descriptors, in registration
// order; Appearance appears twice, once per derived field.
static constexpr Array misc_reset_group_properties {
    PropertyID::ScrollMarginTop,
    PropertyID::ScrollMarginRight,
    PropertyID::ScrollMarginBottom,
    PropertyID::ScrollMarginLeft,
    PropertyID::ScrollPaddingTop,
    PropertyID::ScrollPaddingRight,
    PropertyID::ScrollPaddingBottom,
    PropertyID::ScrollPaddingLeft,
    PropertyID::OverflowClipMarginTop,
    PropertyID::OverflowClipMarginRight,
    PropertyID::OverflowClipMarginBottom,
    PropertyID::OverflowClipMarginLeft,
    PropertyID::ColumnSpan,
    PropertyID::Appearance,
    PropertyID::Appearance,
    PropertyID::OutlineStyle,
    PropertyID::ObjectFit,
    PropertyID::ColumnCount,
    PropertyID::ColumnWidth,
    PropertyID::ColumnHeight,
    PropertyID::OutlineColor,
    PropertyID::OutlineOffset,
    PropertyID::OutlineWidth,
    PropertyID::TableLayout,
    PropertyID::UserSelect,
    PropertyID::ObjectPosition,
    PropertyID::ViewTransitionName,
    PropertyID::TouchAction,
    PropertyID::ScrollBehavior,
    PropertyID::ScrollbarGutter,
    PropertyID::ScrollbarWidth,
    PropertyID::ShapeImageThreshold,
    PropertyID::ShapeMargin,
    PropertyID::ShapeOutside,
};

// overflow-wrap has no generated keyword converter; the mapping matches the
// switch in create().
static Optional<OverflowWrap> overflow_wrap_from_keyword(Keyword keyword)
{
    switch (keyword) {
    case Keyword::Normal:
        return OverflowWrap::Normal;
    case Keyword::BreakWord:
        return OverflowWrap::BreakWord;
    case Keyword::Anywhere:
        return OverflowWrap::Anywhere;
    default:
        return {};
    }
}

// The properties feeding the inherited text group's descriptors, in
// registration order; the doubled properties feed two fields each.
static constexpr Array inherited_text_group_properties {
    PropertyID::Color,
    PropertyID::Color,
    PropertyID::WebkitTextFillColor,
    PropertyID::WebkitTextFillColor,
    PropertyID::TextShadow,
    PropertyID::TextAlign,
    PropertyID::TextJustify,
    PropertyID::TextTransform,
    PropertyID::TextWrapMode,
    PropertyID::TextWrapStyle,
    PropertyID::TextDecorationSkipInk,
    PropertyID::TextUnderlinePosition,
    PropertyID::TextUnderlineOffset,
    PropertyID::TextIndent,
    PropertyID::TabSize,
    PropertyID::WhiteSpaceCollapse,
    PropertyID::WordBreak,
    PropertyID::OverflowWrap,
    PropertyID::WordSpacing,
    PropertyID::WordSpacing,
    PropertyID::LetterSpacing,
    PropertyID::LetterSpacing,
    PropertyID::Orphans,
    PropertyID::Widows,
};

// The properties feeding the inherited UI group's descriptors, in
// registration order; the doubled properties feed two fields each.
static constexpr Array inherited_ui_group_properties {
    PropertyID::CaretColor,
    PropertyID::CaretColor,
    PropertyID::AccentColor,
    PropertyID::AccentColor,
    PropertyID::Cursor,
    PropertyID::PointerEvents,
    PropertyID::ScrollbarColor,
    PropertyID::ColorScheme,
    PropertyID::ColorScheme,
};

// The properties feeding the sizing group's descriptors, in registration
// order. All six register as keyword constraints: the group adopts a shared
// payload when every size is untouched and falls back to the setters
// otherwise, until the core learns the size representation.
static constexpr Array sizing_group_properties {
    PropertyID::Width,
    PropertyID::MinWidth,
    PropertyID::MaxWidth,
    PropertyID::Height,
    PropertyID::MinHeight,
    PropertyID::MaxHeight,
};

// The properties feeding the transform group's descriptors, in registration
// order.
static constexpr Array transform_group_properties {
    PropertyID::Transform,
    PropertyID::TransformBox,
    PropertyID::TransformOrigin,
    PropertyID::TransformStyle,
    PropertyID::Rotate,
    PropertyID::Translate,
    PropertyID::Scale,
    PropertyID::Perspective,
    PropertyID::PerspectiveOrigin,
};

// The properties feeding the mask group's descriptors, in registration
// order.
static constexpr Array mask_group_properties {
    PropertyID::MaskImage,
    PropertyID::MaskType,
    PropertyID::ClipPath,
    PropertyID::MaskMode,
    PropertyID::MaskRepeat,
    PropertyID::MaskPosition,
    PropertyID::MaskClip,
    PropertyID::MaskOrigin,
    PropertyID::MaskSize,
    PropertyID::MaskComposite,
};

// The properties feeding the grid group's descriptors, in registration
// order. Every field registers as an initial-value constraint until the
// core learns the grid representations.
static constexpr Array grid_group_properties {
    PropertyID::GridAutoColumns,
    PropertyID::GridAutoRows,
    PropertyID::GridTemplateColumns,
    PropertyID::GridTemplateRows,
    PropertyID::GridAutoFlow,
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

// The properties feeding the SVG reset group's descriptors, in registration
// order.
static constexpr Array svg_reset_group_properties {
    PropertyID::Cx,
    PropertyID::Cy,
    PropertyID::R,
    PropertyID::Rx,
    PropertyID::Ry,
    PropertyID::X,
    PropertyID::Y,
    PropertyID::StopColor,
    PropertyID::StopOpacity,
    PropertyID::FloodColor,
    PropertyID::FloodOpacity,
    PropertyID::VectorEffect,
    PropertyID::ShapeRendering,
};

// The properties feeding the inherited SVG group's descriptors, in
// registration order.
static constexpr Array inherited_svg_group_properties {
    PropertyID::Fill,
    PropertyID::Stroke,
    PropertyID::FillRule,
    PropertyID::ClipRule,
    PropertyID::FillOpacity,
    PropertyID::StrokeOpacity,
    PropertyID::StrokeLinecap,
    PropertyID::StrokeLinejoin,
    PropertyID::StrokeDasharray,
    PropertyID::StrokeDashoffset,
    PropertyID::StrokeMiterlimit,
    PropertyID::StrokeWidth,
    PropertyID::ColorInterpolation,
    PropertyID::ColorInterpolationFilters,
    PropertyID::PaintOrder,
    PropertyID::TextAnchor,
    PropertyID::DominantBaseline,
};

static void register_style_group_field_descriptors()
{
    using namespace ComputedValuesFFI;
    static_assert(sizeof(FlexDirection) == 1 && sizeof(FlexWrap) == 1 && sizeof(AlignContent) == 1
        && sizeof(AlignItems) == 1 && sizeof(AlignSelf) == 1 && sizeof(JustifyContent) == 1
        && sizeof(JustifyItems) == 1 && sizeof(JustifySelf) == 1);

    Vector<FfiGroupFieldDescriptor> descriptors;
    auto add = [&](size_t group_index, PropertyID property, u32 offset, u8 kind, u16 keyword, Array<u8, number_of_keywords> const* keyword_table, double required_px = 0) {
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

    using Alignment = ComputedValues::AlignmentValues;
    constexpr auto alignment = to_underlying(StyleGroupIndex::AlignmentValues);
    add(alignment, PropertyID::FlexDirection, offsetof(Alignment, flex_direction), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_flex_direction>());
    add(alignment, PropertyID::FlexWrap, offsetof(Alignment, flex_wrap), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_flex_wrap>());
    add(alignment, PropertyID::FlexBasis, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(alignment, PropertyID::FlexGrow, offsetof(Alignment, flex_grow), GROUP_FIELD_F64, 0, nullptr);
    add(alignment, PropertyID::FlexShrink, offsetof(Alignment, flex_shrink), GROUP_FIELD_F64, 0, nullptr);
    add(alignment, PropertyID::Order, offsetof(Alignment, order), GROUP_FIELD_I32, 0, nullptr);
    add(alignment, PropertyID::AlignContent, offsetof(Alignment, align_content), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_align_content>());
    add(alignment, PropertyID::AlignItems, offsetof(Alignment, align_items), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_align_items>());
    add(alignment, PropertyID::AlignSelf, offsetof(Alignment, align_self), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_align_self>());
    add(alignment, PropertyID::JustifyContent, offsetof(Alignment, justify_content), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_justify_content>());
    add(alignment, PropertyID::JustifyItems, offsetof(Alignment, justify_items), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_justify_items>());
    add(alignment, PropertyID::JustifySelf, offsetof(Alignment, justify_self), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_justify_self>());
    add(alignment, PropertyID::ColumnGap, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Normal), nullptr);
    add(alignment, PropertyID::RowGap, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Normal), nullptr);

    static_assert(sizeof(Color) == sizeof(u32));
    using TextReset = ComputedValues::TextResetValues;
    constexpr auto text_reset = to_underlying(StyleGroupIndex::TextResetValues);
    add(text_reset, PropertyID::TextDecorationLine, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(text_reset, PropertyID::TextDecorationThickness, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(text_reset, PropertyID::TextDecorationStyle, offsetof(TextReset, text_decoration_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_decoration_style>());
    add(text_reset, PropertyID::TextDecorationColor, offsetof(TextReset, text_decoration_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(text_reset, PropertyID::TextOverflow, offsetof(TextReset, text_overflow), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_text_overflow>());
    add(text_reset, PropertyID::UnicodeBidi, offsetof(TextReset, unicode_bidi), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_unicode_bidi>());
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

    using MiscReset = ComputedValues::MiscResetValues;
    constexpr auto misc_reset = to_underlying(StyleGroupIndex::MiscResetValues);
    for (auto property : { PropertyID::ScrollMarginTop, PropertyID::ScrollMarginRight, PropertyID::ScrollMarginBottom, PropertyID::ScrollMarginLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    for (auto property : { PropertyID::ScrollPaddingTop, PropertyID::ScrollPaddingRight, PropertyID::ScrollPaddingBottom, PropertyID::ScrollPaddingLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    for (auto property : { PropertyID::OverflowClipMarginTop, PropertyID::OverflowClipMarginRight, PropertyID::OverflowClipMarginBottom, PropertyID::OverflowClipMarginLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ColumnSpan, offsetof(MiscReset, column_span), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_column_span>());
    add(misc_reset, PropertyID::Appearance, offsetof(MiscReset, appearance), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<appearance_without_compat_from_keyword>());
    add(misc_reset, PropertyID::Appearance, offsetof(MiscReset, computed_appearance), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_appearance>());
    add(misc_reset, PropertyID::OutlineStyle, offsetof(MiscReset, outline_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_outline_style>());
    add(misc_reset, PropertyID::ObjectFit, offsetof(MiscReset, object_fit), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_object_fit>());
    add(misc_reset, PropertyID::ColumnCount, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(misc_reset, PropertyID::ColumnWidth, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(misc_reset, PropertyID::ColumnHeight, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(misc_reset, PropertyID::OutlineColor, offsetof(MiscReset, outline_color), GROUP_FIELD_COLOR_OR_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(misc_reset, PropertyID::OutlineOffset, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    add(misc_reset, PropertyID::OutlineWidth, offsetof(MiscReset, outline_width), GROUP_FIELD_CSS_PIXELS_NON_NEGATIVE, 0, nullptr);
    add(misc_reset, PropertyID::TableLayout, offsetof(MiscReset, table_layout), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_table_layout>());
    add(misc_reset, PropertyID::UserSelect, offsetof(MiscReset, user_select), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_user_select>());
    add(misc_reset, PropertyID::ObjectPosition, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ViewTransitionName, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(misc_reset, PropertyID::TouchAction, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ScrollBehavior, offsetof(MiscReset, scroll_behavior), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_scroll_behavior>());
    add(misc_reset, PropertyID::ScrollbarGutter, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ScrollbarWidth, offsetof(MiscReset, scrollbar_width), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_scrollbar_width>());
    add(misc_reset, PropertyID::ShapeImageThreshold, offsetof(MiscReset, shape_image_threshold), GROUP_FIELD_RESOLVED_F64, 0, nullptr);
    add(misc_reset, PropertyID::ShapeMargin, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    add(misc_reset, PropertyID::ShapeOutside, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);

    using InheritedText = ComputedValues::InheritedTextValues;
    constexpr auto inherited_text = to_underlying(StyleGroupIndex::InheritedTextValues);
    add(inherited_text, PropertyID::Color, offsetof(InheritedText, color), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_text, PropertyID::Color, offsetof(InheritedText, color_style_value), GROUP_FIELD_RETAINED_SHELL, 0, nullptr);
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
    add(inherited_text, PropertyID::OverflowWrap, offsetof(InheritedText, overflow_wrap), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<overflow_wrap_from_keyword>());
    add(inherited_text, PropertyID::WordSpacing, offsetof(InheritedText, word_spacing), GROUP_FIELD_CSS_PIXELS, 0, nullptr);
    add(inherited_text, PropertyID::WordSpacing, offsetof(InheritedText, word_spacing_style_value), GROUP_FIELD_RETAINED_SHELL, 0, nullptr);
    add(inherited_text, PropertyID::LetterSpacing, offsetof(InheritedText, letter_spacing), GROUP_FIELD_CSS_PIXELS, 0, nullptr);
    add(inherited_text, PropertyID::LetterSpacing, offsetof(InheritedText, letter_spacing_style_value), GROUP_FIELD_RETAINED_SHELL, 0, nullptr);
    add(inherited_text, PropertyID::Orphans, offsetof(InheritedText, orphans), GROUP_FIELD_U64, 0, nullptr);
    add(inherited_text, PropertyID::Widows, offsetof(InheritedText, widows), GROUP_FIELD_U64, 0, nullptr);

    using InheritedUI = ComputedValues::InheritedUIValues;
    constexpr auto inherited_ui = to_underlying(StyleGroupIndex::InheritedUIValues);
    add(inherited_ui, PropertyID::CaretColor, offsetof(InheritedUI, caret_color) + offsetof(ColorOrAuto, used_value), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_ui, PropertyID::CaretColor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::AccentColor, offsetof(InheritedUI, accent_color) + offsetof(ColorOrAuto, used_value), GROUP_FIELD_COLOR, 0, nullptr);
    add(inherited_ui, PropertyID::AccentColor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::Cursor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::PointerEvents, offsetof(InheritedUI, pointer_events), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_pointer_events>());
    add(inherited_ui, PropertyID::ScrollbarColor, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(inherited_ui, PropertyID::ColorScheme, offsetof(InheritedUI, color_scheme), GROUP_FIELD_RESOLVED_U8, 0, nullptr);
    add(inherited_ui, PropertyID::ColorScheme, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    constexpr auto sizing = to_underlying(StyleGroupIndex::SizingValues);
    add(sizing, PropertyID::Width, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(sizing, PropertyID::MinWidth, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(sizing, PropertyID::MaxWidth, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(sizing, PropertyID::Height, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(sizing, PropertyID::MinHeight, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(sizing, PropertyID::MaxHeight, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);

    using Transform = ComputedValues::TransformValues;
    constexpr auto transform = to_underlying(StyleGroupIndex::TransformValues);
    add(transform, PropertyID::Transform, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::TransformBox, offsetof(Transform, transform_box), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_transform_box>());
    add(transform, PropertyID::TransformOrigin, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(transform, PropertyID::TransformStyle, offsetof(Transform, transform_style), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_transform_style>());
    add(transform, PropertyID::Rotate, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::Translate, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::Scale, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::Perspective, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(transform, PropertyID::PerspectiveOrigin, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    using Mask = ComputedValues::MaskValues;
    constexpr auto mask = to_underlying(StyleGroupIndex::MaskValues);
    add(mask, PropertyID::MaskImage, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(mask, PropertyID::MaskType, offsetof(Mask, mask_type), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_mask_type>());
    add(mask, PropertyID::ClipPath, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::None), nullptr);
    add(mask, PropertyID::MaskMode, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskRepeat, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskPosition, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskClip, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskOrigin, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskSize, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(mask, PropertyID::MaskComposite, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    constexpr auto grid = to_underlying(StyleGroupIndex::GridValues);
    for (auto property : grid_group_properties)
        add(grid, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    constexpr auto animation = to_underlying(StyleGroupIndex::AnimationValues);
    for (auto property : animation_group_properties)
        add(animation, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);

    using SVGReset = ComputedValues::SVGResetValues;
    constexpr auto svg_reset = to_underlying(StyleGroupIndex::SVGResetValues);
    for (auto property : { PropertyID::Cx, PropertyID::Cy, PropertyID::R, PropertyID::X, PropertyID::Y })
        add(svg_reset, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(svg_reset, PropertyID::Rx, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(svg_reset, PropertyID::Ry, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    add(svg_reset, PropertyID::StopColor, offsetof(SVGReset, stop_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(svg_reset, PropertyID::StopOpacity, offsetof(SVGReset, stop_opacity), GROUP_FIELD_RESOLVED_F32, 0, nullptr);
    add(svg_reset, PropertyID::FloodColor, offsetof(SVGReset, flood_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(svg_reset, PropertyID::FloodOpacity, offsetof(SVGReset, flood_opacity), GROUP_FIELD_RESOLVED_F32, 0, nullptr);
    add(svg_reset, PropertyID::VectorEffect, offsetof(SVGReset, vector_effect), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_vector_effect>());
    add(svg_reset, PropertyID::ShapeRendering, offsetof(SVGReset, shape_rendering), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_shape_rendering>());

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

    rust_style_group_register_field_descriptors(descriptors.data(), descriptors.size());
}

// Groups whose layout is defined in Rust must not change size or alignment when the C++
// side layers initial values and accessors on top of the mirrored layout.
static_assert(sizeof(ComputedValues::InheritedBoxValues) == sizeof(ComputedValuesFFI::InheritedBoxValues));
static_assert(alignof(ComputedValues::InheritedBoxValues) == alignof(ComputedValuesFFI::InheritedBoxValues));
static_assert(sizeof(ComputedValues::InheritedTableValues) == sizeof(ComputedValuesFFI::InheritedTableValues));
static_assert(alignof(ComputedValues::InheritedTableValues) == alignof(ComputedValuesFFI::InheritedTableValues));

void const* style_group_default_payload(size_t group_index)
{
    static auto const default_payloads = [] {
        constexpr auto group_count = to_underlying(StyleGroupIndex::Count);
        Array<ComputedValuesFFI::StyleGroupVTable, group_count> vtables;
#define LIBWEB_STYLE_GROUP_VTABLE(name) vtables[to_underlying(StyleGroupIndex::name)] = make_style_group_vtable<ComputedValues::name>();
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_VTABLE)
#undef LIBWEB_STYLE_GROUP_VTABLE
        Array<void const*, group_count> payloads {};
        ComputedValuesFFI::rust_style_group_registry_register(vtables.data(), vtables.size(), payloads.data());
        register_style_group_field_descriptors();
        return payloads;
    }();
    return default_payloads[group_index];
}

// The default group payloads must match what create() produces for a completely
// unstyled element, or every element clones these groups instead of sharing the
// leaked default payload. Groups whose initial computed values are not simply
// value-initialized members build their defaults from the property initial
// values here, exactly as create() would.

// create() appends the elements of comma-separated lists, so the seeded default
// must hold the list element, not the list value itself.
static NonnullRefPtr<StyleValue const> initial_list_element(PropertyID property_id)
{
    auto value = property_initial_value(property_id);
    if (value->is_value_list())
        return value->as_value_list().values().first();
    return value;
}

ComputedValues::AnimationValues ComputedValues::AnimationValues::make_default_payload_value()
{
    AnimationValues values;
    values.animation_timing_function_style_values = { initial_list_element(PropertyID::AnimationTimingFunction) };
    values.transition_timing_function_style_values = { initial_list_element(PropertyID::TransitionTimingFunction) };
    return values;
}

ComputedValues::MaskValues ComputedValues::MaskValues::make_default_payload_value()
{
    MaskValues values;
    VERIFY(values.mask_layers.size() == 1);
    values.mask_layers[0].image_style_value = initial_list_element(PropertyID::MaskImage);
    // NB: The computed initial mask-position offsets are percentages, not lengths.
    values.mask_positions = { Position { .offset_x = Percentage(0), .offset_y = Percentage(0) } };
    return values;
}

ComputedValues::InheritedSVGValues ComputedValues::InheritedSVGValues::make_default_payload_value()
{
    InheritedSVGValues values;
    // NB: The initial fill is black, which create() materializes as an SVGPaint.
    values.fill = SVGPaint { Color::Black };
    return values;
}

ComputedValues::GridValues ComputedValues::GridValues::make_default_payload_value()
{
    GridValues values;
    values.grid_auto_columns = property_initial_value(PropertyID::GridAutoColumns)->as_grid_track_size_list().grid_track_size_list();
    values.grid_auto_rows = property_initial_value(PropertyID::GridAutoRows)->as_grid_track_size_list().grid_track_size_list();
    values.grid_template_columns = property_initial_value(PropertyID::GridTemplateColumns)->as_grid_track_size_list().grid_track_size_list();
    values.grid_template_rows = property_initial_value(PropertyID::GridTemplateRows)->as_grid_track_size_list().grid_track_size_list();
    values.grid_column_start = property_initial_value(PropertyID::GridColumnStart)->as_grid_track_placement().grid_track_placement();
    values.grid_column_end = property_initial_value(PropertyID::GridColumnEnd)->as_grid_track_placement().grid_track_placement();
    values.grid_row_start = property_initial_value(PropertyID::GridRowStart)->as_grid_track_placement().grid_track_placement();
    values.grid_row_end = property_initial_value(PropertyID::GridRowEnd)->as_grid_track_placement().grid_track_placement();
    return values;
}

ComputedValues::TransformValues ComputedValues::TransformValues::make_default_payload_value()
{
    TransformValues values;
    // NB: The computed transform-origin z component is a length, not a percentage.
    values.transform_origin.z = Length::make_px(0);
    return values;
}

bool ComputedValues::FontValues::operator==(FontValues const& other) const
{
    if (font_list != other.font_list) {
        if (!font_list || !other.font_list || !font_list->equals(*other.font_list))
            return false;
    }
    if (font_variation_settings.size() != other.font_variation_settings.size())
        return false;
    for (auto const& entry : font_variation_settings) {
        auto it = other.font_variation_settings.find(entry.key);
        if (it == other.font_variation_settings.end() || it->value != entry.value)
            return false;
    }
    return font_size == other.font_size
        && font_families == other.font_families
        && font_weight == other.font_weight
        && font_width == other.font_width
        && font_style == other.font_style
        && font_optical_sizing == other.font_optical_sizing
        && font_feature_data == other.font_feature_data
        && font_language_override == other.font_language_override
        && line_height == other.line_height
        && font_variant_emoji == other.font_variant_emoji
        && math_shift == other.math_shift
        && math_style == other.math_style
        && math_depth == other.math_depth;
}

bool ComputedValues::adopt_identical_group_payloads(ComputedValues const& previous) const
{
    bool all_shared = true;
    auto adopt = [&]<typename T>(StyleStructRef<T> const& mine, StyleStructRef<T> const& theirs) {
        if (mine.ptr_equals(theirs))
            return;
        if (mine == theirs) {
            const_cast<StyleStructRef<T>&>(mine) = theirs;
            return;
        }
        all_shared = false;
    };
#define LIBWEB_ADOPT_STYLE_GROUP(path) adopt(path, previous.path);
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.table)
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.list)
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.ui)
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.svg)
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.text)
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.box)
    LIBWEB_ADOPT_STYLE_GROUP(m_inherited.font)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.animation)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.box)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.surround)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.sizing)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.misc)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.alignment)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.border)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.background)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.transform)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.effects)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.mask_data)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.text_reset)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.content_data)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.anchor)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.grid)
    LIBWEB_ADOPT_STYLE_GROUP(m_noninherited.svg_reset)
#undef LIBWEB_ADOPT_STYLE_GROUP
    return all_shared;
}

NonnullRefPtr<ComputedValues const> ComputedValues::create(ComputedProperties const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const* inherit_parent)
{
    Builder builder;
    auto& computed_values = *builder.operator->();

    // The inherited box group builds on the Rust side from the computed keyword
    // values, sharing the parent's payload or the default payload when it can. A
    // null result means a value the core cannot map, and the setters below apply.
    auto* inherited_box_payload = ComputedValuesFFI::rust_build_inherited_box_group(
        InheritedBoxValues::style_group_index,
        computed_style.property(PropertyID::Visibility).rust_style_value_data(),
        computed_style.property(PropertyID::Direction).rust_style_value_data(),
        computed_style.property(PropertyID::WritingMode).rust_style_value_data(),
        computed_style.property(PropertyID::ContentVisibility).rust_style_value_data(),
        computed_style.property(PropertyID::ImageRendering).rust_style_value_data(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_inherited.box.operator->()) : nullptr);
    bool const inherited_box_adopted = inherited_box_payload != nullptr;
    if (inherited_box_adopted)
        computed_values.adopt_inherited_box_group(const_cast<void*>(inherited_box_payload));

    auto* inherited_table_payload = ComputedValuesFFI::rust_build_inherited_table_group(
        InheritedTableValues::style_group_index,
        computed_style.property(PropertyID::BorderCollapse).rust_style_value_data(),
        computed_style.property(PropertyID::CaptionSide).rust_style_value_data(),
        computed_style.property(PropertyID::EmptyCells).rust_style_value_data(),
        computed_style.property(PropertyID::BorderSpacing).rust_style_value_data(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_inherited.table.operator->()) : nullptr);
    bool const inherited_table_adopted = inherited_table_payload != nullptr;
    if (inherited_table_adopted)
        computed_values.adopt_inherited_table_group(const_cast<void*>(inherited_table_payload));

    auto gather_group_values = [&]<size_t N>(Array<PropertyID, N> const& properties, Array<ComputedValuesFFI::FfiGroupValueEntry, N>& entries) {
        for (size_t i = 0; i < N; ++i) {
            auto const& value = computed_style.property(properties[i]);
            entries[i] = { &value, value.rust_style_value_data(), 0, false, 0, false };
        }
    };

    Array<ComputedValuesFFI::FfiGroupValueEntry, alignment_group_properties.size()> alignment_group_values;
    gather_group_values(alignment_group_properties, alignment_group_values);
    auto* alignment_payload = ComputedValuesFFI::rust_build_style_group(
        AlignmentValues::style_group_index,
        alignment_group_values.data(),
        alignment_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.alignment.operator->()) : nullptr);
    bool const alignment_adopted = alignment_payload != nullptr;
    if (alignment_adopted)
        computed_values.adopt_alignment_group(const_cast<void*>(alignment_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, sizing_group_properties.size()> sizing_group_values;
    gather_group_values(sizing_group_properties, sizing_group_values);
    auto* sizing_payload = ComputedValuesFFI::rust_build_style_group(
        SizingValues::style_group_index,
        sizing_group_values.data(),
        sizing_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.sizing.operator->()) : nullptr);
    bool const sizing_adopted = sizing_payload != nullptr;
    if (sizing_adopted)
        computed_values.adopt_sizing_group(const_cast<void*>(sizing_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, grid_group_properties.size()> grid_group_values;
    gather_group_values(grid_group_properties, grid_group_values);
    auto* grid_payload = ComputedValuesFFI::rust_build_style_group(
        GridValues::style_group_index,
        grid_group_values.data(),
        grid_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.grid.operator->()) : nullptr);
    bool const grid_adopted = grid_payload != nullptr;
    if (grid_adopted)
        computed_values.adopt_grid_group(const_cast<void*>(grid_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, mask_group_properties.size()> mask_group_values;
    gather_group_values(mask_group_properties, mask_group_values);
    auto* mask_payload = ComputedValuesFFI::rust_build_style_group(
        MaskValues::style_group_index,
        mask_group_values.data(),
        mask_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.mask_data.operator->()) : nullptr);
    bool const mask_adopted = mask_payload != nullptr;
    if (mask_adopted)
        computed_values.adopt_mask_group(const_cast<void*>(mask_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, transform_group_properties.size()> transform_group_values;
    gather_group_values(transform_group_properties, transform_group_values);
    auto* transform_payload = ComputedValuesFFI::rust_build_style_group(
        TransformValues::style_group_index,
        transform_group_values.data(),
        transform_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.transform.operator->()) : nullptr);
    bool const transform_adopted = transform_payload != nullptr;
    if (transform_adopted)
        computed_values.adopt_transform_group(const_cast<void*>(transform_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, effects_group_properties.size()> effects_group_values;
    gather_group_values(effects_group_properties, effects_group_values);
    for (size_t i = 0; i < effects_group_properties.size(); ++i) {
        if (effects_group_properties[i] == PropertyID::Opacity) {
            effects_group_values[i].resolved_number = computed_style.opacity();
            effects_group_values[i].has_resolved_number = true;
        }
    }
    auto* effects_payload = ComputedValuesFFI::rust_build_style_group(
        EffectsValues::style_group_index,
        effects_group_values.data(),
        effects_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.effects.operator->()) : nullptr);
    bool const effects_adopted = effects_payload != nullptr;
    if (effects_adopted)
        computed_values.adopt_effects_group(const_cast<void*>(effects_payload));

    auto custom_ident_list = [&](PropertyID property_id) {
        Vector<Utf16FlyString> names;
        auto append_name = [&](StyleValue const& value) {
            if (value.is_custom_ident())
                names.append(value.as_custom_ident().custom_ident());
        };
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append_name(item);
        } else {
            append_name(value);
        }
        return names;
    };
    auto for_each_comma_separated_value = [&](CSS::PropertyID property_id, auto callback) {
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list() && value.as_value_list().separator() == CSS::StyleValueList::Separator::Comma) {
            for (auto const& item : value.as_value_list().values())
                callback(item);
        } else {
            callback(value);
        }
    };

    // NOTE: color-scheme must be set first to ensure system colors can be resolved correctly.
    auto const& color_scheme_style_value = computed_style.property(PropertyID::ColorScheme).as_color_scheme();
    auto color_scheme = computed_style.color_scheme(document.page().preferred_color_scheme(), document.supported_color_schemes());
    color_resolution_context.color_scheme = color_scheme;

    computed_values.set_anchor_names(custom_ident_list(PropertyID::AnchorName));
    auto const& anchor_scope = computed_style.property(PropertyID::AnchorScope);
    computed_values.set_anchor_scope(AnchorScopeData {
        .all = anchor_scope.to_keyword() == Keyword::All,
        .names = custom_ident_list(PropertyID::AnchorScope),
    });

    // NOTE: We have to be careful that font-related properties get set in the right order.
    //       m_font is used by Length::to_px() when resolving sizes against this layout node.
    //       That's why it has to be set before everything else.
    computed_values.set_font_list(computed_style.computed_font_list(document.font_computer()));
    Vector<ComputedFontFamily> font_families;
    for (auto const& family : computed_style.property(PropertyID::FontFamily).as_value_list().values()) {
        if (family->is_keyword()) {
            font_families.append(keyword_to_generic_font_family(family->to_keyword()).value());
        } else {
            font_families.append(ComputedFontFamilyName {
                .name = string_from_style_value(family),
                .syntax = family->is_string() ? ComputedFontFamilySyntax::String : ComputedFontFamilySyntax::CustomIdent,
            });
        }
    }
    computed_values.set_font_families(move(font_families));
    computed_values.set_font_size(computed_style.font_size());
    computed_values.set_font_weight(computed_style.font_weight());
    computed_values.set_font_width(computed_style.font_width());
    auto const& font_style = computed_style.property(PropertyID::FontStyle).as_font_style();
    Optional<Variant<Angle, NonnullRefPtr<CalculatedStyleValue const>>> font_style_angle;
    if (font_style.angle()) {
        if (font_style.angle()->is_angle())
            font_style_angle = font_style.angle()->as_angle().angle();
        else
            font_style_angle = NonnullRefPtr { font_style.angle()->as_calculated() };
    }
    computed_values.set_font_style({ font_style.font_style(), move(font_style_angle) });
    computed_values.set_font_optical_sizing(computed_style.font_optical_sizing());
    computed_values.set_font_feature_data(computed_style.font_feature_data());
    computed_values.set_line_height(computed_style.line_height_data(document.font_computer()));
    computed_values.set_font_variant_emoji(computed_style.font_variant_emoji());

    Array<ComputedValuesFFI::FfiGroupValueEntry, animation_group_properties.size()> animation_group_values;
    gather_group_values(animation_group_properties, animation_group_values);
    auto* animation_payload = ComputedValuesFFI::rust_build_style_group(
        AnimationValues::style_group_index,
        animation_group_values.data(),
        animation_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.animation.operator->()) : nullptr);
    bool const animation_adopted = animation_payload != nullptr;
    if (animation_adopted)
        computed_values.adopt_animation_group(const_cast<void*>(animation_payload));

    Vector<ComputedAnimationName> animation_names;
    for (auto const& name : computed_style.property(PropertyID::AnimationName).as_value_list().values()) {
        if (name->to_keyword() == Keyword::None) {
            animation_names.empend();
        } else {
            animation_names.append(ComputedAnimationName {
                .name = string_from_style_value(name),
                .syntax = name->is_string() ? ComputedAnimationNameSyntax::String : ComputedAnimationNameSyntax::CustomIdent,
            });
        }
    }
    if (!animation_adopted)
        computed_values.set_animation_names(move(animation_names));
    Vector<AnimationComposition> animation_compositions;
    for_each_comma_separated_value(PropertyID::AnimationComposition, [&](StyleValue const& value) { animation_compositions.append(keyword_to_animation_composition(value.to_keyword()).release_value()); });
    if (!animation_adopted)
        computed_values.set_animation_compositions(move(animation_compositions));
    Vector<Time> animation_delays;
    for_each_comma_separated_value(PropertyID::AnimationDelay, [&](StyleValue const& value) { animation_delays.append(Time::from_style_value(NonnullRefPtr<StyleValue const> { value }, {})); });
    if (!animation_adopted)
        computed_values.set_animation_delays(move(animation_delays));
    Vector<AnimationDirection> animation_directions;
    for_each_comma_separated_value(PropertyID::AnimationDirection, [&](StyleValue const& value) { animation_directions.append(keyword_to_animation_direction(value.to_keyword()).release_value()); });
    if (!animation_adopted)
        computed_values.set_animation_directions(move(animation_directions));
    Vector<Optional<Time>> animation_durations;
    for_each_comma_separated_value(PropertyID::AnimationDuration, [&](StyleValue const& value) {
        animation_durations.append(value.to_keyword() == Keyword::Auto ? Optional<Time> {} : Time::from_style_value(NonnullRefPtr<StyleValue const> { value }, {}));
    });
    if (!animation_adopted)
        computed_values.set_animation_durations(move(animation_durations));
    Vector<AnimationFillMode> animation_fill_modes;
    for_each_comma_separated_value(PropertyID::AnimationFillMode, [&](StyleValue const& value) { animation_fill_modes.append(keyword_to_animation_fill_mode(value.to_keyword()).release_value()); });
    if (!animation_adopted)
        computed_values.set_animation_fill_modes(move(animation_fill_modes));
    Vector<double> animation_iteration_counts;
    for_each_comma_separated_value(PropertyID::AnimationIterationCount, [&](StyleValue const& value) {
        animation_iteration_counts.append(value.to_keyword() == Keyword::Infinite ? AK::Infinity<double> : number_from_style_value(value, {}));
    });
    if (!animation_adopted)
        computed_values.set_animation_iteration_counts(move(animation_iteration_counts));
    Vector<AnimationPlayState> animation_play_states;
    for_each_comma_separated_value(PropertyID::AnimationPlayState, [&](StyleValue const& value) { animation_play_states.append(keyword_to_animation_play_state(value.to_keyword()).release_value()); });
    if (!animation_adopted)
        computed_values.set_animation_play_states(move(animation_play_states));
    Vector<AnimationTimelineData> animation_timelines;
    for_each_comma_separated_value(PropertyID::AnimationTimeline, [&](StyleValue const& value) {
        AnimationTimelineData timeline;
        if (value.to_keyword() == Keyword::Auto) {
            timeline.type = AnimationTimelineData::Type::Auto;
        } else if (value.to_keyword() == Keyword::None) {
            timeline.type = AnimationTimelineData::Type::None;
        } else if (value.is_custom_ident()) {
            timeline.type = AnimationTimelineData::Type::Name;
            timeline.name = value.as_custom_ident().custom_ident();
        } else {
            VERIFY(value.is_function());
            auto const& function = value.as_function();
            auto const& arguments = function.value()->as_tuple().tuple();
            if (function.name() == "scroll"_utf16_fly_string) {
                timeline.type = AnimationTimelineData::Type::Scroll;
                if (arguments[TupleStyleValue::Indices::ScrollFunction::Scroller])
                    timeline.scroller = keyword_to_scroller(arguments[TupleStyleValue::Indices::ScrollFunction::Scroller]->to_keyword()).release_value();
                if (arguments[TupleStyleValue::Indices::ScrollFunction::Axis])
                    timeline.axis = keyword_to_axis(arguments[TupleStyleValue::Indices::ScrollFunction::Axis]->to_keyword()).release_value();
            } else {
                VERIFY(function.name() == "view"_utf16_fly_string);
                timeline.type = AnimationTimelineData::Type::View;
                if (arguments[TupleStyleValue::Indices::ViewFunction::Axis])
                    timeline.axis = keyword_to_axis(arguments[TupleStyleValue::Indices::ViewFunction::Axis]->to_keyword()).release_value();
                if (auto const& inset = arguments[TupleStyleValue::Indices::ViewFunction::Inset]) {
                    VERIFY(inset->is_value_list());
                    auto const& values = inset->as_value_list().values();
                    VERIFY(values.size() == 2);
                    timeline.inset = {
                        .start = LengthPercentageOrAuto::from_style_value(values[0]),
                        .end = LengthPercentageOrAuto::from_style_value(values[1]),
                    };
                }
            }
        }
        animation_timelines.append(move(timeline));
    });
    if (!animation_adopted)
        computed_values.set_animation_timelines(move(animation_timelines));
    Vector<EasingFunction> animation_timing_functions;
    StyleValueVector animation_timing_function_style_values;
    for_each_comma_separated_value(PropertyID::AnimationTimingFunction, [&](StyleValue const& value) {
        animation_timing_functions.append(EasingFunction::from_style_value(value));
        animation_timing_function_style_values.append(value);
    });
    if (!animation_adopted)
        computed_values.set_animation_timing_functions(move(animation_timing_functions));
    if (!animation_adopted)
        computed_values.set_animation_timing_function_style_values(move(animation_timing_function_style_values));

    // NOTE: color must be set after color-scheme to ensure currentColor can be resolved in other properties (e.g. background-color).
    // NOTE: color must be set after font_size as `CalculatedStyleValue`s can rely on it being set for resolving lengths.
    auto color = computed_style.color(CSS::PropertyID::Color, color_resolution_context);
    // NB: The inherited text group resolves its color fields against the element's
    //     own color, which reaches the shared resolution context only further down.
    auto own_color_resolution_context = color_resolution_context;
    own_color_resolution_context.current_color = color;
    Array<ComputedValuesFFI::FfiGroupValueEntry, inherited_text_group_properties.size()> inherited_text_group_values;
    gather_group_values(inherited_text_group_properties, inherited_text_group_values);
    for (size_t i = 0; i < inherited_text_group_properties.size(); ++i) {
        auto gather_property_id = inherited_text_group_properties[i];
        if (gather_property_id == PropertyID::Color) {
            inherited_text_group_values[i].resolved_color = color.value();
            inherited_text_group_values[i].has_resolved_color = true;
        } else if (gather_property_id == PropertyID::WebkitTextFillColor) {
            if (auto resolved = computed_style.property(PropertyID::WebkitTextFillColor).to_color(own_color_resolution_context); resolved.has_value()) {
                inherited_text_group_values[i].resolved_color = resolved->value();
                inherited_text_group_values[i].has_resolved_color = true;
            }
        }
    }
    Array<ComputedValuesFFI::FfiGroupValueEntry, inherited_ui_group_properties.size()> inherited_ui_group_values;
    gather_group_values(inherited_ui_group_properties, inherited_ui_group_values);
    for (size_t i = 0; i < inherited_ui_group_properties.size(); ++i) {
        auto ui_property_id = inherited_ui_group_properties[i];
        if (ui_property_id == PropertyID::CaretColor) {
            inherited_ui_group_values[i].resolved_color = computed_style.caret_color(own_color_resolution_context).value();
            inherited_ui_group_values[i].has_resolved_color = true;
        } else if (ui_property_id == PropertyID::AccentColor) {
            inherited_ui_group_values[i].resolved_color = computed_style.accent_color(own_color_resolution_context).value();
            inherited_ui_group_values[i].has_resolved_color = true;
        } else if (ui_property_id == PropertyID::ColorScheme) {
            inherited_ui_group_values[i].resolved_number = static_cast<double>(to_underlying(color_scheme));
            inherited_ui_group_values[i].has_resolved_number = true;
        }
    }
    Array<ComputedValuesFFI::FfiGroupValueEntry, inherited_svg_group_properties.size()> inherited_svg_group_values;
    gather_group_values(inherited_svg_group_properties, inherited_svg_group_values);
    for (size_t i = 0; i < inherited_svg_group_properties.size(); ++i) {
        auto svg_property_id = inherited_svg_group_properties[i];
        if (svg_property_id == PropertyID::FillOpacity) {
            inherited_svg_group_values[i].resolved_number = computed_style.fill_opacity();
            inherited_svg_group_values[i].has_resolved_number = true;
        } else if (svg_property_id == PropertyID::StrokeOpacity) {
            inherited_svg_group_values[i].resolved_number = computed_style.stroke_opacity();
            inherited_svg_group_values[i].has_resolved_number = true;
        }
    }
    auto* inherited_svg_payload = ComputedValuesFFI::rust_build_style_group(
        InheritedSVGValues::style_group_index,
        inherited_svg_group_values.data(),
        inherited_svg_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_inherited.svg.operator->()) : nullptr);
    bool const inherited_svg_adopted = inherited_svg_payload != nullptr;
    if (inherited_svg_adopted)
        computed_values.adopt_inherited_svg_group(const_cast<void*>(inherited_svg_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, svg_reset_group_properties.size()> svg_reset_group_values;
    gather_group_values(svg_reset_group_properties, svg_reset_group_values);
    for (size_t i = 0; i < svg_reset_group_properties.size(); ++i) {
        auto svg_property_id = svg_reset_group_properties[i];
        if (svg_property_id == PropertyID::StopColor || svg_property_id == PropertyID::FloodColor) {
            svg_reset_group_values[i].resolved_color = computed_style.color(svg_property_id, own_color_resolution_context).value();
            svg_reset_group_values[i].has_resolved_color = true;
        } else if (svg_property_id == PropertyID::StopOpacity) {
            svg_reset_group_values[i].resolved_number = computed_style.stop_opacity();
            svg_reset_group_values[i].has_resolved_number = true;
        } else if (svg_property_id == PropertyID::FloodOpacity) {
            svg_reset_group_values[i].resolved_number = computed_style.flood_opacity();
            svg_reset_group_values[i].has_resolved_number = true;
        }
    }
    auto* svg_reset_payload = ComputedValuesFFI::rust_build_style_group(
        SVGResetValues::style_group_index,
        svg_reset_group_values.data(),
        svg_reset_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.svg_reset.operator->()) : nullptr);
    bool const svg_reset_adopted = svg_reset_payload != nullptr;
    if (svg_reset_adopted)
        computed_values.adopt_svg_reset_group(const_cast<void*>(svg_reset_payload));

    auto* inherited_ui_payload = ComputedValuesFFI::rust_build_style_group(
        InheritedUIValues::style_group_index,
        inherited_ui_group_values.data(),
        inherited_ui_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_inherited.ui.operator->()) : nullptr);
    bool const inherited_ui_adopted = inherited_ui_payload != nullptr;
    if (inherited_ui_adopted) {
        computed_values.adopt_inherited_ui_group(const_cast<void*>(inherited_ui_payload));
    } else {
        // NB: Nothing in this function reads the group's color-scheme fields; the
        //     resolution context carries its own copy, so setting them here rather
        //     than first is equivalent.
        computed_values.set_color_schemes(color_scheme_style_value.schemes(), color_scheme_style_value.only());
        computed_values.set_color_scheme(color_scheme);
    }

    auto* inherited_text_payload = ComputedValuesFFI::rust_build_style_group(
        InheritedTextValues::style_group_index,
        inherited_text_group_values.data(),
        inherited_text_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_inherited.text.operator->()) : nullptr);
    bool const inherited_text_adopted = inherited_text_payload != nullptr;
    if (inherited_text_adopted)
        computed_values.adopt_inherited_text_group(const_cast<void*>(inherited_text_payload));

    if (!inherited_text_adopted)
        computed_values.set_color(color);
    if (!inherited_text_adopted)
        computed_values.set_color_style_value(&computed_style.property(CSS::PropertyID::Color));

    // NOTE: This color resolution context must be created after we set color above so that currentColor resolves correctly
    // FIXME: We should resolve colors to their absolute forms at compute time (i.e. by implementing the relevant absolutized methods)
    color_resolution_context.current_color = color;

    // NB: The text reset group builds only after the element's color lands in the
    //     resolution context above, since text-decoration-color may be currentcolor.
    Array<ComputedValuesFFI::FfiGroupValueEntry, text_reset_group_properties.size()> text_reset_group_values;
    gather_group_values(text_reset_group_properties, text_reset_group_values);
    for (size_t i = 0; i < text_reset_group_properties.size(); ++i) {
        if (text_reset_group_properties[i] == PropertyID::TextDecorationColor) {
            text_reset_group_values[i].resolved_color = computed_style.color(PropertyID::TextDecorationColor, color_resolution_context).value();
            text_reset_group_values[i].has_resolved_color = true;
        }
    }
    auto* text_reset_payload = ComputedValuesFFI::rust_build_style_group(
        TextResetValues::style_group_index,
        text_reset_group_values.data(),
        text_reset_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.text_reset.operator->()) : nullptr);
    bool const text_reset_adopted = text_reset_payload != nullptr;
    if (text_reset_adopted)
        computed_values.adopt_text_reset_group(const_cast<void*>(text_reset_payload));

    Array<ComputedValuesFFI::FfiGroupValueEntry, misc_reset_group_properties.size()> misc_reset_group_values;
    gather_group_values(misc_reset_group_properties, misc_reset_group_values);
    for (size_t i = 0; i < misc_reset_group_properties.size(); ++i) {
        if (misc_reset_group_properties[i] == PropertyID::OutlineColor) {
            if (auto const& outline_color = computed_style.property(PropertyID::OutlineColor); outline_color.has_color()) {
                if (auto resolved = outline_color.to_color(color_resolution_context); resolved.has_value()) {
                    misc_reset_group_values[i].resolved_color = resolved->value();
                    misc_reset_group_values[i].has_resolved_color = true;
                }
            }
        } else if (misc_reset_group_properties[i] == PropertyID::ShapeImageThreshold) {
            misc_reset_group_values[i].resolved_number = computed_style.property(PropertyID::ShapeImageThreshold).as_opacity_value().resolved();
            misc_reset_group_values[i].has_resolved_number = true;
        }
    }
    auto* misc_reset_payload = ComputedValuesFFI::rust_build_style_group(
        MiscResetValues::style_group_index,
        misc_reset_group_values.data(),
        misc_reset_group_values.size(),
        inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.misc.operator->()) : nullptr);
    bool const misc_reset_adopted = misc_reset_payload != nullptr;
    if (misc_reset_adopted)
        computed_values.adopt_misc_reset_group(const_cast<void*>(misc_reset_payload));

    if (!inherited_ui_adopted) {
        auto const& accent_color_value = computed_style.property(CSS::PropertyID::AccentColor);
        CSS::ColorOrAuto accent_color;
        accent_color.used_value = computed_style.accent_color(color_resolution_context);
        if (accent_color_value.to_keyword() != CSS::Keyword::Auto)
            accent_color.computed_value = accent_color.used_value;
        computed_values.set_accent_color(move(accent_color));
    }

    computed_values.set_vertical_align(computed_style.vertical_align());

    auto background_layers = computed_style.background_layers();
    computed_values.set_background_layers(move(background_layers));

    auto mask_layers = computed_style.mask_layers();
    if (!mask_adopted)
        computed_values.set_mask_layers(move(mask_layers));
    Vector<CSS::Position> mask_positions;
    for_each_comma_separated_value(CSS::PropertyID::MaskPosition, [&](CSS::StyleValue const& value) {
        auto const& position = value.as_position();
        mask_positions.append({
            .offset_x = CSS::LengthPercentage::from_style_value(position.edge_x()->as_edge().offset()),
            .offset_y = CSS::LengthPercentage::from_style_value(position.edge_y()->as_edge().offset()),
        });
    });
    if (!mask_adopted)
        computed_values.set_mask_positions(move(mask_positions));

    auto border_image = computed_style.border_image();
    computed_values.set_border_image(move(border_image));

    auto const& content = computed_style.property(CSS::PropertyID::Content);
    ComputedContentData computed_content;
    if (content.to_keyword() == Keyword::None) {
        computed_content.type = ComputedContentData::Type::None;
    } else if (content.is_content()) {
        computed_content.type = ComputedContentData::Type::List;
        auto append_item = [](StyleValue const& item, Vector<ComputedContentItem>& items) {
            if (item.is_string()) {
                items.append(item.as_string().string_value().to_utf16_string());
            } else if (item.is_keyword()) {
                items.append(item.to_keyword());
            } else if (item.is_counter()) {
                auto const& counter = item.as_counter();
                ComputedContentCounter computed_counter {
                    .function = counter.function_type() == CounterStyleValue::CounterFunction::Counters ? ComputedContentCounter::Function::Counters : ComputedContentCounter::Function::Counter,
                    .name = counter.counter_name(),
                    .join_string = counter.join_string(),
                    .style = counter.counter_style()->as_counter_style().value().visit(
                        [](Utf16FlyString const& name) -> Variant<Utf16FlyString, ComputedContentCounter::SymbolsFunction> { return name; },
                        [](CounterStyleStyleValue::SymbolsFunction const& symbols) -> Variant<Utf16FlyString, ComputedContentCounter::SymbolsFunction> {
                            return ComputedContentCounter::SymbolsFunction { .type = symbols.type, .symbols = symbols.symbols };
                        }),
                };
                items.append(move(computed_counter));
            } else {
                VERIFY(item.is_abstract_image());
                items.append(NonnullRefPtr<AbstractImageStyleValue const> { item.as_abstract_image() });
            }
        };
        auto const& content_style_value = content.as_content();
        for (auto const& item : content_style_value.content().values())
            append_item(item, computed_content.items);
        if (auto const* alt_text = content_style_value.alt_text()) {
            for (auto const& item : alt_text->values())
                append_item(item, computed_content.alt_text);
        }
    }
    computed_values.set_computed_content(move(computed_content));

    computed_values.set_background_color(computed_style.color(CSS::PropertyID::BackgroundColor, color_resolution_context));
    computed_values.set_background_color_style_value(computed_style.property(CSS::PropertyID::BackgroundColor));
    computed_values.set_background_color_clip(computed_style.background_color_clip());

    computed_values.set_box_sizing(computed_style.box_sizing());

    if (auto maybe_font_language_override = computed_style.font_language_override(); maybe_font_language_override.has_value())
        computed_values.set_font_language_override(maybe_font_language_override.release_value());
    computed_values.set_font_variation_settings(computed_style.font_variation_settings());

    auto border_radius_data_from_style_value = [](CSS::StyleValue const& value) -> CSS::BorderRadiusData {
        return CSS::BorderRadiusData {
            CSS::LengthPercentage::from_style_value(value.as_border_radius().horizontal_radius()),
            CSS::LengthPercentage::from_style_value(value.as_border_radius().vertical_radius())
        };
    };

    computed_values.set_border_bottom_left_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderBottomLeftRadius)));
    computed_values.set_border_bottom_right_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderBottomRightRadius)));
    computed_values.set_border_top_left_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderTopLeftRadius)));
    computed_values.set_border_top_right_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderTopRightRadius)));
    computed_values.set_corner_bottom_left_shape(computed_style.property(CSS::PropertyID::CornerBottomLeftShape).as_superellipse().parameter());
    computed_values.set_corner_bottom_right_shape(computed_style.property(CSS::PropertyID::CornerBottomRightShape).as_superellipse().parameter());
    computed_values.set_corner_top_left_shape(computed_style.property(CSS::PropertyID::CornerTopLeftShape).as_superellipse().parameter());
    computed_values.set_corner_top_right_shape(computed_style.property(CSS::PropertyID::CornerTopRightShape).as_superellipse().parameter());
    computed_values.set_display(computed_style.display());
    computed_values.set_display_before_box_type_transformation(computed_style.display_before_box_type_transformation());

    if (!alignment_adopted)
        computed_values.set_flex_direction(computed_style.flex_direction());
    if (!alignment_adopted)
        computed_values.set_flex_wrap(computed_style.flex_wrap());
    if (!alignment_adopted)
        computed_values.set_flex_basis(computed_style.flex_basis());
    if (!alignment_adopted)
        computed_values.set_flex_grow(computed_style.flex_grow());
    if (!alignment_adopted)
        computed_values.set_flex_shrink(computed_style.flex_shrink());
    if (!alignment_adopted)
        computed_values.set_order(computed_style.order());
    if (!effects_adopted)
        computed_values.set_clip(computed_style.clip());

    if (!effects_adopted)
        computed_values.set_backdrop_filter(computed_style.backdrop_filter());
    if (!effects_adopted)
        computed_values.set_filter(computed_style.filter());

    if (!svg_reset_adopted)
        computed_values.set_flood_color(computed_style.color(CSS::PropertyID::FloodColor, color_resolution_context));
    if (!svg_reset_adopted)
        computed_values.set_flood_opacity(computed_style.flood_opacity());

    if (!alignment_adopted)
        computed_values.set_justify_content(computed_style.justify_content());
    if (!alignment_adopted)
        computed_values.set_justify_items(computed_style.justify_items());
    if (!alignment_adopted)
        computed_values.set_justify_self(computed_style.justify_self());

    if (!alignment_adopted)
        computed_values.set_align_content(computed_style.align_content());
    if (!alignment_adopted)
        computed_values.set_align_items(computed_style.align_items());
    if (!alignment_adopted)
        computed_values.set_align_self(computed_style.align_self());

    if (!misc_reset_adopted)
        computed_values.set_appearance(computed_style.appearance());
    if (!misc_reset_adopted)
        computed_values.set_computed_appearance(keyword_to_appearance(computed_style.property(PropertyID::Appearance).to_keyword()).release_value());

    computed_values.set_position(computed_style.position());

    // https://drafts.csswg.org/css-anchor-position-1/#position-anchor
    auto const& position_anchor_value = computed_style.property(CSS::PropertyID::PositionAnchor);
    CSS::PositionAnchor position_anchor;
    if (position_anchor_value.is_custom_ident()) {
        position_anchor.type = CSS::PositionAnchor::Type::Name;
        position_anchor.name = position_anchor_value.as_custom_ident().custom_ident();
    } else {
        switch (position_anchor_value.to_keyword()) {
        case CSS::Keyword::Normal:
            position_anchor.type = CSS::PositionAnchor::Type::Normal;
            break;
        case CSS::Keyword::None:
            position_anchor.type = CSS::PositionAnchor::Type::None;
            break;
        case CSS::Keyword::Auto:
            position_anchor.type = CSS::PositionAnchor::Type::Auto;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    computed_values.set_position_anchor(move(position_anchor));

    auto position_area_from_style_value = [](CSS::StyleValue const& value) {
        CSS::PositionAreaData area;
        auto append_keyword = [&](CSS::StyleValue const& item) {
            if (auto keyword = CSS::keyword_to_position_area(item.to_keyword()); keyword.has_value())
                area.keywords.append(*keyword);
        };
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append_keyword(item);
        } else {
            append_keyword(value);
        }
        return area;
    };
    computed_values.set_position_area(position_area_from_style_value(computed_style.property(CSS::PropertyID::PositionArea)));

    Vector<CSS::PositionTryFallbackData> position_try_fallbacks;
    auto append_position_try_fallback = [&](CSS::StyleValue const& value) {
        CSS::PositionTryFallbackData fallback;
        auto apply_item = [&](CSS::StyleValue const& item) {
            if (item.is_custom_ident()) {
                fallback.name = item.as_custom_ident().custom_ident();
            } else if (auto tactic = CSS::keyword_to_try_tactic(item.to_keyword()); tactic.has_value()) {
                fallback.tactics.append(*tactic);
            }
        };
        auto area = position_area_from_style_value(value);
        if (!area.keywords.is_empty()) {
            fallback.position_area = move(area);
        } else if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values()) {
                if (item->is_value_list()) {
                    for (auto const& child : item->as_value_list().values())
                        apply_item(child);
                } else {
                    apply_item(item);
                }
            }
        } else {
            apply_item(value);
        }
        position_try_fallbacks.append(move(fallback));
    };
    auto const& position_try_fallbacks_value = computed_style.property(CSS::PropertyID::PositionTryFallbacks);
    if (position_try_fallbacks_value.to_keyword() != CSS::Keyword::None) {
        if (position_try_fallbacks_value.is_value_list() && position_try_fallbacks_value.as_value_list().separator() == CSS::StyleValueList::Separator::Comma) {
            for (auto const& item : position_try_fallbacks_value.as_value_list().values())
                append_position_try_fallback(item);
        } else {
            append_position_try_fallback(position_try_fallbacks_value);
        }
    }
    computed_values.set_position_try_fallbacks(move(position_try_fallbacks));

    auto const& position_try_order = computed_style.property(CSS::PropertyID::PositionTryOrder);
    computed_values.set_position_try_order(position_try_order.to_keyword() == CSS::Keyword::Normal
            ? Optional<CSS::TryOrder> {}
            : CSS::keyword_to_try_order(position_try_order.to_keyword()));

    auto const& position_visibility = computed_style.property(CSS::PropertyID::PositionVisibility);
    CSS::PositionVisibilityData position_visibility_data {
        .always = position_visibility.to_keyword() == CSS::Keyword::Always,
        .anchors_visible = false,
    };
    auto apply_position_visibility_keyword = [&](CSS::Keyword keyword) {
        switch (keyword) {
        case CSS::Keyword::AnchorsValid:
            position_visibility_data.anchors_valid = true;
            break;
        case CSS::Keyword::AnchorsVisible:
            position_visibility_data.anchors_visible = true;
            break;
        case CSS::Keyword::NoOverflow:
            position_visibility_data.no_overflow = true;
            break;
        default:
            break;
        }
    };
    if (position_visibility.is_value_list()) {
        for (auto const& item : position_visibility.as_value_list().values())
            apply_position_visibility_keyword(item->to_keyword());
    } else {
        apply_position_visibility_keyword(position_visibility.to_keyword());
    }
    computed_values.set_position_visibility(position_visibility_data);

    auto timeline_names = [&](CSS::PropertyID property_id) {
        Vector<Optional<Utf16FlyString>> names;
        auto append = [&](CSS::StyleValue const& item) {
            names.append(item.is_custom_ident() ? Optional<Utf16FlyString> { item.as_custom_ident().custom_ident() } : Optional<Utf16FlyString> {});
        };
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append(item);
        } else {
            append(value);
        }
        return names;
    };
    auto timeline_axes = [&](CSS::PropertyID property_id) {
        Vector<CSS::Axis> axes;
        auto append = [&](CSS::StyleValue const& item) { axes.append(CSS::keyword_to_axis(item.to_keyword()).release_value()); };
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append(item);
        } else {
            append(value);
        }
        return axes;
    };
    if (!animation_adopted)
        computed_values.set_scroll_timeline_names(timeline_names(CSS::PropertyID::ScrollTimelineName));
    if (!animation_adopted)
        computed_values.set_scroll_timeline_axes(timeline_axes(CSS::PropertyID::ScrollTimelineAxis));
    if (!animation_adopted)
        computed_values.set_view_timeline_names(timeline_names(CSS::PropertyID::ViewTimelineName));
    if (!animation_adopted)
        computed_values.set_view_timeline_axes(timeline_axes(CSS::PropertyID::ViewTimelineAxis));

    if (!animation_adopted) {
        auto const& timeline_scope = computed_style.property(CSS::PropertyID::TimelineScope);
        computed_values.set_timeline_scope(CSS::TimelineScopeData {
            .all = timeline_scope.to_keyword() == CSS::Keyword::All,
            .names = custom_ident_list(CSS::PropertyID::TimelineScope),
        });
    }

    Vector<CSS::ViewTimelineInsetData> view_timeline_insets;
    auto append_view_timeline_inset = [&](CSS::StyleValue const& value) {
        VERIFY(value.is_value_list());
        auto const& values = value.as_value_list().values();
        VERIFY(values.size() == 2);
        view_timeline_insets.append({
            .start = CSS::LengthPercentageOrAuto::from_style_value(values[0]),
            .end = CSS::LengthPercentageOrAuto::from_style_value(values[1]),
        });
    };
    auto const& view_timeline_inset = computed_style.property(CSS::PropertyID::ViewTimelineInset);
    if (view_timeline_inset.as_value_list().separator() == CSS::StyleValueList::Separator::Comma) {
        for (auto const& item : view_timeline_inset.as_value_list().values())
            append_view_timeline_inset(item);
    } else {
        append_view_timeline_inset(view_timeline_inset);
    }
    if (!animation_adopted)
        computed_values.set_view_timeline_insets(move(view_timeline_insets));

    Vector<Optional<Utf16FlyString>> transition_properties;
    for_each_comma_separated_value(CSS::PropertyID::TransitionProperty, [&](CSS::StyleValue const& value) {
        transition_properties.append(value.is_custom_ident() ? Optional<Utf16FlyString> { value.as_custom_ident().custom_ident() } : Optional<Utf16FlyString> {});
    });
    if (!animation_adopted)
        computed_values.set_transition_properties(move(transition_properties));
    Vector<CSS::Time> transition_durations;
    for_each_comma_separated_value(CSS::PropertyID::TransitionDuration, [&](CSS::StyleValue const& value) {
        transition_durations.append(CSS::Time::from_style_value(NonnullRefPtr<CSS::StyleValue const> { value }, {}));
    });
    if (!animation_adopted)
        computed_values.set_transition_durations(move(transition_durations));
    Vector<CSS::EasingFunction> transition_timing_functions;
    CSS::StyleValueVector transition_timing_function_style_values;
    for_each_comma_separated_value(CSS::PropertyID::TransitionTimingFunction, [&](CSS::StyleValue const& value) {
        transition_timing_functions.append(CSS::EasingFunction::from_style_value(value));
        transition_timing_function_style_values.append(value);
    });
    if (!animation_adopted)
        computed_values.set_transition_timing_functions(move(transition_timing_functions));
    if (!animation_adopted)
        computed_values.set_transition_timing_function_style_values(move(transition_timing_function_style_values));
    Vector<CSS::Time> transition_delays;
    for_each_comma_separated_value(CSS::PropertyID::TransitionDelay, [&](CSS::StyleValue const& value) {
        transition_delays.append(CSS::Time::from_style_value(NonnullRefPtr<CSS::StyleValue const> { value }, {}));
    });
    if (!animation_adopted)
        computed_values.set_transition_delays(move(transition_delays));
    Vector<CSS::TransitionBehavior> transition_behaviors;
    for_each_comma_separated_value(CSS::PropertyID::TransitionBehavior, [&](CSS::StyleValue const& value) {
        transition_behaviors.append(CSS::keyword_to_transition_behavior(value.to_keyword()).release_value());
    });
    if (!animation_adopted)
        computed_values.set_transition_behaviors(move(transition_behaviors));

    if (!inherited_text_adopted)
        computed_values.set_text_align(computed_style.text_align());
    if (!inherited_text_adopted)
        computed_values.set_text_justify(computed_style.text_justify());
    computed_values.set_text_overflow(computed_style.text_overflow());
    auto const& text_underline_offset_value = computed_style.property(CSS::PropertyID::TextUnderlineOffset);
    CSS::TextUnderlineOffset text_underline_offset;
    text_underline_offset.used_value = computed_style.text_underline_offset();
    if (text_underline_offset_value.to_keyword() != CSS::Keyword::Auto)
        text_underline_offset.computed_value = CSS::LengthPercentage::from_style_value(text_underline_offset_value);
    if (!inherited_text_adopted)
        computed_values.set_text_underline_offset(move(text_underline_offset));
    if (!inherited_text_adopted)
        computed_values.set_text_underline_position(computed_style.text_underline_position());

    if (!inherited_text_adopted)
        computed_values.set_text_indent(computed_style.text_indent());
    if (!inherited_text_adopted)
        computed_values.set_text_wrap_mode(computed_style.text_wrap_mode());
    if (!inherited_text_adopted)
        computed_values.set_text_wrap_style(CSS::keyword_to_text_wrap_style(computed_style.property(CSS::PropertyID::TextWrapStyle).to_keyword()).release_value());
    if (!inherited_text_adopted)
        computed_values.set_tab_size(computed_style.tab_size());

    if (!inherited_text_adopted)
        computed_values.set_white_space_collapse(computed_style.white_space_collapse());
    if (!text_reset_adopted)
        computed_values.set_white_space_trim(computed_style.white_space_trim());
    if (!inherited_text_adopted)
        computed_values.set_word_break(computed_style.word_break());
    if (!inherited_text_adopted) {
        switch (computed_style.property(CSS::PropertyID::OverflowWrap).to_keyword()) {
        case CSS::Keyword::Normal:
            computed_values.set_overflow_wrap(CSS::OverflowWrap::Normal);
            break;
        case CSS::Keyword::BreakWord:
            computed_values.set_overflow_wrap(CSS::OverflowWrap::BreakWord);
            break;
        case CSS::Keyword::Anywhere:
            computed_values.set_overflow_wrap(CSS::OverflowWrap::Anywhere);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    auto integer_from_style_value = [](CSS::StyleValue const& value) -> u64 {
        i32 integer;
        if (value.is_integer())
            integer = value.as_integer().integer();
        else
            integer = value.as_calculated().resolve_integer({}).value();
        VERIFY(integer >= 0);
        return integer;
    };
    if (!inherited_text_adopted)
        computed_values.set_orphans(integer_from_style_value(computed_style.property(CSS::PropertyID::Orphans)));
    if (!inherited_text_adopted)
        computed_values.set_widows(integer_from_style_value(computed_style.property(CSS::PropertyID::Widows)));

    if (!inherited_text_adopted)
        computed_values.set_word_spacing(computed_style.word_spacing());
    if (!inherited_text_adopted)
        computed_values.set_letter_spacing(computed_style.letter_spacing());
    if (!inherited_text_adopted)
        computed_values.set_word_spacing_style_value(computed_style.property(PropertyID::WordSpacing));
    if (!inherited_text_adopted)
        computed_values.set_letter_spacing_style_value(computed_style.property(PropertyID::LetterSpacing));

    computed_values.set_float(computed_style.float_());

    if (!inherited_table_adopted) {
        computed_values.set_border_spacing_horizontal(computed_style.border_spacing_horizontal());
        computed_values.set_border_spacing_vertical(computed_style.border_spacing_vertical());
    }

    if (!inherited_table_adopted)
        computed_values.set_caption_side(computed_style.caption_side());
    computed_values.set_clear(computed_style.clear());
    computed_values.set_overflow_x(computed_style.overflow_x());
    computed_values.set_overflow_y(computed_style.overflow_y());
    if (!inherited_box_adopted)
        computed_values.set_content_visibility(computed_style.content_visibility());
    auto cursor = computed_style.cursor();
    if (!inherited_ui_adopted)
        computed_values.set_cursor(move(cursor));
    if (!inherited_box_adopted)
        computed_values.set_image_rendering(computed_style.image_rendering());
    if (!inherited_ui_adopted)
        computed_values.set_pointer_events(computed_style.pointer_events());
    if (!text_reset_adopted)
        computed_values.set_text_decoration_line(computed_style.text_decoration_line());
    if (!inherited_text_adopted)
        computed_values.set_text_decoration_skip_ink(computed_style.text_decoration_skip_ink());
    if (!text_reset_adopted)
        computed_values.set_text_decoration_style(computed_style.text_decoration_style());
    if (!inherited_text_adopted)
        computed_values.set_text_transform(computed_style.text_transform());

    auto list_style_type = computed_style.list_style_type(style_scope);
    auto const& list_style_type_value = computed_style.property(PropertyID::ListStyleType);
    if (list_style_type_value.is_counter_style() && list_style_type_value.as_counter_style().value().has<CounterStyleStyleValue::SymbolsFunction>()) {
        auto counter_style_value = list_style_type_value.as_counter_style().value();
        auto const& symbols = counter_style_value.get<CounterStyleStyleValue::SymbolsFunction>();
        auto counter_style = list_style_type.get<RefPtr<CounterStyle const>>();
        VERIFY(counter_style);
        list_style_type = ListStyleSymbols {
            .counter_style = counter_style.release_nonnull(),
            .type = symbols.type,
            .symbols = symbols.symbols,
        };
    }
    computed_values.set_list_style_type(move(list_style_type));
    computed_values.set_list_style_position(computed_style.list_style_position());
    auto const& list_style_image = computed_style.property(PropertyID::ListStyleImage);
    if (list_style_image.is_abstract_image())
        computed_values.set_list_style_image(list_style_image.as_abstract_image());

    if (!text_reset_adopted)
        computed_values.set_text_decoration_color(computed_style.color(CSS::PropertyID::TextDecorationColor, color_resolution_context));
    if (!text_reset_adopted)
        computed_values.set_text_decoration_thickness(computed_style.text_decoration_thickness());

    if (!inherited_text_adopted) {
        auto const& webkit_text_fill_color = computed_style.property(CSS::PropertyID::WebkitTextFillColor);
        computed_values.set_webkit_text_fill_color(
            webkit_text_fill_color.to_color(color_resolution_context).value(),
            webkit_text_fill_color.to_keyword() == Keyword::Currentcolor);
    }

    if (!inherited_text_adopted)
        computed_values.set_text_shadow(computed_style.text_shadow(color_resolution_context));

    computed_values.set_z_index(computed_style.z_index());
    if (!effects_adopted)
        computed_values.set_opacity(computed_style.opacity());

    if (!inherited_box_adopted)
        computed_values.set_visibility(computed_style.visibility());

    if (!sizing_adopted)
        computed_values.set_width(computed_style.size_value(CSS::PropertyID::Width));
    if (!sizing_adopted)
        computed_values.set_min_width(computed_style.size_value(CSS::PropertyID::MinWidth));
    if (!sizing_adopted)
        computed_values.set_max_width(computed_style.size_value(CSS::PropertyID::MaxWidth));

    if (!sizing_adopted)
        computed_values.set_height(computed_style.size_value(CSS::PropertyID::Height));
    if (!sizing_adopted)
        computed_values.set_min_height(computed_style.size_value(CSS::PropertyID::MinHeight));
    if (!sizing_adopted)
        computed_values.set_max_height(computed_style.size_value(CSS::PropertyID::MaxHeight));

    computed_values.set_inset(computed_style.length_box(CSS::PropertyID::Left, CSS::PropertyID::Top, CSS::PropertyID::Right, CSS::PropertyID::Bottom, CSS::LengthPercentageOrAuto::make_auto()));
    for (auto property_id : { PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left }) {
        auto const& inset = computed_style.property(property_id);
        if (inset.is_anchor())
            computed_values.set_anchor_inset(property_id, inset);
    }
    computed_values.set_margin(computed_style.length_box(CSS::PropertyID::MarginLeft, CSS::PropertyID::MarginTop, CSS::PropertyID::MarginRight, CSS::PropertyID::MarginBottom, CSS::Length::make_px(0)));
    computed_values.set_padding(computed_style.length_box(CSS::PropertyID::PaddingLeft, CSS::PropertyID::PaddingTop, CSS::PropertyID::PaddingRight, CSS::PropertyID::PaddingBottom, CSS::Length::make_px(0)));
    if (!misc_reset_adopted)
        computed_values.set_scroll_margin(computed_style.length_box(CSS::PropertyID::ScrollMarginLeft, CSS::PropertyID::ScrollMarginTop, CSS::PropertyID::ScrollMarginRight, CSS::PropertyID::ScrollMarginBottom, CSS::Length::make_px(0)));
    if (!misc_reset_adopted)
        computed_values.set_scroll_padding(computed_style.length_box(CSS::PropertyID::ScrollPaddingLeft, CSS::PropertyID::ScrollPaddingTop, CSS::PropertyID::ScrollPaddingRight, CSS::PropertyID::ScrollPaddingBottom, CSS::LengthPercentageOrAuto::make_auto()));
    if (!misc_reset_adopted) {
        auto extract_side = [&](CSS::PropertyID property_id) -> CSS::OverflowClipMarginSide {
            auto const& value = computed_style.property(property_id);
            if (value.is_overflow_clip_margin()) {
                auto const& overflow_clip_margin = value.as_overflow_clip_margin();
                CSSPixels offset = 0;
                if (overflow_clip_margin.offset().is_calculated())
                    offset = overflow_clip_margin.offset().as_calculated().resolve_length(color_resolution_context.calculation_resolution_context).value().absolute_length_to_px();
                else if (overflow_clip_margin.offset().is_length())
                    offset = overflow_clip_margin.offset().as_length().length().absolute_length_to_px();
                return { overflow_clip_margin.visual_box(), offset };
            }
            return {};
        };
        CSS::OverflowClipMarginData data;
        data.left = extract_side(CSS::PropertyID::OverflowClipMarginLeft);
        data.top = extract_side(CSS::PropertyID::OverflowClipMarginTop);
        data.right = extract_side(CSS::PropertyID::OverflowClipMarginRight);
        data.bottom = extract_side(CSS::PropertyID::OverflowClipMarginBottom);
        computed_values.set_overflow_clip_margin(data);
    }

    if (!effects_adopted)
        computed_values.set_box_shadow(computed_style.box_shadow(color_resolution_context));

    if (!transform_adopted)
        computed_values.set_rotate(computed_style.rotate());
    if (!transform_adopted)
        computed_values.set_translate(computed_style.translate());
    if (!transform_adopted)
        computed_values.set_scale(computed_style.scale());
    if (!transform_adopted)
        computed_values.set_transformations(computed_style.transformations());
    if (!transform_adopted)
        computed_values.set_transform_box(computed_style.transform_box());
    if (!transform_adopted)
        computed_values.set_transform_origin(computed_style.transform_origin());
    if (!transform_adopted)
        computed_values.set_transform_style(computed_style.transform_style());
    if (!transform_adopted)
        computed_values.set_perspective(computed_style.perspective());
    if (!transform_adopted)
        computed_values.set_perspective_origin(computed_style.perspective_origin());

    struct NamedBorderAndWidth {
        CSS::BorderData border;
        CSSPixels computed_width;
    };
    auto do_border_style = [&](CSS::BorderData border, CSS::PropertyID width_property, CSS::PropertyID color_property, CSS::PropertyID style_property) {
        // FIXME: Support <image-1d>
        border.color = computed_style.color(color_property, color_resolution_context);
        border.line_style = computed_style.line_style(style_property);
        // FIXME: Interpolation can cause negative values - we clamp here but should instead clamp as part of interpolation
        auto computed_width = max(CSSPixels { 0 }, computed_style.length(width_property).absolute_length_to_px());

        // If the border-style corresponding to a given border-width is none or hidden, then the used width is 0.
        // https://drafts.csswg.org/css-backgrounds/#border-width
        if (border.line_style == CSS::LineStyle::None || border.line_style == CSS::LineStyle::Hidden) {
            border.width = 0;
        } else {
            border.width = computed_width;
        }
        return NamedBorderAndWidth { border, computed_width };
    };

    auto left_border = do_border_style({}, CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor, CSS::PropertyID::BorderLeftStyle);
    computed_values.set_border_left(left_border.border);
    computed_values.set_border_left_computed_width(left_border.computed_width);
    auto top_border = do_border_style({}, CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor, CSS::PropertyID::BorderTopStyle);
    computed_values.set_border_top(top_border.border);
    computed_values.set_border_top_computed_width(top_border.computed_width);
    auto right_border = do_border_style({}, CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor, CSS::PropertyID::BorderRightStyle);
    computed_values.set_border_right(right_border.border);
    computed_values.set_border_right_computed_width(right_border.computed_width);
    auto bottom_border = do_border_style({}, CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor, CSS::PropertyID::BorderBottomStyle);
    computed_values.set_border_bottom(bottom_border.border);
    computed_values.set_border_bottom_computed_width(bottom_border.computed_width);
    computed_values.set_border_left_color_style_value(computed_style.property(CSS::PropertyID::BorderLeftColor));
    computed_values.set_border_top_color_style_value(computed_style.property(CSS::PropertyID::BorderTopColor));
    computed_values.set_border_right_color_style_value(computed_style.property(CSS::PropertyID::BorderRightColor));
    computed_values.set_border_bottom_color_style_value(computed_style.property(CSS::PropertyID::BorderBottomColor));

    if (auto const& outline_color = computed_style.property(CSS::PropertyID::OutlineColor); !misc_reset_adopted && outline_color.has_color())
        computed_values.set_outline_color(outline_color.to_color(color_resolution_context).value());
    auto const& outline_offset = computed_style.property(CSS::PropertyID::OutlineOffset);
    auto resolved_outline_offset = outline_offset.is_calculated()
        ? outline_offset.as_calculated().resolve_length(color_resolution_context.calculation_resolution_context).value()
        : outline_offset.as_length().length();
    if (!misc_reset_adopted)
        computed_values.set_outline_offset(resolved_outline_offset.absolute_length_to_px());
    if (!misc_reset_adopted)
        computed_values.set_outline_offset_style_value(outline_offset);
    if (!misc_reset_adopted)
        computed_values.set_outline_style(computed_style.outline_style());

    // FIXME: Interpolation can cause negative values - we clamp here but should instead clamp as part of interpolation.
    if (!misc_reset_adopted)
        computed_values.set_outline_width(max(CSSPixels { 0 }, computed_style.length(CSS::PropertyID::OutlineWidth).absolute_length_to_px()));

    if (!grid_adopted)
        computed_values.set_grid_auto_columns(computed_style.grid_auto_columns());
    if (!grid_adopted)
        computed_values.set_grid_auto_rows(computed_style.grid_auto_rows());
    if (!grid_adopted)
        computed_values.set_grid_template_columns(computed_style.grid_template_columns());
    if (!grid_adopted)
        computed_values.set_grid_template_rows(computed_style.grid_template_rows());
    if (!grid_adopted)
        computed_values.set_grid_column_end(computed_style.grid_column_end());
    if (!grid_adopted)
        computed_values.set_grid_column_start(computed_style.grid_column_start());
    if (!grid_adopted)
        computed_values.set_grid_row_end(computed_style.grid_row_end());
    if (!grid_adopted)
        computed_values.set_grid_row_start(computed_style.grid_row_start());
    if (!grid_adopted)
        computed_values.set_grid_template_areas(computed_style.grid_template_areas());
    if (!grid_adopted)
        computed_values.set_grid_auto_flow(computed_style.grid_auto_flow());

    if (!svg_reset_adopted)
        computed_values.set_cx(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Cx)));
    if (!svg_reset_adopted)
        computed_values.set_cy(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Cy)));
    if (!svg_reset_adopted)
        computed_values.set_r(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::R)));
    if (!svg_reset_adopted)
        computed_values.set_rx(CSS::LengthPercentageOrAuto::from_style_value(computed_style.property(CSS::PropertyID::Rx)));
    if (!svg_reset_adopted)
        computed_values.set_ry(CSS::LengthPercentageOrAuto::from_style_value(computed_style.property(CSS::PropertyID::Ry)));
    if (!svg_reset_adopted)
        computed_values.set_x(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::X)));
    if (!svg_reset_adopted)
        computed_values.set_y(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Y)));

    if (!inherited_svg_adopted)
        computed_values.set_fill(computed_style.fill(color_resolution_context));
    if (!inherited_svg_adopted)
        computed_values.set_stroke(computed_style.stroke(color_resolution_context));

    if (!svg_reset_adopted)
        computed_values.set_stop_color(computed_style.color(CSS::PropertyID::StopColor, color_resolution_context));

    auto const& stroke_width = computed_style.property(CSS::PropertyID::StrokeWidth);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    if (!inherited_svg_adopted) {
        if (stroke_width.is_number())
            computed_values.set_stroke_width(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_width.as_number().number())));
        else
            computed_values.set_stroke_width(CSS::LengthPercentage::from_style_value(stroke_width));
    }
    if (!svg_reset_adopted)
        computed_values.set_shape_rendering(computed_style.shape_rendering());
    if (!inherited_svg_adopted) {
        computed_values.set_paint_order(computed_style.paint_order());
        auto const& paint_order = computed_style.property(PropertyID::PaintOrder);
        computed_values.set_paint_order_serialization(
            paint_order.is_value_list() ? paint_order.as_value_list().size() : paint_order.to_keyword() == Keyword::Normal ? 0
                                                                                                                           : 1,
            paint_order.to_keyword() == Keyword::Normal);
    }

    // FIXME: Remove this once we support URL values in mask_layers and can therefore use it in
    //        `establishes_stacking_context()`
    auto const& mask_image = [&] -> CSS::StyleValue const& {
        auto const& value = computed_style.property(CSS::PropertyID::MaskImage);

        if (value.is_value_list())
            return value.as_value_list().values()[0];

        return value;
    }();
    if (!mask_adopted) {
        if (mask_image.is_url()) {
            computed_values.set_mask(mask_image.as_url().url());
        } else if (mask_image.is_abstract_image()) {
            auto const& abstract_image = mask_image.as_abstract_image();
            computed_values.set_mask_image(abstract_image);
        }
    }

    if (!mask_adopted)
        computed_values.set_mask_type(computed_style.mask_type());

    auto const& clip_path = computed_style.property(CSS::PropertyID::ClipPath);
    if (!mask_adopted) {
        if (clip_path.is_url())
            computed_values.set_clip_path(clip_path.as_url().url());
        else if (clip_path.is_basic_shape())
            computed_values.set_clip_path(clip_path.as_basic_shape());
    }
    if (!inherited_svg_adopted)
        computed_values.set_clip_rule(computed_style.clip_rule());
    if (!inherited_svg_adopted)
        computed_values.set_fill_rule(computed_style.fill_rule());

    if (!inherited_svg_adopted)
        computed_values.set_fill_opacity(computed_style.fill_opacity());
    if (!inherited_svg_adopted)
        computed_values.set_stroke_dasharray(computed_style.stroke_dasharray());

    auto const& stroke_dashoffset = computed_style.property(CSS::PropertyID::StrokeDashoffset);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    if (!inherited_svg_adopted) {
        if (stroke_dashoffset.is_number())
            computed_values.set_stroke_dashoffset(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_dashoffset.as_number().number())));
        else
            computed_values.set_stroke_dashoffset(CSS::LengthPercentage::from_style_value(stroke_dashoffset));
    }

    if (!inherited_svg_adopted)
        computed_values.set_stroke_linecap(computed_style.stroke_linecap());
    if (!inherited_svg_adopted)
        computed_values.set_stroke_linejoin(computed_style.stroke_linejoin());
    if (!svg_reset_adopted)
        computed_values.set_vector_effect(computed_style.vector_effect());
    if (!inherited_svg_adopted)
        computed_values.set_stroke_miterlimit(computed_style.stroke_miterlimit());

    if (!inherited_svg_adopted)
        computed_values.set_stroke_opacity(computed_style.stroke_opacity());
    if (!svg_reset_adopted)
        computed_values.set_stop_opacity(computed_style.stop_opacity());

    if (!inherited_svg_adopted)
        computed_values.set_text_anchor(computed_style.text_anchor());
    if (!inherited_svg_adopted)
        computed_values.set_dominant_baseline(computed_style.dominant_baseline());

    if (auto const& column_count = computed_style.property(CSS::PropertyID::ColumnCount); !misc_reset_adopted && column_count.to_keyword() != Keyword::Auto)
        computed_values.set_column_count(CSS::ColumnCount::make_integer(int_from_style_value(NonnullRefPtr<StyleValue const> { column_count })));

    if (!misc_reset_adopted)
        computed_values.set_column_span(computed_style.column_span());

    if (!misc_reset_adopted)
        computed_values.set_column_width(computed_style.size_value(CSS::PropertyID::ColumnWidth));
    if (!misc_reset_adopted)
        computed_values.set_column_height(computed_style.size_value(CSS::PropertyID::ColumnHeight));

    if (!alignment_adopted)
        computed_values.set_column_gap(computed_style.gap_value(CSS::PropertyID::ColumnGap));
    if (!alignment_adopted)
        computed_values.set_row_gap(computed_style.gap_value(CSS::PropertyID::RowGap));

    if (!inherited_table_adopted)
        computed_values.set_border_collapse(computed_style.border_collapse());

    if (!inherited_table_adopted)
        computed_values.set_empty_cells(computed_style.empty_cells());

    if (!misc_reset_adopted)
        computed_values.set_table_layout(computed_style.table_layout());

    auto const& aspect_ratio = computed_style.property(CSS::PropertyID::AspectRatio);
    if (aspect_ratio.is_value_list()) {
        auto const& values_list = aspect_ratio.as_value_list().values();
        if (values_list.size() == 2
            && values_list[0]->is_keyword() && values_list[0]->as_keyword().keyword() == CSS::Keyword::Auto
            && values_list[1]->is_ratio()) {
            auto ratio = values_list[1]->as_ratio().resolved();
            if (ratio.is_degenerate())
                computed_values.set_aspect_ratio({ true, {}, true, ratio });
            else
                computed_values.set_aspect_ratio({ true, ratio, true, ratio });
        }
    } else if (aspect_ratio.is_keyword() && aspect_ratio.as_keyword().keyword() == CSS::Keyword::Auto) {
        computed_values.set_aspect_ratio({ true, {}, true, {} });
    } else if (aspect_ratio.is_ratio()) {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio
        // If the <ratio> is degenerate, the property instead behaves as auto.
        if (aspect_ratio.as_ratio().resolved().is_degenerate())
            computed_values.set_aspect_ratio({ true, {}, false, aspect_ratio.as_ratio().resolved() });
        else
            computed_values.set_aspect_ratio({ false, aspect_ratio.as_ratio().resolved(), false, aspect_ratio.as_ratio().resolved() });
    }

    if (!misc_reset_adopted)
        computed_values.set_touch_action(computed_style.touch_action());

    auto const& math_shift_value = computed_style.property(CSS::PropertyID::MathShift);
    if (auto math_shift = keyword_to_math_shift(math_shift_value.to_keyword()); math_shift.has_value())
        computed_values.set_math_shift(math_shift.value());

    auto const& math_style_value = computed_style.property(CSS::PropertyID::MathStyle);
    if (auto math_style = keyword_to_math_style(math_style_value.to_keyword()); math_style.has_value())
        computed_values.set_math_style(math_style.value());

    computed_values.set_math_depth(computed_style.math_depth());
    computed_values.set_quotes(computed_style.quotes());
    computed_values.set_counter_increment(computed_style.counter_data(CSS::PropertyID::CounterIncrement));
    computed_values.set_counter_reset(computed_style.counter_data(CSS::PropertyID::CounterReset));
    computed_values.set_counter_set(computed_style.counter_data(CSS::PropertyID::CounterSet));

    if (!misc_reset_adopted)
        computed_values.set_object_fit(computed_style.object_fit());
    if (!misc_reset_adopted)
        computed_values.set_object_position(computed_style.object_position());
    if (!inherited_box_adopted)
        computed_values.set_direction(computed_style.direction());
    computed_values.set_unicode_bidi(computed_style.unicode_bidi());
    if (!misc_reset_adopted)
        computed_values.set_scroll_behavior(CSS::keyword_to_scroll_behavior(computed_style.property(CSS::PropertyID::ScrollBehavior).to_keyword()).release_value());
    if (!inherited_ui_adopted)
        computed_values.set_scrollbar_color(computed_style.scrollbar_color(color_resolution_context));
    if (!misc_reset_adopted)
        computed_values.set_scrollbar_gutter(computed_style.property(CSS::PropertyID::ScrollbarGutter).as_scrollbar_gutter().value());
    if (!misc_reset_adopted)
        computed_values.set_scrollbar_width(computed_style.scrollbar_width());
    if (!misc_reset_adopted)
        computed_values.set_shape_image_threshold(computed_style.property(CSS::PropertyID::ShapeImageThreshold).as_opacity_value().resolved());
    if (!misc_reset_adopted)
        computed_values.set_shape_margin(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::ShapeMargin)));
    CSS::ShapeOutsideData shape_outside;
    auto apply_shape_outside_item = [&](CSS::StyleValue const& item) {
        if (item.is_url())
            shape_outside.image = item.as_url().url();
        else if (item.is_abstract_image())
            shape_outside.image = NonnullRefPtr<CSS::AbstractImageStyleValue const> { item.as_abstract_image() };
        else if (item.is_basic_shape())
            shape_outside.basic_shape = item.as_basic_shape();
        else if (auto shape_box = CSS::keyword_to_shape_box(item.to_keyword()); shape_box.has_value())
            shape_outside.shape_box = *shape_box;
    };
    auto const& shape_outside_value = computed_style.property(CSS::PropertyID::ShapeOutside);
    if (shape_outside_value.is_value_list()) {
        for (auto const& item : shape_outside_value.as_value_list().values())
            apply_shape_outside_item(item);
    } else {
        apply_shape_outside_item(shape_outside_value);
    }
    if (!misc_reset_adopted)
        computed_values.set_shape_outside(move(shape_outside));
    if (!inherited_box_adopted)
        computed_values.set_writing_mode(computed_style.writing_mode());
    if (!misc_reset_adopted)
        computed_values.set_user_select(computed_style.user_select());
    if (!effects_adopted)
        computed_values.set_isolation(computed_style.isolation());
    if (!effects_adopted)
        computed_values.set_mix_blend_mode(computed_style.mix_blend_mode());
    if (!misc_reset_adopted)
        computed_values.set_view_transition_name(computed_style.view_transition_name());
    computed_values.set_contain(computed_style.contain());
    computed_values.set_container_name(computed_style.container_name());
    computed_values.set_container_type(computed_style.container_type());
    computed_values.set_will_change(computed_style.will_change());

    if (!inherited_ui_adopted) {
        auto const& caret_color_value = computed_style.property(CSS::PropertyID::CaretColor);
        CSS::ColorOrAuto caret_color;
        caret_color.used_value = computed_style.caret_color(color_resolution_context);
        if (caret_color_value.to_keyword() != CSS::Keyword::Auto)
            caret_color.computed_value = caret_color.used_value;
        computed_values.set_caret_color(move(caret_color));
    }
    if (!inherited_svg_adopted)
        computed_values.set_color_interpolation(computed_style.color_interpolation());
    if (!inherited_svg_adopted)
        computed_values.set_color_interpolation_filters(computed_style.color_interpolation_filters());
    computed_values.set_resize(computed_style.resize());

    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        computed_values.set_property_important(property_id, computed_style.is_property_important(property_id));
        computed_values.set_property_inherited(property_id, computed_style.is_property_inherited(property_id));
    }
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
    computed_values.set_inheritance_dependent_specified_values(computed_style.inheritance_dependent_specified_values());
    computed_values.set_raw_cascaded_font_size(computed_style.raw_cascaded_font_size());

    return move(builder).build();
}

}
