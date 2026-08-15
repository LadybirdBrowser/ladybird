/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

static constexpr bool style_group_payload_is_rust_native(ComputedValuesFFI::StyleGroupLifecycle lifecycle)
{
    switch (lifecycle) {
    case ComputedValuesFFI::StyleGroupLifecycle::Cpp:
    case ComputedValuesFFI::StyleGroupLifecycle::CppWithBorderFacts:
    case ComputedValuesFFI::StyleGroupLifecycle::CppWithInheritedTextFacts:
    case ComputedValuesFFI::StyleGroupLifecycle::CppWithFontFacts:
        return false;
    case ComputedValuesFFI::StyleGroupLifecycle::InheritedTable:
    case ComputedValuesFFI::StyleGroupLifecycle::InheritedBox:
    case ComputedValuesFFI::StyleGroupLifecycle::Sizing:
    case ComputedValuesFFI::StyleGroupLifecycle::Alignment:
    case ComputedValuesFFI::StyleGroupLifecycle::SVGReset:
    case ComputedValuesFFI::StyleGroupLifecycle::Surround:
    case ComputedValuesFFI::StyleGroupLifecycle::Box:
    case ComputedValuesFFI::StyleGroupLifecycle::Grid:
        return true;
    }
    VERIFY_NOT_REACHED();
}

template<typename T>
static consteval ComputedValuesFFI::StyleGroupLifecycle style_group_lifecycle_of()
{
    if constexpr (requires { T::style_group_lifecycle; })
        return T::style_group_lifecycle;
    else
        return ComputedValuesFFI::StyleGroupLifecycle::Cpp;
}

template<typename T>
static consteval ComputedValuesFFI::StyleGroupVTable make_style_group_vtable()
{
    if constexpr (style_group_payload_is_rust_native(style_group_lifecycle_of<T>())) {
        return {
            .lifecycle = T::style_group_lifecycle,
            .size = sizeof(T),
            .align = alignof(T),
            .default_construct = nullptr,
            .copy_construct = nullptr,
            .destruct = nullptr,
            .equals = nullptr,
        };
    }
    return {
        .lifecycle = style_group_lifecycle_of<T>(),
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
static void assemble_transform_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_effects_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_background_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_mask_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_border_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_inherited_svg_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_inherited_list_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_content_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_inherited_ui_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_inherited_text_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_misc_reset_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_animation_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_text_reset_group_payload(void* payload_pointer, void const* data_pointer);
static void assemble_anchor_group_payload(void* payload_pointer, void const* data_pointer);

// The C++ context the registered table-group assemblers reach through their
// assembly structs, for the members only C++ can produce: stamped image
// wrapper mints through property(), the color fallback arm for values the
// core could not resolve, and style-scope lookups.
struct TableGroupAssemblerContext {
    ComputedStyleWorkingSet const& computed_style;
    StyleScope const& style_scope;
    ColorResolutionContext const& color_resolution_context;
};

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

    using MiscReset = ComputedValues::MiscResetValues;
    constexpr auto misc_reset = to_underlying(StyleGroupIndex::MiscResetValues);
    for (auto property : { PropertyID::ScrollMarginTop, PropertyID::ScrollMarginRight, PropertyID::ScrollMarginBottom, PropertyID::ScrollMarginLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_PX, 0, nullptr, 0);
    for (auto property : { PropertyID::ScrollPaddingTop, PropertyID::ScrollPaddingRight, PropertyID::ScrollPaddingBottom, PropertyID::ScrollPaddingLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_KEYWORD, to_underlying(Keyword::Auto), nullptr);
    for (auto property : { PropertyID::OverflowClipMarginTop, PropertyID::OverflowClipMarginRight, PropertyID::OverflowClipMarginBottom, PropertyID::OverflowClipMarginLeft })
        add(misc_reset, property, 0, GROUP_FIELD_REQUIRE_INITIAL_VALUE, 0, nullptr);
    add(misc_reset, PropertyID::ColumnSpan, offsetof(MiscReset, column_span), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<keyword_to_column_span>());
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
    add(inherited_text, PropertyID::OverflowWrap, offsetof(InheritedText, overflow_wrap), GROUP_FIELD_ENUM_KEYWORD, 0, &keyword_code_table<overflow_wrap_from_keyword>());
    add(inherited_text, PropertyID::WordSpacing, offsetof(InheritedText, word_spacing), GROUP_FIELD_CSS_PIXELS, 0, nullptr);
    add(inherited_text, PropertyID::WordSpacing, offsetof(InheritedText, word_spacing_style_value), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
    add(inherited_text, PropertyID::LetterSpacing, offsetof(InheritedText, letter_spacing), GROUP_FIELD_CSS_PIXELS, 0, nullptr);
    add(inherited_text, PropertyID::LetterSpacing, offsetof(InheritedText, letter_spacing_style_value), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
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

    using InheritedList = ComputedValues::InheritedListValues;
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

    using Border = ComputedValues::BorderValues;
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
        add(border, side.color, side.data_offset + offsetof(BorderData, color), GROUP_FIELD_COLOR, 0, nullptr);
        add(border, side.color, side.data_handle_offset, GROUP_FIELD_RETAINED_DATA, 0, nullptr);
        // NB: A none border-style keeps BorderData's width at the constructor's zero,
        //     matching the used-width rule; styled borders take the C++ path.
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

    using Background = ComputedValues::BackgroundValues;
    constexpr auto background = to_underlying(StyleGroupIndex::BackgroundValues);
    add(background, PropertyID::BackgroundColor, offsetof(Background, background_color), GROUP_FIELD_COLOR, 0, nullptr);
    add(background, PropertyID::BackgroundColor, offsetof(Background, background_color_style_value), GROUP_FIELD_RETAINED_DATA, 0, nullptr);
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
    for (auto property : { PropertyID::FlexDirection, PropertyID::FlexWrap, PropertyID::FlexBasis,
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
             PropertyID::ColumnWidth, PropertyID::ColumnCount, PropertyID::ZIndex, PropertyID::VerticalAlign,
             PropertyID::AspectRatio, PropertyID::Contain, PropertyID::ContainerType, PropertyID::ContainerName })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::BoxValues));
    // The font group builds in C++; only bindings verified against its setters are made.
    for (auto property : { PropertyID::FontSize, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::LineHeight })
        bind_property_to_group(property, to_underlying(StyleGroupIndex::FontValues));

    rust_style_group_register_field_descriptors(descriptors.data(), descriptors.size());
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::TransformValues), assemble_transform_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::EffectsValues), assemble_effects_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::BackgroundValues), assemble_background_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::MaskValues), assemble_mask_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::BorderValues), assemble_border_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::InheritedSVGValues), assemble_inherited_svg_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::InheritedListValues), assemble_inherited_list_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::ContentValues), assemble_content_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::InheritedUIValues), assemble_inherited_ui_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::InheritedTextValues), assemble_inherited_text_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::MiscResetValues), assemble_misc_reset_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::AnimationValues), assemble_animation_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::TextResetValues), assemble_text_reset_group_payload);
    rust_style_group_register_payload_assembler(to_underlying(StyleGroupIndex::AnchorValues), assemble_anchor_group_payload);

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
static_assert(sizeof(Size) == sizeof(ComputedValuesFFI::ComputedSize));
static_assert(alignof(Size) == alignof(ComputedValuesFFI::ComputedSize));
static_assert(sizeof(RustStyleValueHandle) == sizeof(StyleValueFFI::StyleValueData const*));
static_assert(alignof(RustStyleValueHandle) == alignof(StyleValueFFI::StyleValueData const*));

// The border group keeps its C++ lifecycle, but its four leading BorderData
// members double as the Rust BorderLayoutFacts prefix that layout reads as
// typed fields.
static_assert(sizeof(Gfx::Color) == sizeof(u32));
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

// The inherited-text group keeps its C++ lifecycle, but its leading members
// double as the Rust InheritedTextLayoutFacts prefix that layout reads as
// typed fields.
static_assert(sizeof(TextIndentData) == sizeof(ComputedValuesFFI::ComputedTextIndent));
static_assert(offsetof(TextIndentData, length_percentage) == offsetof(ComputedValuesFFI::ComputedTextIndent, length_percentage));
static_assert(offsetof(TextIndentData, each_line) == offsetof(ComputedValuesFFI::ComputedTextIndent, each_line));
static_assert(offsetof(TextIndentData, hanging) == offsetof(ComputedValuesFFI::ComputedTextIndent, hanging));
static_assert(offsetof(ComputedValues::InheritedTextValues, text_align) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, text_align));
static_assert(offsetof(ComputedValues::InheritedTextValues, text_justify) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, text_justify));
static_assert(offsetof(ComputedValues::InheritedTextValues, white_space_collapse) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, white_space_collapse));
static_assert(offsetof(ComputedValues::InheritedTextValues, text_wrap_mode) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, text_wrap_mode));
static_assert(offsetof(ComputedValues::InheritedTextValues, word_break) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, word_break));
static_assert(offsetof(ComputedValues::InheritedTextValues, tab_size_is_number) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, tab_size_is_number));
static_assert(offsetof(ComputedValues::InheritedTextValues, letter_spacing) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, letter_spacing));
static_assert(offsetof(ComputedValues::InheritedTextValues, word_spacing) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, word_spacing));
static_assert(offsetof(ComputedValues::InheritedTextValues, tab_size_length) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, tab_size_length));
static_assert(offsetof(ComputedValues::InheritedTextValues, tab_size_number) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, tab_size_number));
static_assert(offsetof(ComputedValues::InheritedTextValues, text_indent) == offsetof(ComputedValuesFFI::InheritedTextLayoutFacts, text_indent));
static_assert(sizeof(ComputedValuesFFI::InheritedTextLayoutFacts) <= offsetof(ComputedValues::InheritedTextValues, color));

// The font group keeps its C++ lifecycle, but its leading members double as
// the Rust FontLayoutFacts prefix that layout reads as typed fields.
static_assert(sizeof(RefPtr<Gfx::FontCascadeList const>) == sizeof(void const*));
static_assert(offsetof(ComputedValues::FontValues, font_size) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_size));
static_assert(offsetof(ComputedValues::FontValues, line_height_used) == offsetof(ComputedValuesFFI::FontLayoutFacts, line_height_used));
static_assert(offsetof(ComputedValues::FontValues, font_variant_emoji) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_variant_emoji));
static_assert(offsetof(ComputedValues::FontValues, font_ascent) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_ascent));
static_assert(offsetof(ComputedValues::FontValues, font_descent) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_descent));
static_assert(offsetof(ComputedValues::FontValues, font_x_height) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_x_height));
static_assert(offsetof(ComputedValues::FontValues, first_available_font) == offsetof(ComputedValuesFFI::FontLayoutFacts, first_available_font));
static_assert(offsetof(ComputedValues::FontValues, font_list) == offsetof(ComputedValuesFFI::FontLayoutFacts, font_cascade_list));
static_assert(sizeof(ComputedValuesFFI::FontLayoutFacts) <= offsetof(ComputedValues::FontValues, font_families));

void const* style_group_default_payload(size_t group_index)
{
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
    auto values = m_inheritance_dependent_specified_values;
    for (auto const& entry : m_borrowed_inheritance_dependent_values) {
        auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(entry.value);
        values.set(
            static_cast<PropertyID>(entry.property),
            StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data)));
    }
    return values;
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
    if (m_longhand_values.is_empty() || previous.m_longhand_values.is_empty())
        return;
    if (m_longhand_values.data() == previous.m_longhand_values.data())
        return;
    for (size_t index = 0; index < number_of_longhand_properties; ++index) {
        if (!StyleValueFFI::rust_style_value_equals(
                static_cast<StyleValueFFI::StyleValueData const*>(m_longhand_values[index]),
                static_cast<StyleValueFFI::StyleValueData const*>(previous.m_longhand_values[index])))
            return;
    }
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

// https://drafts.csswg.org/css-transforms-2/#grouping-property-values
bool ComputedValues::has_transform_style_grouping_property() const
{
    // The following CSS property values require the user agent to create a flattened representation of the
    // descendant elements before they can be applied, and therefore force the element to have a used style of
    // flat for preserve-3d.
    // NB: The contain bullet requires layout context and is checked by NodeWithStyle::used_transform_style().

    // overflow: any value other than visible or clip.
    if (!first_is_one_of(overflow_x(), Overflow::Visible, Overflow::Clip)
        || !first_is_one_of(overflow_y(), Overflow::Visible, Overflow::Clip))
        return true;

    // opacity: any value less than 1.
    if (opacity() < 1)
        return true;

    // filter: any value other than none.
    if (filter().has_filters())
        return true;

    // clip: any value other than auto.
    if (!clip().is_auto())
        return true;

    // clip-path: any value other than none.
    if (clip_path().has_value())
        return true;

    // isolation: used value of isolate.
    if (isolation() == Isolation::Isolate)
        return true;

    // mask-image: any value other than none.
    if (mask().has_value() || any_of(mask_layers(), [](auto const& layer) { return layer.background_image != nullptr; }))
        return true;

    // FIXME: mask-border-source: any value other than none.

    // mix-blend-mode: any value other than normal.
    if (mix_blend_mode() != MixBlendMode::Normal)
        return true;

    // AD-HOC: backdrop-filter is missing from the specification's list, but it buffers descendants the same way
    //         filter does, and other engines treat it as a grouping property.
    if (backdrop_filter().has_filters())
        return true;

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

ComputedStyleRecordView::ComputedStyleRecordView(StyleEngineFFI::FfiStyleRecordView const& view, StyleComputer const& style_computer, StyleRecordID style_record_identity)
    : m_style_computer(view.animation_overlay_identity != 0 ? &style_computer : nullptr)
    , m_style_record_identity(style_record_identity)
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
        m_base_values.borrow_style_record_payloads(base_payloads);
        m_values.m_borrowed_base_values = &m_base_values;
    }

    VERIFY(view.property_importance_count == m_values.m_property_important.size_in_bytes());
    VERIFY(view.property_inheritance_count == m_values.m_property_inherited.size_in_bytes());
    m_values.m_property_important.copy_from({ view.property_importance, view.property_importance_count });
    m_values.m_property_inherited.copy_from({ view.property_inheritance, view.property_inheritance_count });

    m_values.m_pseudo_element_styles = view.pseudo_element_styles;
    m_values.m_depends_on_viewport_metrics = view.dependency_flags & 1;
    m_values.m_font_metrics_depend_on_viewport_metrics = view.dependency_flags & 2;
    m_values.m_in_display_none_subtree = view.dependency_flags & 4;
    m_values.m_borrowed_raw_cascaded_font_size = static_cast<StyleValueFFI::StyleValueData const*>(view.raw_cascaded_font_size);
    m_values.m_borrowed_inheritance_dependent_values = { view.inheritance_dependent_values, view.inheritance_dependent_value_count };
    VERIFY(view.longhand_value_count == 0 || view.longhand_value_count == number_of_longhand_properties);
    m_values.m_longhand_values = { view.longhand_values, view.longhand_value_count };
    m_values.m_animated_properties = static_cast<AnimatedProperties const*>(view.animated_properties);
    if (view.animation_overlay_identity != 0) {
        m_base_values.m_property_important = m_values.m_property_important;
        m_base_values.m_property_inherited = m_values.m_property_inherited;
        m_base_values.m_pseudo_element_styles = m_values.m_pseudo_element_styles;
        m_base_values.m_depends_on_viewport_metrics = m_values.m_depends_on_viewport_metrics;
        m_base_values.m_font_metrics_depend_on_viewport_metrics = m_values.m_font_metrics_depend_on_viewport_metrics;
        m_base_values.m_in_display_none_subtree = m_values.m_in_display_none_subtree;
        m_base_values.m_borrowed_raw_cascaded_font_size = m_values.m_borrowed_raw_cascaded_font_size;
        m_base_values.m_borrowed_inheritance_dependent_values = m_values.m_borrowed_inheritance_dependent_values;
        m_base_values.m_longhand_values = m_values.m_longhand_values;
    }
    m_present = true;
}

ComputedStyleRecordView::~ComputedStyleRecordView()
{
    if (m_style_computer)
        m_style_computer->unpin_style_record(m_style_record_identity);
}

void ComputedStyleRecordView::retain_across_style_record_publication()
{
    VERIFY(m_present);
    VERIFY(m_style_computer);
    VERIFY(m_values.animated_properties());
    m_retained_values = ComputedValues::Builder { m_values }.build();
    m_style_computer->unpin_style_record(m_style_record_identity);
    m_style_computer = nullptr;
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

static RefPtr<StyleValue const> adopt_assembly_handle(void const* pointer)
{
    if (!pointer)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(pointer));
}

// Fills the transform group's wrapper-backed members from the core's
// pre-lowered assembly: the matrices arrive baked, and only the values that
// genuinely need C++ - the style value wrappers and the per-axis
// length-percentage slots - are wrapped here from the retained handles the
// assembly carries.
static void assemble_transform_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::TransformValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiTransformGroupAssembly const*>(data_pointer);

    if (auto transform_list = adopt_assembly_handle(data.transform_list))
        payload.transformations = transformations_for_style_value(*transform_list);
    auto adopt_transformation = [](void const* pointer) -> RefPtr<TransformationStyleValue const> {
        auto value = adopt_assembly_handle(pointer);
        if (!value)
            return nullptr;
        return value->as_transformation();
    };
    payload.rotate = adopt_transformation(data.rotate);
    payload.translate = adopt_transformation(data.translate);
    payload.scale = adopt_transformation(data.scale);

    payload.resolved_transform_list.ensure_capacity(data.resolved_transform_count);
    for (size_t i = 0; i < data.resolved_transform_count; ++i) {
        auto const& entry = data.resolved_transforms[i];
        if (entry.is_translate) {
            payload.resolved_transform_list.unchecked_append(ResolvedTransform { ResolvedTransform::Translate {
                .x = { .px = entry.x_px, .percentage_value = adopt_assembly_handle(entry.x_percentage) },
                .y = { .px = entry.y_px, .percentage_value = adopt_assembly_handle(entry.y_percentage) },
                .z = entry.z_px,
            } });
        } else {
            auto const& m = entry.matrix;
            payload.resolved_transform_list.unchecked_append(ResolvedTransform { FloatMatrix4x4(
                m[0], m[1], m[2], m[3],
                m[4], m[5], m[6], m[7],
                m[8], m[9], m[10], m[11],
                m[12], m[13], m[14], m[15]) });
        }
    }

    if (auto origin_x = adopt_assembly_handle(data.transform_origin_x)) {
        auto origin_y = adopt_assembly_handle(data.transform_origin_y);
        auto origin_z = adopt_assembly_handle(data.transform_origin_z);
        auto length_percentage_with_keywords_resolved = [](StyleValue const& value) -> LengthPercentage {
            if (value.is_keyword()) {
                auto keyword = value.to_keyword();
                if (keyword == Keyword::Left || keyword == Keyword::Top)
                    return Percentage(0);
                if (keyword == Keyword::Center)
                    return Percentage(50);
                if (keyword == Keyword::Right || keyword == Keyword::Bottom)
                    return Percentage(100);

                VERIFY_NOT_REACHED();
            }
            return LengthPercentage::from_style_value(value);
        };
        payload.transform_origin = {
            length_percentage_with_keywords_resolved(*origin_x),
            length_percentage_with_keywords_resolved(*origin_y),
            LengthPercentage::from_style_value(*origin_z),
        };
    }

    if (data.has_perspective)
        payload.perspective = CSSPixels::from_raw(data.perspective_px);

    if (auto perspective_origin = adopt_assembly_handle(data.perspective_origin)) {
        auto const& position = perspective_origin->as_position();
        auto edge_x = position.edge_x();
        auto edge_y = position.edge_y();
        payload.perspective_origin = {
            .offset_x = LengthPercentage::from_style_value(edge_x->offset()),
            .offset_y = LengthPercentage::from_style_value(edge_y->offset()),
        };
    }
}

static Filter filter_from_assembly(ComputedValuesFFI::FfiLoweredFilter const& lowered)
{
    auto filter_list = adopt_assembly_handle(lowered.filter_list);
    if (!filter_list)
        return Filter::make_none();

    Vector<Filter::FilterOperation> operations;
    operations.ensure_capacity(lowered.operation_count);
    for (size_t i = 0; i < lowered.operation_count; ++i) {
        auto const& operation = lowered.operations[i];
        switch (operation.kind) {
        case to_underlying(FilterStyleValue::Kind::Blur):
            operations.unchecked_append(Filter::Blur { .resolved_radius = operation.amount });
            break;
        case to_underlying(FilterStyleValue::Kind::DropShadow):
            operations.unchecked_append(Filter::DropShadow {
                .offset_x = CSSPixels::from_raw(operation.shadow_offset_x),
                .offset_y = CSSPixels::from_raw(operation.shadow_offset_y),
                .radius = CSSPixels::from_raw(operation.shadow_radius),
                .color = Color::from_bgra(operation.shadow_color),
            });
            break;
        case to_underlying(FilterStyleValue::Kind::HueRotate):
            operations.unchecked_append(Filter::HueRotate { .angle_degrees = operation.amount });
            break;
        case to_underlying(FilterStyleValue::Kind::Color):
            operations.unchecked_append(Filter::ColorOperation {
                .operation = static_cast<Gfx::ColorFilterType>(operation.color_operation),
                .resolved_amount = operation.amount,
            });
            break;
        default: {
            // A url(#fragment) filter reference; the fragment extraction stays
            // here with the URL type.
            auto url_value = adopt_assembly_handle(operation.url_value);
            auto url = url_value->as_url().url();
            auto const& url_string = url.url();
            Utf16String fragment;
            if (!url_string.is_empty() && url_string.starts_with('#')) {
                if (auto fragment_or_error = url_string.substring_from_byte_offset(1); !fragment_or_error.is_error())
                    fragment = Utf16String::from_utf8(fragment_or_error.value());
            }
            operations.unchecked_append(Filter::Url { move(fragment) });
            break;
        }
        }
    }
    return Filter::create_lowered(filter_list->as_value_list(), move(operations));
}

// Fills the effects group's complex members from the core's pre-lowered
// assembly: filter operations, shadow data and the clip rect arrive as plain
// data.
static void assemble_effects_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::EffectsValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiEffectsGroupAssembly const*>(data_pointer);

    payload.filter = filter_from_assembly(data.filter);
    payload.backdrop_filter = filter_from_assembly(data.backdrop_filter);

    payload.box_shadow.ensure_capacity(data.box_shadow_count);
    for (size_t i = 0; i < data.box_shadow_count; ++i) {
        auto const& shadow = data.box_shadows[i];
        payload.box_shadow.unchecked_append(ShadowData {
            CSSPixels::from_raw(shadow.offset_x),
            CSSPixels::from_raw(shadow.offset_y),
            CSSPixels::from_raw(shadow.blur_radius),
            CSSPixels::from_raw(shadow.spread_distance),
            Color::from_bgra(shadow.color),
            static_cast<ColorSyntax>(shadow.color_syntax),
            static_cast<ShadowPlacement>(shadow.placement),
        });
    }

    if (data.clip_is_rect) {
        auto edge = [](ComputedValuesFFI::FfiLoweredClipEdge const& lowered) {
            if (lowered.is_auto)
                return LengthOrAuto::make_auto();
            return LengthOrAuto { Length { lowered.value, static_cast<LengthUnit>(lowered.unit) } };
        };
        payload.clip = Clip(EdgeRect {
            edge(data.clip_edges[0]),
            edge(data.clip_edges[1]),
            edge(data.clip_edges[2]),
            edge(data.clip_edges[3]),
        });
    }
}

// Fills one background or mask layer's wrapper-backed members from the
// core's lowering: the image wrapper (minted through the stamped property()
// path when the slot holds an actual image resource), the position offsets,
// the repeat codes and the size.
static void assemble_coordinated_layer(BackgroundLayerData& layer, ComputedValuesFFI::FfiCoordinatedLayerAssembly const& lowered, TableGroupAssemblerContext const& context, PropertyID image_property, size_t layer_index)
{
    RefPtr<StyleValue const> image_wrapper;
    if (lowered.image_needs_stamped_wrapper) {
        auto const& image_value = context.computed_style.property(image_property);
        if (image_value.is_value_list())
            image_wrapper = image_value.as_value_list().values()[layer_index];
        else
            image_wrapper = image_value;
    } else {
        image_wrapper = adopt_assembly_handle(lowered.image);
    }
    layer.image_style_value = image_wrapper;
    if (lowered.image_is_abstract_image)
        layer.background_image = image_wrapper->as_abstract_image();

    layer.position_x = LengthPercentage::from_style_value(adopt_assembly_handle(lowered.position_x).release_nonnull());
    layer.position_y = LengthPercentage::from_style_value(adopt_assembly_handle(lowered.position_y).release_nonnull());

    layer.repeat_x = static_cast<Repetition>(lowered.repeat_x);
    layer.repeat_y = static_cast<Repetition>(lowered.repeat_y);

    layer.size_type = static_cast<BackgroundSize>(lowered.size_type);
    if (layer.size_type == BackgroundSize::LengthPercentage) {
        layer.size_x = LengthPercentageOrAuto::from_style_value(adopt_assembly_handle(lowered.size_x).release_nonnull());
        layer.size_y = LengthPercentageOrAuto::from_style_value(adopt_assembly_handle(lowered.size_y).release_nonnull());
    }
}

// Fills the background group's complex members from the core's pre-lowered
// assembly: the layer list's wrapper and length-percentage members are
// transcribed here, and a background-color the core could not resolve takes
// the C++ resolution arm.
static void assemble_background_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::BackgroundValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiBackgroundGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    if (!data.color_resolved)
        payload.background_color = context.computed_style.color(PropertyID::BackgroundColor, context.color_resolution_context);
    payload.background_color_clip = static_cast<BackgroundBox>(data.color_clip);

    Vector<BackgroundLayerData> layers;
    layers.ensure_capacity(data.layer_count);
    for (size_t i = 0; i < data.layer_count; ++i) {
        auto const& lowered = data.layers[i];
        BackgroundLayerData layer;
        assemble_coordinated_layer(layer, lowered, context, PropertyID::BackgroundImage, i);
        layer.attachment = static_cast<BackgroundAttachment>(lowered.attachment);
        layer.blend_mode = static_cast<MixBlendMode>(lowered.blend_mode);
        layer.clip = static_cast<BackgroundBox>(lowered.clip);
        layer.origin = static_cast<BackgroundBox>(lowered.origin);
        layers.unchecked_append(move(layer));
    }
    payload.background_layers = move(layers);
}

// Fills the mask group's complex members from the core's pre-lowered
// assembly: the mask layers, the group's mask positions, the first slot's
// mask reference or image, and the clip path.
static void assemble_mask_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::MaskValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiMaskGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    Vector<BackgroundLayerData> layers;
    layers.ensure_capacity(data.layer_count);
    for (size_t i = 0; i < data.layer_count; ++i) {
        auto const& lowered = data.layers[i];
        BackgroundLayerData layer;
        assemble_coordinated_layer(layer, lowered, context, PropertyID::MaskImage, i);
        layer.origin = static_cast<BackgroundBox>(lowered.origin);
        layer.clip = static_cast<BackgroundBox>(lowered.clip);
        layer.mask_clip_is_no_clip = lowered.mask_clip_is_no_clip;
        if (!lowered.mask_clip_is_no_clip)
            layer.mask_clip = static_cast<CoordBox>(lowered.mask_clip);
        layer.mask_composite = static_cast<CompositingOperator>(lowered.mask_composite);
        layer.mask_mode = static_cast<MaskingMode>(lowered.mask_mode);
        layer.mask_origin = static_cast<CoordBox>(lowered.mask_origin);
        layers.unchecked_append(move(layer));
    }
    payload.mask_layers = move(layers);

    Vector<Position> positions;
    positions.ensure_capacity(data.position_count);
    for (size_t i = 0; i < data.position_count; ++i) {
        auto const& lowered = data.positions[i];
        positions.unchecked_append(Position {
            .offset_x = LengthPercentage::from_style_value(adopt_assembly_handle(lowered.x_offset).release_nonnull()),
            .offset_y = LengthPercentage::from_style_value(adopt_assembly_handle(lowered.y_offset).release_nonnull()),
        });
    }
    payload.mask_positions = move(positions);

    if (data.mask_reference_url) {
        auto url_value = adopt_assembly_handle(data.mask_reference_url).release_nonnull();
        payload.mask = MaskReference { url_value->as_url().url() };
    }
    if (data.first_image_is_abstract_image && !payload.mask_layers.is_empty())
        payload.mask_image = payload.mask_layers[0].background_image;

    if (data.clip_path_kind == 1) {
        auto url_value = adopt_assembly_handle(data.clip_path).release_nonnull();
        payload.clip_path = ClipPathReference { url_value->as_url().url() };
    } else if (data.clip_path_kind == 2) {
        auto shape_value = adopt_assembly_handle(data.clip_path).release_nonnull();
        payload.clip_path = ClipPathReference { shape_value->as_basic_shape() };
    }
}

// Fills the border group's complex members from the core's pre-lowered
// assembly: the sides' line styles and used widths (the computed widths and
// resolved colors arrive through the field descriptors), the corner radii,
// and the border-image. A side color the core could not resolve takes the
// C++ resolution arm.
static void assemble_border_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::BorderValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiBorderGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    auto side = [&](BorderData& border, CSSPixels computed_width, ComputedValuesFFI::FfiBorderSideAssembly const& lowered, PropertyID color_property) {
        border.line_style = static_cast<LineStyle>(lowered.line_style);
        if (!lowered.color_resolved)
            border.color = context.computed_style.color(color_property, context.color_resolution_context);
        // If the border-style corresponding to a given border-width is none or hidden, then the used width is 0.
        // https://drafts.csswg.org/css-backgrounds/#border-width
        if (border.line_style != LineStyle::None && border.line_style != LineStyle::Hidden)
            border.width = computed_width;
    };
    side(payload.border_left, payload.border_left_computed_width, data.left, PropertyID::BorderLeftColor);
    side(payload.border_top, payload.border_top_computed_width, data.top, PropertyID::BorderTopColor);
    side(payload.border_right, payload.border_right_computed_width, data.right, PropertyID::BorderRightColor);
    side(payload.border_bottom, payload.border_bottom_computed_width, data.bottom, PropertyID::BorderBottomColor);

    auto radius = [&](void const* handle) {
        auto value = adopt_assembly_handle(handle).release_nonnull();
        return BorderRadiusData {
            LengthPercentage::from_style_value(value->as_border_radius().horizontal_radius()),
            LengthPercentage::from_style_value(value->as_border_radius().vertical_radius()),
        };
    };
    payload.border_bottom_left_radius = radius(data.radii[0]);
    payload.border_bottom_right_radius = radius(data.radii[1]);
    payload.border_top_left_radius = radius(data.radii[2]);
    payload.border_top_right_radius = radius(data.radii[3]);
    payload.has_noninitial_border_radii = !payload.border_bottom_left_radius.is_initial()
        || !payload.border_bottom_right_radius.is_initial()
        || !payload.border_top_left_radius.is_initial()
        || !payload.border_top_right_radius.is_initial();

    BorderImageData border_image;
    if (data.border_image_source_is_abstract_image) {
        RefPtr<StyleValue const> source_wrapper;
        if (data.border_image_source_needs_stamped_wrapper)
            source_wrapper = context.computed_style.property(PropertyID::BorderImageSource);
        else
            source_wrapper = adopt_assembly_handle(data.border_image_source);
        border_image.source = source_wrapper->as_abstract_image();
    }
    auto slice_value = [&](ComputedValuesFFI::FfiBorderImageSlotAssembly const& slot) -> BorderImageSliceValue {
        if (slot.kind == 0)
            return slot.number;
        auto value = adopt_assembly_handle(slot.value).release_nonnull();
        if (value->is_percentage())
            return value->as_percentage().percentage();
        return NonnullRefPtr<CalculatedStyleValue const> { value->as_calculated() };
    };
    auto width_value = [&](ComputedValuesFFI::FfiBorderImageSlotAssembly const& slot) -> BorderImageWidthValue {
        if (slot.kind == 0)
            return slot.number;
        if (slot.kind == 2)
            return BorderImageWidthAuto {};
        return LengthPercentage::from_style_value(adopt_assembly_handle(slot.value).release_nonnull());
    };
    auto outset_value = [&](ComputedValuesFFI::FfiBorderImageSlotAssembly const& slot) -> BorderImageOutsetValue {
        if (slot.kind == 0)
            return slot.number;
        return Length::from_style_value(adopt_assembly_handle(slot.value).release_nonnull(), {});
    };
    border_image.slice = { slice_value(data.slice[0]), slice_value(data.slice[1]), slice_value(data.slice[2]), slice_value(data.slice[3]) };
    border_image.width = { width_value(data.width[0]), width_value(data.width[1]), width_value(data.width[2]), width_value(data.width[3]) };
    border_image.outset = { outset_value(data.outset[0]), outset_value(data.outset[1]), outset_value(data.outset[2]), outset_value(data.outset[3]) };
    border_image.width_value_count = data.width_value_count;
    border_image.outset_value_count = data.outset_value_count;
    border_image.fill = data.slice_fill;
    border_image.repeat_x = static_cast<BorderImageRepeat>(data.border_image_repeat_x);
    border_image.repeat_y = static_cast<BorderImageRepeat>(data.border_image_repeat_y);
    payload.border_image = move(border_image);
}

// Fills the inherited SVG group's complex members from the core's
// pre-lowered assembly: the fill and stroke paints, the dash array, the
// stroke width and offset, the paint order and the dominant baseline. A
// paint the core could not resolve takes the C++ resolution arm.
static void assemble_inherited_svg_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::InheritedSVGValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiInheritedSvgGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    auto paint = [&](ComputedValuesFFI::FfiSvgPaintAssembly const& lowered) -> Optional<SVGPaint> {
        switch (lowered.kind) {
        case 0:
            return {};
        case 1:
            if (lowered.is_url) {
                auto url_value = adopt_assembly_handle(lowered.url).release_nonnull();
                Optional<Color> fallback_color;
                if (lowered.has_color)
                    fallback_color = Color::from_bgra(lowered.color);
                return SVGPaint { url_value->as_url().url(), fallback_color, lowered.color_is_currentcolor };
            }
            return SVGPaint { Color::from_bgra(lowered.color), lowered.color_is_currentcolor };
        default:
            return SVGPaint::from_style_value(adopt_assembly_handle(lowered.value).release_nonnull(), context.color_resolution_context);
        }
    };
    payload.fill = paint(data.fill);
    payload.stroke = paint(data.stroke);

    Vector<Variant<LengthPercentage, float>> dashes;
    dashes.ensure_capacity(data.dash_count);
    for (size_t i = 0; i < data.dash_count; ++i) {
        auto const& item = data.dashes[i];
        if (item.is_number)
            dashes.unchecked_append(static_cast<float>(item.number));
        else
            dashes.unchecked_append(LengthPercentage::from_style_value(adopt_assembly_handle(item.value).release_nonnull()));
    }
    payload.stroke_dasharray = move(dashes);

    auto length_percentage_or_number = [](ComputedValuesFFI::FfiLengthPercentageOrNumberAssembly const& lowered) -> LengthPercentage {
        // FIXME: Converting to pixels isn't really correct - values should be in "user units"
        //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
        if (lowered.is_number)
            return Length::make_px(CSSPixels::nearest_value_for(lowered.number));
        return LengthPercentage::from_style_value(adopt_assembly_handle(lowered.value).release_nonnull());
    };
    payload.stroke_dashoffset = length_percentage_or_number(data.stroke_dashoffset);
    payload.stroke_width = length_percentage_or_number(data.stroke_width);

    payload.paint_order = PaintOrderList {
        static_cast<PaintOrder>(data.paint_order[0]),
        static_cast<PaintOrder>(data.paint_order[1]),
        static_cast<PaintOrder>(data.paint_order[2]),
    };
    payload.paint_order_serialization_length = data.paint_order_serialization_length;
    payload.paint_order_is_normal = data.paint_order_is_normal;

    if (data.has_dominant_baseline)
        payload.dominant_baseline = static_cast<BaselineMetric>(data.dominant_baseline);
    else
        payload.dominant_baseline = {};
}

// Fills the inherited list group's complex members from the core's
// pre-lowered assembly. The list-style-type stays with the C++ arm because
// its counter styles resolve against the style scope, and a url() list image
// mints its wrapper through the stamped property() path.
static void assemble_inherited_list_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::InheritedListValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiInheritedListGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    auto list_style_type = context.computed_style.list_style_type(context.style_scope);
    auto const& list_style_type_value = context.computed_style.property(PropertyID::ListStyleType);
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
    payload.list_style_type = move(list_style_type);

    if (data.image_kind == 1)
        payload.list_style_image = context.computed_style.property(PropertyID::ListStyleImage).as_abstract_image();
    else if (data.image_kind == 2)
        payload.list_style_image = adopt_assembly_handle(data.image).release_nonnull()->as_abstract_image();
    else
        payload.list_style_image = nullptr;

    QuotesData quotes { .type = QuotesData::Type::Auto };
    if (data.quotes_kind == 1) {
        quotes.type = QuotesData::Type::None;
    } else if (data.quotes_kind == 2) {
        quotes.type = QuotesData::Type::Specified;
        VERIFY(data.quote_string_count % 2 == 0);
        for (size_t i = 0; i < data.quote_string_count; i += 2) {
            quotes.strings.empend(
                Utf16FlyString::from_raw(data.quote_strings[i]),
                Utf16FlyString::from_raw(data.quote_strings[i + 1]));
        }
    }
    payload.quotes = move(quotes);
}

// Fills the content group's complex members from the core's pre-lowered
// assembly: the counter definitions arrive as plain data, and a content list
// transcribes through the C++ arm, whose items are wrapper-typed and whose
// images mint through the stamped property() path.
static void assemble_content_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::ContentValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiContentGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    if (data.content_kind == 1) {
        payload.computed_content.type = ComputedContentData::Type::None;
    } else if (data.content_kind == 2) {
        ComputedContentData computed_content;
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
        auto const& content_style_value = context.computed_style.property(PropertyID::Content).as_content();
        for (auto const& item : content_style_value.content().values())
            append_item(item, computed_content.items);
        if (auto const* alt_text = content_style_value.alt_text()) {
            for (auto const& item : alt_text->values())
                append_item(item, computed_content.alt_text);
        }
        payload.computed_content = move(computed_content);
    }

    auto counter_vector = [](ComputedValuesFFI::FfiCounterDataAssembly const* lowered, size_t count) {
        Vector<CounterData, 0> counters;
        counters.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i) {
            CounterData counter {
                .name = Utf16FlyString::from_raw(lowered[i].name_raw),
                .is_reversed = lowered[i].is_reversed,
                .value = {},
            };
            if (lowered[i].has_value)
                counter.value = lowered[i].value;
            counters.unchecked_append(move(counter));
        }
        return counters;
    };
    payload.counter_increment = counter_vector(data.counter_increment, data.counter_increment_count);
    payload.counter_reset = counter_vector(data.counter_reset, data.counter_reset_count);
    payload.counter_set = counter_vector(data.counter_set, data.counter_set_count);
}

// Fills the inherited UI group's complex members from the core's pre-lowered
// assembly: the cursor list, the caret and accent computed halves (the used
// values arrive through the field descriptors), the scrollbar colors and the
// color-scheme names. Colors the core could not resolve take the C++
// resolution arm.
static void assemble_inherited_ui_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::InheritedUIValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiInheritedUiGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    auto color_or_auto = [](ColorOrAuto& slot, bool is_auto, bool resolved, auto resolve) {
        if (!resolved)
            slot.used_value = resolve();
        if (!is_auto)
            slot.computed_value = slot.used_value;
    };
    color_or_auto(payload.caret_color, data.caret_is_auto, data.caret_resolved,
        [&] { return context.computed_style.caret_color(context.color_resolution_context); });
    color_or_auto(payload.accent_color, data.accent_is_auto, data.accent_resolved,
        [&] { return context.computed_style.accent_color(context.color_resolution_context); });

    Vector<CursorData> cursors;
    cursors.ensure_capacity(data.cursor_count);
    for (size_t i = 0; i < data.cursor_count; ++i) {
        auto const& item = data.cursors[i];
        if (item.is_cursor_value)
            cursors.unchecked_append(CursorData { NonnullRefPtr<CursorStyleValue const> { adopt_assembly_handle(item.cursor).release_nonnull()->as_cursor() } });
        else
            cursors.unchecked_append(static_cast<CursorPredefined>(item.predefined));
    }
    payload.cursor = move(cursors);

    if (data.scrollbar_color_kind == 1) {
        payload.scrollbar_color = ScrollbarColorData {
            .thumb_color = Color::from_bgra(data.scrollbar_thumb_color),
            .track_color = Color::from_bgra(data.scrollbar_track_color),
            .is_auto = false,
        };
    } else if (data.scrollbar_color_kind == 2) {
        payload.scrollbar_color = context.computed_style.scrollbar_color(context.color_resolution_context);
    }

    Vector<Utf16FlyString> schemes;
    schemes.ensure_capacity(data.color_scheme_count);
    for (size_t i = 0; i < data.color_scheme_count; ++i)
        schemes.unchecked_append(Utf16FlyString::from_raw(data.color_schemes[i]));
    payload.color_schemes = move(schemes);
    payload.color_scheme_only = data.color_scheme_only;
}

// Fills the inherited text group's complex members from the core's
// pre-lowered assembly: the text shadows arrive as plain data, the indent
// and underline offset keep their length-percentage slots, and a fill color
// the core could not resolve takes the C++ resolution arm.
static void assemble_inherited_text_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::InheritedTextValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiInheritedTextGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    if (!data.webkit_text_fill_color_resolved)
        payload.webkit_text_fill_color = context.computed_style.property(PropertyID::WebkitTextFillColor).to_color(context.color_resolution_context).value();
    if (!data.word_spacing_resolved)
        payload.word_spacing = context.computed_style.word_spacing();
    if (!data.letter_spacing_resolved)
        payload.letter_spacing = context.computed_style.letter_spacing();

    Vector<ShadowData> shadows;
    shadows.ensure_capacity(data.text_shadow_count);
    for (size_t i = 0; i < data.text_shadow_count; ++i) {
        auto const& shadow = data.text_shadows[i];
        shadows.unchecked_append(ShadowData {
            CSSPixels::from_raw(shadow.offset_x),
            CSSPixels::from_raw(shadow.offset_y),
            CSSPixels::from_raw(shadow.blur_radius),
            CSSPixels::from_raw(shadow.spread_distance),
            Color::from_bgra(shadow.color),
            static_cast<ColorSyntax>(shadow.color_syntax),
            static_cast<ShadowPlacement>(shadow.placement),
        });
    }
    payload.text_shadow = move(shadows);

    payload.text_underline_position = TextUnderlinePosition {
        .horizontal = static_cast<TextUnderlinePositionHorizontal>(data.underline_position_horizontal),
        .vertical = static_cast<TextUnderlinePositionVertical>(data.underline_position_vertical),
    };

    TextUnderlineOffset underline_offset;
    underline_offset.used_value = context.computed_style.text_underline_offset();
    if (!data.underline_offset_is_auto)
        underline_offset.computed_value = LengthPercentage::from_style_value(adopt_assembly_handle(data.underline_offset).release_nonnull());
    payload.text_underline_offset = move(underline_offset);

    payload.text_indent = TextIndentData {
        .length_percentage = LengthPercentage::from_style_value(adopt_assembly_handle(data.text_indent).release_nonnull()),
        .each_line = data.text_indent_each_line,
        .hanging = data.text_indent_hanging,
    };

    payload.tab_size_is_number = data.tab_size_is_number;
    payload.tab_size_length = data.tab_size_is_number ? CSSPixels(0) : CSSPixels::from_raw(data.tab_size_px);
    payload.tab_size_number = data.tab_size_is_number ? data.tab_size_number : 0;
}

// Fills the misc reset group's complex members from the core's pre-lowered
// assembly. The scroll margins and paddings, the outline offset, the shape
// members and the object position keep their wrapper-backed slots; an
// outline color the core could not resolve takes the C++ resolution arm, and
// a non-initial shape-outside transcribes through the stamped wrapper, whose
// images read style sheet context.
static void assemble_misc_reset_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::MiscResetValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiMiscResetGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    if (!data.outline_color_is_auto && !data.outline_color_resolved) {
        if (auto const& outline_color = context.computed_style.property(PropertyID::OutlineColor); outline_color.has_color())
            payload.outline_color = outline_color.to_color(context.color_resolution_context).value();
    }

    auto outline_offset_value = adopt_assembly_handle(data.outline_offset).release_nonnull();
    auto resolved_outline_offset = outline_offset_value->is_calculated()
        ? outline_offset_value->as_calculated().resolve_length(context.color_resolution_context.calculation_resolution_context).value()
        : outline_offset_value->as_length().length();
    payload.outline_offset = resolved_outline_offset.absolute_length_to_px();
    payload.outline_offset_style_value = move(outline_offset_value);

    auto length_box = [](void const* const(&handles)[4], LengthPercentageOrAuto const& default_value) {
        auto side = [&](void const* handle) -> LengthPercentageOrAuto {
            auto value = adopt_assembly_handle(handle).release_nonnull();
            if (value->is_calculated() || value->is_percentage() || value->is_length() || value->has_auto())
                return LengthPercentageOrAuto::from_style_value(value);
            // FIXME: Handle anchor sizes
            return default_value;
        };
        // Top, right, bottom, left.
        return LengthBox { side(handles[0]), side(handles[1]), side(handles[2]), side(handles[3]) };
    };
    payload.scroll_margin = length_box(data.scroll_margin, Length::make_px(0));
    payload.scroll_padding = length_box(data.scroll_padding, LengthPercentageOrAuto::make_auto());

    auto overflow_clip_margin_side = [&](ComputedValuesFFI::FfiOverflowClipMarginSideAssembly const& lowered) -> OverflowClipMarginSide {
        if (!lowered.offset)
            return {};
        OverflowClipMarginSide side;
        if (lowered.has_visual_box)
            side.visual_box = static_cast<BackgroundBox>(lowered.visual_box);
        auto offset_value = adopt_assembly_handle(lowered.offset).release_nonnull();
        if (offset_value->is_calculated())
            side.offset = offset_value->as_calculated().resolve_length(context.color_resolution_context.calculation_resolution_context).value().absolute_length_to_px();
        else if (offset_value->is_length())
            side.offset = offset_value->as_length().length().absolute_length_to_px();
        return side;
    };
    payload.overflow_clip_margin = OverflowClipMarginData {
        .left = overflow_clip_margin_side(data.overflow_clip_margin[0]),
        .top = overflow_clip_margin_side(data.overflow_clip_margin[1]),
        .right = overflow_clip_margin_side(data.overflow_clip_margin[2]),
        .bottom = overflow_clip_margin_side(data.overflow_clip_margin[3]),
    };

    payload.appearance = static_cast<Appearance>(data.appearance);
    payload.computed_appearance = static_cast<Appearance>(data.computed_appearance);

    payload.object_position = Position {
        .offset_x = LengthPercentage::from_style_value(adopt_assembly_handle(data.object_position_x).release_nonnull()),
        .offset_y = LengthPercentage::from_style_value(adopt_assembly_handle(data.object_position_y).release_nonnull()),
    };

    if (data.has_view_transition_name)
        payload.view_transition_name = Utf16FlyString::from_raw(data.view_transition_name_raw);

    payload.touch_action = TouchActionData {
        .allow_left = data.touch_action_allow_left,
        .allow_right = data.touch_action_allow_right,
        .allow_up = data.touch_action_allow_up,
        .allow_down = data.touch_action_allow_down,
        .allow_pinch_zoom = data.touch_action_allow_pinch_zoom,
        .allow_other = data.touch_action_allow_other,
    };

    payload.column_height = Size::from_style_value(*adopt_assembly_handle(data.column_height));
    payload.shape_margin = LengthPercentage::from_style_value(adopt_assembly_handle(data.shape_margin).release_nonnull());

    if (data.shape_outside_noninitial) {
        ShapeOutsideData shape_outside;
        auto apply_shape_outside_item = [&](StyleValue const& item) {
            if (item.is_url())
                shape_outside.image = item.as_url().url();
            else if (item.is_abstract_image())
                shape_outside.image = NonnullRefPtr<AbstractImageStyleValue const> { item.as_abstract_image() };
            else if (item.is_basic_shape())
                shape_outside.basic_shape = item.as_basic_shape();
            else if (auto shape_box = keyword_to_shape_box(item.to_keyword()); shape_box.has_value())
                shape_outside.shape_box = *shape_box;
        };
        auto const& shape_outside_value = context.computed_style.property(PropertyID::ShapeOutside);
        if (shape_outside_value.is_value_list()) {
            for (auto const& item : shape_outside_value.as_value_list().values())
                apply_shape_outside_item(item);
        } else {
            apply_shape_outside_item(shape_outside_value);
        }
        payload.shape_outside = move(shape_outside);
    }

    payload.scrollbar_gutter = static_cast<ScrollbarGutter>(data.scrollbar_gutter);

    if (!data.will_change_is_auto) {
        Vector<WillChange::WillChangeEntry> entries;
        for (size_t i = 0; i < data.will_change_entry_count; ++i) {
            auto const& entry = data.will_change_entries[i];
            if (entry.is_keyword) {
                switch (static_cast<Keyword>(entry.keyword)) {
                case Keyword::Contents:
                    entries.append(WillChange::Type::Contents);
                    break;
                case Keyword::ScrollPosition:
                    entries.append(WillChange::Type::ScrollPosition);
                    break;
                default:
                    VERIFY_NOT_REACHED();
                }
            } else if (auto property_id = property_id_from_string(Utf16FlyString::from_raw(entry.ident_raw)); property_id.has_value()) {
                entries.append(property_id.release_value());
            }
        }
        payload.will_change = WillChange(move(entries));
    }
}

// Fills the animation group's coordinated vectors from the core's pre-lowered
// assembly. Only the values that genuinely need C++ decode here: calc times
// and counts resolve through their style value wrappers, easing entries
// through EasingFunction, and inset edges through LengthPercentageOrAuto.
static void assemble_animation_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::AnimationValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiAnimationGroupAssembly const*>(data_pointer);

    auto time_from_item = [](ComputedValuesFFI::FfiTimeItemAssembly const& item) {
        if (item.is_plain)
            return Time { item.value, static_cast<TimeUnit>(item.unit) };
        return Time::from_style_value(adopt_assembly_handle(item.calculated).release_nonnull(), {});
    };
    auto times = [&](ComputedValuesFFI::FfiTimeItemAssembly const* items, size_t count) {
        Vector<Time> result;
        result.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i)
            result.append(time_from_item(items[i]));
        return result;
    };
    auto codes = [](u8 const* items, size_t count, auto convert) {
        Vector<decltype(convert(0))> result;
        result.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i)
            result.append(convert(items[i]));
        return result;
    };
    auto optional_names = [](ComputedValuesFFI::FfiTimelineNameAssembly const* items, size_t count) {
        Vector<Optional<Utf16FlyString>> result;
        result.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i)
            result.append(items[i].has_name ? Optional<Utf16FlyString> { Utf16FlyString::from_raw(items[i].name_raw) } : Optional<Utf16FlyString> {});
        return result;
    };
    auto easing_functions = [](void const* const* items, size_t count, Vector<EasingFunction>& functions, StyleValueVector& style_values) {
        functions.ensure_capacity(count);
        style_values.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i) {
            auto value = adopt_assembly_handle(items[i]).release_nonnull();
            functions.append(EasingFunction::from_style_value(value));
            style_values.append(move(value));
        }
    };

    Vector<ComputedAnimationName> animation_names;
    animation_names.ensure_capacity(data.name_count);
    for (size_t i = 0; i < data.name_count; ++i) {
        auto const& name = data.names[i];
        if (!name.has_name) {
            animation_names.empend();
        } else {
            animation_names.append(ComputedAnimationName {
                .name = Utf16FlyString::from_raw(name.name_raw),
                .syntax = name.is_string ? ComputedAnimationNameSyntax::String : ComputedAnimationNameSyntax::CustomIdent,
            });
        }
    }
    payload.animation_names = move(animation_names);

    payload.animation_compositions = codes(data.compositions, data.composition_count, [](u8 code) { return static_cast<AnimationComposition>(code); });
    payload.animation_delays = times(data.delays, data.delay_count);
    payload.animation_directions = codes(data.directions, data.direction_count, [](u8 code) { return static_cast<AnimationDirection>(code); });

    Vector<Optional<Time>> animation_durations;
    animation_durations.ensure_capacity(data.duration_count);
    for (size_t i = 0; i < data.duration_count; ++i) {
        auto const& duration = data.durations[i];
        animation_durations.append(duration.is_auto ? Optional<Time> {} : Optional<Time> { time_from_item(duration.time) });
    }
    payload.animation_durations = move(animation_durations);

    payload.animation_fill_modes = codes(data.fill_modes, data.fill_mode_count, [](u8 code) { return static_cast<AnimationFillMode>(code); });

    Vector<double> animation_iteration_counts;
    animation_iteration_counts.ensure_capacity(data.iteration_count_count);
    for (size_t i = 0; i < data.iteration_count_count; ++i) {
        auto const& count = data.iteration_counts[i];
        if (count.is_plain)
            animation_iteration_counts.append(count.number);
        else
            animation_iteration_counts.append(number_from_style_value(adopt_assembly_handle(count.value).release_nonnull(), {}));
    }
    payload.animation_iteration_counts = move(animation_iteration_counts);

    payload.animation_play_states = codes(data.play_states, data.play_state_count, [](u8 code) { return static_cast<AnimationPlayState>(code); });

    Vector<AnimationTimelineData> animation_timelines;
    animation_timelines.ensure_capacity(data.timeline_count);
    for (size_t i = 0; i < data.timeline_count; ++i) {
        auto const& lowered = data.timelines[i];
        AnimationTimelineData timeline;
        timeline.type = static_cast<AnimationTimelineData::Type>(lowered.kind);
        if (timeline.type == AnimationTimelineData::Type::Name)
            timeline.name = Utf16FlyString::from_raw(lowered.name_raw);
        if (lowered.has_scroller)
            timeline.scroller = static_cast<Scroller>(lowered.scroller);
        if (lowered.has_axis)
            timeline.axis = static_cast<Axis>(lowered.axis);
        if (lowered.has_inset) {
            timeline.inset = {
                .start = LengthPercentageOrAuto::from_style_value(adopt_assembly_handle(lowered.inset_start).release_nonnull()),
                .end = LengthPercentageOrAuto::from_style_value(adopt_assembly_handle(lowered.inset_end).release_nonnull()),
            };
        }
        animation_timelines.append(move(timeline));
    }
    payload.animation_timelines = move(animation_timelines);

    Vector<EasingFunction> animation_timing_functions;
    StyleValueVector animation_timing_function_style_values;
    easing_functions(data.timing_functions, data.timing_function_count, animation_timing_functions, animation_timing_function_style_values);
    payload.animation_timing_functions = move(animation_timing_functions);
    payload.animation_timing_function_style_values = move(animation_timing_function_style_values);

    payload.scroll_timeline_names = optional_names(data.scroll_timeline_names, data.scroll_timeline_name_count);
    payload.scroll_timeline_axes = codes(data.scroll_timeline_axes, data.scroll_timeline_axis_count, [](u8 code) { return static_cast<Axis>(code); });

    Vector<Utf16FlyString> timeline_scope_names;
    timeline_scope_names.ensure_capacity(data.timeline_scope_name_count);
    for (size_t i = 0; i < data.timeline_scope_name_count; ++i)
        timeline_scope_names.append(Utf16FlyString::from_raw(data.timeline_scope_names[i]));
    payload.timeline_scope = TimelineScopeData {
        .all = data.timeline_scope_all,
        .names = move(timeline_scope_names),
    };

    payload.view_timeline_names = optional_names(data.view_timeline_names, data.view_timeline_name_count);
    payload.view_timeline_axes = codes(data.view_timeline_axes, data.view_timeline_axis_count, [](u8 code) { return static_cast<Axis>(code); });

    Vector<ViewTimelineInsetData> view_timeline_insets;
    view_timeline_insets.ensure_capacity(data.view_timeline_inset_count);
    for (size_t i = 0; i < data.view_timeline_inset_count; ++i) {
        auto const& inset = data.view_timeline_insets[i];
        view_timeline_insets.append({
            .start = LengthPercentageOrAuto::from_style_value(adopt_assembly_handle(inset.start).release_nonnull()),
            .end = LengthPercentageOrAuto::from_style_value(adopt_assembly_handle(inset.end).release_nonnull()),
        });
    }
    payload.view_timeline_insets = move(view_timeline_insets);

    payload.transition_properties = optional_names(data.transition_properties, data.transition_property_count);
    payload.transition_durations = times(data.transition_durations, data.transition_duration_count);

    Vector<EasingFunction> transition_timing_functions;
    StyleValueVector transition_timing_function_style_values;
    easing_functions(data.transition_timing_functions, data.transition_timing_function_count, transition_timing_functions, transition_timing_function_style_values);
    payload.transition_timing_functions = move(transition_timing_functions);
    payload.transition_timing_function_style_values = move(transition_timing_function_style_values);

    payload.transition_delays = times(data.transition_delays, data.transition_delay_count);
    payload.transition_behaviors = codes(data.transition_behaviors, data.transition_behavior_count, [](u8 code) { return static_cast<TransitionBehavior>(code); });
}

// Fills the text reset group's members the pokes cannot carry from the core's
// pre-lowered assembly; a color the core could not resolve decodes here
// through the wrapper path.
static void assemble_text_reset_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::TextResetValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiTextResetGroupAssembly const*>(data_pointer);
    auto const& context = *static_cast<TableGroupAssemblerContext const*>(data.cpp_context);

    Vector<TextDecorationLine> lines;
    lines.ensure_capacity(data.text_decoration_line_count);
    for (size_t i = 0; i < data.text_decoration_line_count; ++i)
        lines.append(static_cast<TextDecorationLine>(data.text_decoration_lines[i]));
    payload.text_decoration_line = move(lines);

    switch (data.text_decoration_thickness_kind) {
    case 0:
        payload.text_decoration_thickness = TextDecorationThickness { TextDecorationThickness::Auto {} };
        break;
    case 1:
        payload.text_decoration_thickness = TextDecorationThickness { TextDecorationThickness::FromFont {} };
        break;
    default:
        payload.text_decoration_thickness = TextDecorationThickness { LengthPercentage::from_style_value(adopt_assembly_handle(data.text_decoration_thickness).release_nonnull()) };
        break;
    }

    if (!data.text_decoration_color_resolved)
        payload.text_decoration_color = context.computed_style.color(PropertyID::TextDecorationColor, context.color_resolution_context);

    payload.white_space_trim = WhiteSpaceTrimData {
        .discard_before = data.white_space_trim_discard_before,
        .discard_after = data.white_space_trim_discard_after,
        .discard_inner = data.white_space_trim_discard_inner,
    };
}

// Fills the anchor group's members from the core's pre-lowered assembly; the
// name raws are borrowed and intern fresh references here.
static void assemble_anchor_group_payload(void* payload_pointer, void const* data_pointer)
{
    auto& payload = *static_cast<ComputedValues::AnchorValues*>(payload_pointer);
    auto const& data = *static_cast<ComputedValuesFFI::FfiAnchorGroupAssembly const*>(data_pointer);

    auto fly_strings = [](size_t const* raws, size_t count) {
        Vector<Utf16FlyString> names;
        names.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i)
            names.append(Utf16FlyString::from_raw(raws[i]));
        return names;
    };
    auto position_area_keywords = [](u8 const* codes, size_t count) {
        Vector<PositionArea> keywords;
        keywords.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i)
            keywords.append(static_cast<PositionArea>(codes[i]));
        return keywords;
    };

    payload.anchor_names = fly_strings(data.anchor_names, data.anchor_name_count);
    payload.anchor_scope = AnchorScopeData {
        .all = data.anchor_scope_all,
        .names = fly_strings(data.anchor_scope_names, data.anchor_scope_name_count),
    };

    PositionAnchor position_anchor;
    position_anchor.type = static_cast<PositionAnchor::Type>(data.position_anchor_type);
    if (position_anchor.type == PositionAnchor::Type::Name)
        position_anchor.name = Utf16FlyString::from_raw(data.position_anchor_name_raw);
    payload.position_anchor = move(position_anchor);

    payload.position_area = PositionAreaData {
        .keywords = position_area_keywords(data.position_area_keywords, data.position_area_keyword_count),
    };

    Vector<PositionTryFallbackData> fallbacks;
    fallbacks.ensure_capacity(data.position_try_fallback_count);
    for (size_t i = 0; i < data.position_try_fallback_count; ++i) {
        auto const& lowered = data.position_try_fallbacks[i];
        PositionTryFallbackData fallback;
        if (lowered.has_name)
            fallback.name = Utf16FlyString::from_raw(lowered.name_raw);
        for (size_t tactic = 0; tactic < lowered.tactic_count; ++tactic)
            fallback.tactics.append(static_cast<TryTactic>(lowered.tactics[tactic]));
        if (lowered.has_position_area) {
            fallback.position_area = PositionAreaData {
                .keywords = position_area_keywords(lowered.position_area_keywords, lowered.position_area_keyword_count),
            };
        }
        fallbacks.append(move(fallback));
    }
    payload.position_try_fallbacks = move(fallbacks);

    payload.position_try_order = data.has_position_try_order
        ? Optional<TryOrder> { static_cast<TryOrder>(data.position_try_order) }
        : Optional<TryOrder> {};

    payload.position_visibility = PositionVisibilityData {
        .always = data.position_visibility_always,
        .anchors_valid = data.position_visibility_anchors_valid,
        .anchors_visible = data.position_visibility_anchors_visible,
        .no_overflow = data.position_visibility_no_overflow,
    };
}

NonnullRefPtr<ComputedValues const> ComputedValues::create(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const* inherit_parent)
{
    return create_internal(computed_style, document, style_scope, move(color_resolution_context), inherit_parent, nullptr, all_style_groups);
}

NonnullRefPtr<ComputedValues const> ComputedValues::create_over_base(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const& base, u32 groups_to_apply)
{
    return create_internal(computed_style, document, style_scope, move(color_resolution_context), nullptr, &base, groups_to_apply);
}

NonnullRefPtr<ComputedValues const> ComputedValues::create_internal(ComputedStyleWorkingSet const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context, ComputedValues const* inherit_parent, ComputedValues const* base, u32 groups_to_apply)
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
    // Effective values living outside the table - the animated overlay and the partial-drive
    // specified-value preferences - travel as a sparse override span, so the build sees exactly
    // the values property() returns.
    auto const* longhand_table = computed_style.computed_longhand_table();
    Vector<u16> override_properties;
    Vector<void const*> override_values;
    computed_style.collect_effective_longhand_overrides(override_properties, override_values);
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_context_storage;
    auto ffi_color_input = make_rust_color_resolution_input(color_resolution_context, length_context_storage);
    TableGroupAssemblerContext assembler_context {
        .computed_style = computed_style,
        .style_scope = style_scope,
        .color_resolution_context = color_resolution_context,
    };
    ComputedValuesFFI::FfiTableGroupBuildInputs table_build_inputs {
        .color_input = &ffi_color_input,
        .used_color_scheme = static_cast<u8>(to_underlying(color_scheme)),
        .override_properties = override_properties.data(),
        .override_values = override_values.data(),
        .override_count = override_properties.size(),
        .box_display_before_transformation_raw = bit_cast<u32>(computed_style.display_before_box_type_transformation()),
        .cpp_assembler_context = &assembler_context,
    };
    Array<void const*, to_underlying(StyleGroupIndex::Count)> parent_group_payloads {};
    if (inherit_parent) {
        for (size_t group = 0; group < parent_group_payloads.size(); ++group)
            parent_group_payloads[group] = inherit_parent->style_group_payload(static_cast<StyleGroupIndex>(group));
    }
    Array<void const*, to_underlying(StyleGroupIndex::Count)> table_group_payloads {};
    ComputedValuesFFI::rust_build_group_payloads_from_table(longhand_table, groups_to_apply, parent_group_payloads.data(), &table_build_inputs, table_group_payloads.data(), table_group_payloads.size());
    auto table_group_payload = [&](StyleGroupIndex group) { return table_group_payloads[to_underlying(group)]; };

    // A null payload means a value the core cannot map, and the population setters below apply.
    void const* inherited_box_payload = applies(StyleGroupIndex::InheritedBoxValues) ? table_group_payload(StyleGroupIndex::InheritedBoxValues) : nullptr;
    bool const inherited_box_adopted = inherited_box_payload != nullptr || !applies(StyleGroupIndex::InheritedBoxValues);
    if (inherited_box_payload)
        computed_values.adopt_inherited_box_group(const_cast<void*>(inherited_box_payload));

    void const* inherited_table_payload = applies(StyleGroupIndex::InheritedTableValues) ? table_group_payload(StyleGroupIndex::InheritedTableValues) : nullptr;
    bool const inherited_table_adopted = inherited_table_payload != nullptr || !applies(StyleGroupIndex::InheritedTableValues);
    if (inherited_table_payload)
        computed_values.adopt_inherited_table_group(const_cast<void*>(inherited_table_payload));

    void const* content_payload = applies(StyleGroupIndex::ContentValues) ? table_group_payload(StyleGroupIndex::ContentValues) : nullptr;
    VERIFY(content_payload || !applies(StyleGroupIndex::ContentValues));
    if (content_payload)
        computed_values.adopt_content_group(const_cast<void*>(content_payload));

    void const* anchor_payload = applies(StyleGroupIndex::AnchorValues) ? table_group_payload(StyleGroupIndex::AnchorValues) : nullptr;
    VERIFY(anchor_payload || !applies(StyleGroupIndex::AnchorValues));
    if (anchor_payload)
        computed_values.adopt_anchor_group(const_cast<void*>(anchor_payload));

    void const* surround_payload = applies(StyleGroupIndex::SurroundValues) ? table_group_payload(StyleGroupIndex::SurroundValues) : nullptr;
    VERIFY(surround_payload || !applies(StyleGroupIndex::SurroundValues));
    if (surround_payload)
        computed_values.adopt_surround_group(const_cast<void*>(surround_payload));

    void const* box_payload = applies(StyleGroupIndex::BoxValues) ? table_group_payload(StyleGroupIndex::BoxValues) : nullptr;
    VERIFY(box_payload || !applies(StyleGroupIndex::BoxValues));
    if (box_payload)
        computed_values.adopt_box_group(const_cast<void*>(box_payload));

    void const* alignment_payload = applies(StyleGroupIndex::AlignmentValues) ? table_group_payload(StyleGroupIndex::AlignmentValues) : nullptr;
    VERIFY(alignment_payload || !applies(StyleGroupIndex::AlignmentValues));
    if (alignment_payload)
        computed_values.adopt_alignment_group(const_cast<void*>(alignment_payload));

    void const* sizing_payload = applies(StyleGroupIndex::SizingValues) ? table_group_payload(StyleGroupIndex::SizingValues) : nullptr;
    if (sizing_payload)
        computed_values.adopt_sizing_group(const_cast<void*>(sizing_payload));

    void const* grid_payload = applies(StyleGroupIndex::GridValues) ? table_group_payload(StyleGroupIndex::GridValues) : nullptr;
    VERIFY(grid_payload || !applies(StyleGroupIndex::GridValues));
    if (grid_payload)
        computed_values.adopt_grid_group(const_cast<void*>(grid_payload));

    void const* mask_payload = applies(StyleGroupIndex::MaskValues) ? table_group_payload(StyleGroupIndex::MaskValues) : nullptr;
    VERIFY(mask_payload || !applies(StyleGroupIndex::MaskValues));
    if (mask_payload)
        computed_values.adopt_mask_group(const_cast<void*>(mask_payload));

    void const* transform_payload = applies(StyleGroupIndex::TransformValues) ? table_group_payload(StyleGroupIndex::TransformValues) : nullptr;
    VERIFY(transform_payload || !applies(StyleGroupIndex::TransformValues));
    if (transform_payload)
        computed_values.adopt_transform_group(const_cast<void*>(transform_payload));

    void const* effects_payload = applies(StyleGroupIndex::EffectsValues) ? table_group_payload(StyleGroupIndex::EffectsValues) : nullptr;
    VERIFY(effects_payload || !applies(StyleGroupIndex::EffectsValues));
    if (effects_payload)
        computed_values.adopt_effects_group(const_cast<void*>(effects_payload));

    // NOTE: We have to be careful that font-related properties get set in the right order.
    //       m_font is used by Length::to_px() when resolving sizes against this layout node.
    //       That's why it has to be set before everything else.
    if (applies(StyleGroupIndex::FontValues)) {
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
        computed_values.set_line_height(computed_style.line_height_data(), computed_style.line_height(document.font_computer()));
        computed_values.set_font_variant_emoji(computed_style.font_variant_emoji());
    }

    void const* animation_payload = applies(StyleGroupIndex::AnimationValues) ? table_group_payload(StyleGroupIndex::AnimationValues) : nullptr;
    VERIFY(animation_payload || !applies(StyleGroupIndex::AnimationValues));
    if (animation_payload)
        computed_values.adopt_animation_group(const_cast<void*>(animation_payload));

    void const* inherited_list_payload = applies(StyleGroupIndex::InheritedListValues) ? table_group_payload(StyleGroupIndex::InheritedListValues) : nullptr;
    VERIFY(inherited_list_payload || !applies(StyleGroupIndex::InheritedListValues));
    if (inherited_list_payload)
        computed_values.adopt_inherited_list_group(const_cast<void*>(inherited_list_payload));

    void const* inherited_svg_payload = applies(StyleGroupIndex::InheritedSVGValues) ? table_group_payload(StyleGroupIndex::InheritedSVGValues) : nullptr;
    VERIFY(inherited_svg_payload || !applies(StyleGroupIndex::InheritedSVGValues));
    if (inherited_svg_payload)
        computed_values.adopt_inherited_svg_group(const_cast<void*>(inherited_svg_payload));

    void const* svg_reset_payload = nullptr;
    if (applies(StyleGroupIndex::SVGResetValues)) {
        auto build_marshalled_svg_reset_payload = [&] {
            return ComputedValuesFFI::rust_build_svg_reset_group(
                SVGResetValues::style_group_index,
                computed_style.property(PropertyID::Cx).rust_style_value_data(),
                computed_style.property(PropertyID::Cy).rust_style_value_data(),
                computed_style.property(PropertyID::D).rust_style_value_data(),
                computed_style.property(PropertyID::R).rust_style_value_data(),
                computed_style.property(PropertyID::Rx).rust_style_value_data(),
                computed_style.property(PropertyID::Ry).rust_style_value_data(),
                computed_style.property(PropertyID::X).rust_style_value_data(),
                computed_style.property(PropertyID::Y).rust_style_value_data(),
                computed_style.color(PropertyID::StopColor, color_resolution_context).value(),
                computed_style.stop_opacity(),
                computed_style.color(PropertyID::FloodColor, color_resolution_context).value(),
                computed_style.flood_opacity(),
                computed_style.property(PropertyID::VectorEffect).rust_style_value_data(),
                inherit_parent ? static_cast<void const*>(inherit_parent->m_noninherited.svg_reset.operator->()) : nullptr);
        };
        // The table build declines stop and flood colors the core cannot resolve; the marshalled
        // build resolves them through the C++ fallback arms and remains the fallback.
        svg_reset_payload = table_group_payload(StyleGroupIndex::SVGResetValues);
        if (!svg_reset_payload)
            svg_reset_payload = build_marshalled_svg_reset_payload();
    }
    VERIFY(svg_reset_payload || !applies(StyleGroupIndex::SVGResetValues));
    if (svg_reset_payload)
        computed_values.adopt_svg_reset_group(const_cast<void*>(svg_reset_payload));

    void const* background_payload = applies(StyleGroupIndex::BackgroundValues) ? table_group_payload(StyleGroupIndex::BackgroundValues) : nullptr;
    VERIFY(background_payload || !applies(StyleGroupIndex::BackgroundValues));
    if (background_payload)
        computed_values.adopt_background_group(const_cast<void*>(background_payload));

    void const* border_payload = applies(StyleGroupIndex::BorderValues) ? table_group_payload(StyleGroupIndex::BorderValues) : nullptr;
    VERIFY(border_payload || !applies(StyleGroupIndex::BorderValues));
    if (border_payload)
        computed_values.adopt_border_group(const_cast<void*>(border_payload));

    void const* inherited_ui_payload = applies(StyleGroupIndex::InheritedUIValues) ? table_group_payload(StyleGroupIndex::InheritedUIValues) : nullptr;
    VERIFY(inherited_ui_payload || !applies(StyleGroupIndex::InheritedUIValues));
    if (inherited_ui_payload)
        computed_values.adopt_inherited_ui_group(const_cast<void*>(inherited_ui_payload));

    void const* inherited_text_payload = applies(StyleGroupIndex::InheritedTextValues) ? table_group_payload(StyleGroupIndex::InheritedTextValues) : nullptr;
    VERIFY(inherited_text_payload || !applies(StyleGroupIndex::InheritedTextValues));
    if (inherited_text_payload)
        computed_values.adopt_inherited_text_group(const_cast<void*>(inherited_text_payload));

    void const* text_reset_payload = applies(StyleGroupIndex::TextResetValues) ? table_group_payload(StyleGroupIndex::TextResetValues) : nullptr;
    VERIFY(text_reset_payload || !applies(StyleGroupIndex::TextResetValues));
    if (text_reset_payload)
        computed_values.adopt_text_reset_group(const_cast<void*>(text_reset_payload));

    void const* misc_reset_payload = applies(StyleGroupIndex::MiscResetValues) ? table_group_payload(StyleGroupIndex::MiscResetValues) : nullptr;
    VERIFY(misc_reset_payload || !applies(StyleGroupIndex::MiscResetValues));
    if (misc_reset_payload)
        computed_values.adopt_misc_reset_group(const_cast<void*>(misc_reset_payload));

    if (auto maybe_font_language_override = computed_style.font_language_override(); maybe_font_language_override.has_value())
        computed_values.set_font_language_override(maybe_font_language_override.release_value());
    computed_values.set_font_variation_settings(computed_style.font_variation_settings());

    if (!inherited_table_adopted) {
        computed_values.set_border_spacing_horizontal(computed_style.border_spacing_horizontal());
        computed_values.set_border_spacing_vertical(computed_style.border_spacing_vertical());
    }

    if (!inherited_table_adopted)
        computed_values.set_caption_side(computed_style.caption_side());
    if (!inherited_box_adopted)
        computed_values.set_content_visibility(computed_style.content_visibility());
    if (!inherited_box_adopted)
        computed_values.set_image_rendering(computed_style.image_rendering());
    if (!inherited_box_adopted)
        computed_values.set_visibility(computed_style.visibility());

    if (!inherited_table_adopted)
        computed_values.set_border_collapse(computed_style.border_collapse());

    if (!inherited_table_adopted)
        computed_values.set_empty_cells(computed_style.empty_cells());

    auto const& math_shift_value = computed_style.property(CSS::PropertyID::MathShift);
    if (auto math_shift = keyword_to_math_shift(math_shift_value.to_keyword()); math_shift.has_value())
        computed_values.set_math_shift(math_shift.value());

    auto const& math_style_value = computed_style.property(CSS::PropertyID::MathStyle);
    if (auto math_style = keyword_to_math_style(math_style_value.to_keyword()); math_style.has_value())
        computed_values.set_math_style(math_style.value());

    computed_values.set_math_depth(computed_style.math_depth());

    if (!inherited_box_adopted)
        computed_values.set_direction(computed_style.direction());
    if (!inherited_box_adopted)
        computed_values.set_writing_mode(computed_style.writing_mode());
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
    computed_values.set_raw_cascaded_font_size(computed_style.raw_cascaded_font_size());
    // The drive records its inheritance-dependent specified values on the table; the style
    // takes owned wrappers, because its own table reference can later be canonicalized onto a
    // value-equal donor table whose recorded values are not this style's.
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> inheritance_dependent_specified_values;
    for (auto const& entry : computed_style.inheritance_dependent_value_span()) {
        inheritance_dependent_specified_values.set(
            static_cast<PropertyID>(entry.property),
            StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(entry.value))));
    }
    computed_values.set_inheritance_dependent_specified_values(move(inheritance_dependent_specified_values));
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
    clear_computed_longhand_table();
    if (!m_is_style_record_view)
        --s_statistics.live_instance_count;
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
    m_longhand_values = { ComputedValuesFFI::rust_computed_longhand_table_values(typed_table), number_of_longhand_properties };
}

void ComputedValues::clear_computed_longhand_table()
{
    if (m_computed_longhand_table)
        ComputedValuesFFI::rust_computed_longhand_table_release(const_cast<ComputedValuesFFI::ComputedLonghandTable*>(static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(m_computed_longhand_table)));
    m_computed_longhand_table = nullptr;
    m_longhand_values = {};
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
    ComputedValuesFFI::rust_computed_longhand_table_freeze(table);
    // The freshly created table already carries the one reference this style owns.
    m_computed_longhand_table = table;
    m_longhand_values = { ComputedValuesFFI::rust_computed_longhand_table_values(table), number_of_longhand_properties };
}

void ComputedValues::adopt_swapped_computed_longhand_table(ComputedValues const& old_values, ComputedValues const& inherited_source)
{
    auto old_longhand_values = old_values.computed_longhand_values();
    auto parent_longhand_values = inherited_source.computed_longhand_values();
    if (old_longhand_values.is_empty() || parent_longhand_values.is_empty()) {
        clear_computed_longhand_table();
        return;
    }
    auto* table = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_copy_from_values(table, old_longhand_values.data(), old_longhand_values.size());
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        if (!is_inherited_property(property_id))
            continue;
        if (auto const* data = parent_longhand_values[i - to_underlying(first_longhand_property_id)])
            ComputedValuesFFI::rust_computed_longhand_table_set(table, i, data, -1);
    }
    ComputedValuesFFI::rust_computed_longhand_table_freeze(table);
    clear_computed_longhand_table();
    // The freshly created table already carries the one reference this style owns.
    m_computed_longhand_table = table;
    m_longhand_values = { ComputedValuesFFI::rust_computed_longhand_table_values(table), number_of_longhand_properties };
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
        m_style_value_cache.remove(property_id);
        return nullptr;
    }
    if (auto it = m_style_value_cache.find(property_id); it != m_style_value_cache.end() && it->value->rust_style_value_data() == handle.data())
        return it->value;
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(handle.data()));
    m_style_value_cache.set(property_id, value);
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
        return non_empty(&m_inherited.text->letter_spacing_style_value);
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
        return non_empty(&m_inherited.text->word_spacing_style_value);
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
    if (m_inherited.text->color_style_value)
        return style_value_from_handle(PropertyID::Color, m_inherited.text->color_style_value);
    return computed_style_value(PropertyID::Color);
}

RefPtr<StyleValue const> ComputedValues::raw_cascaded_font_size() const
{
    if (m_raw_cascaded_font_size)
        return m_raw_cascaded_font_size;
    if (!m_borrowed_raw_cascaded_font_size)
        return {};
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(m_borrowed_raw_cascaded_font_size));
}

RefPtr<StyleValue const> ComputedValues::background_color_style_value() const
{
    return style_value_from_handle(PropertyID::BackgroundColor, m_noninherited.background->background_color_style_value);
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
    if (auto it = m_style_value_cache.find(property_id); it != m_style_value_cache.end() && it->value->rust_style_value_data() == stored)
        return it->value;
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(stored)));
    m_style_value_cache.set(property_id, value);
    return value;
}

RefPtr<StyleValue const> ComputedValues::computed_style_value_for_inheritance(PropertyID property_id, WithAnimationsApplied with_animations_applied) const
{
    if (with_animations_applied == WithAnimationsApplied::No && has_animated_values())
        return base_values().computed_style_value_for_inheritance(property_id);

    if (auto value = m_inheritance_dependent_specified_values.get(property_id); value.has_value() && value.value()->depends_on_current_color())
        return *value;

    for (auto const& entry : m_borrowed_inheritance_dependent_values) {
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

static ContentDataAndQuoteNestingLevel resolve_content(StyleValue const& value, QuotesData const& quotes_data, DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level)
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

static NonnullRefPtr<StyleValue const> computed_content_item_style_value(ComputedContentItem const& item)
{
    return item.visit(
        [](Utf16String const& string) -> NonnullRefPtr<StyleValue const> { return StringStyleValue::create(string); },
        [](Keyword keyword) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(keyword); },
        [](ComputedContentCounter const& counter) -> NonnullRefPtr<StyleValue const> {
            auto counter_style = CounterStyleStyleValue::create(counter.style.visit(
                [](Utf16FlyString const& name) -> Variant<Utf16FlyString, CounterStyleStyleValue::SymbolsFunction> { return name; },
                [](ComputedContentCounter::SymbolsFunction const& symbols) -> Variant<Utf16FlyString, CounterStyleStyleValue::SymbolsFunction> {
                    return CounterStyleStyleValue::SymbolsFunction { .type = symbols.type, .symbols = symbols.symbols };
                }));
            if (counter.function == ComputedContentCounter::Function::Counters)
                return CounterStyleValue::create_counters(counter.name, counter.join_string, move(counter_style));
            return CounterStyleValue::create_counter(counter.name, move(counter_style));
        },
        [](NonnullRefPtr<AbstractImageStyleValue const> const& image) -> NonnullRefPtr<StyleValue const> { return image; });
}

ContentDataAndQuoteNestingLevel ComputedValues::resolved_content(DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level) const
{
    // The content value resolve_content() consumes is rebuilt from the content group's data
    // rather than minted from the longhand table: the group holds the live image style values
    // whose loads this style already started, and layout must receive those exact objects.
    auto content_style_value = [&]() -> NonnullRefPtr<StyleValue const> {
        switch (computed_content().type) {
        case ComputedContentData::Type::Normal:
            return KeywordStyleValue::create(Keyword::Normal);
        case ComputedContentData::Type::None:
            return KeywordStyleValue::create(Keyword::None);
        case ComputedContentData::Type::List: {
            StyleValueVector items;
            for (auto const& item : computed_content().items)
                items.append(computed_content_item_style_value(item));
            StyleValueVector alt_text;
            for (auto const& item : computed_content().alt_text)
                alt_text.append(computed_content_item_style_value(item));
            ValueComparingRefPtr<StyleValueList const> alt_text_style_value;
            if (!alt_text.is_empty())
                alt_text_style_value = StyleValueList::create(move(alt_text), StyleValueList::Separator::Space);
            return ContentStyleValue::create(
                StyleValueList::create(move(items), StyleValueList::Separator::Space),
                move(alt_text_style_value));
        }
        }
        VERIFY_NOT_REACHED();
    }();
    return resolve_content(content_style_value, quotes(), element_reference, initial_quote_nesting_level);
}

}
