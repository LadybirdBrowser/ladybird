/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/FixedBitmap.h>
#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Utf16FlyString.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/ScalingMode.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/ColumnCount.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/InstalledStyle.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/ResolvedTransform.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/CSS/StyleStructRef.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/Time.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/StyleEngineRustFFI.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>

namespace Web::DOM {

class Document;

}

namespace Web::CSS {

class AnimatedProperties;

class ComputedFilterView {
public:
    explicit ComputedFilterView(ComputedValuesFFI::ComputedFilter const& filter)
        : m_filter(filter)
    {
    }

    bool has_filters() const { return m_filter.filter_list.pointer; }
    bool is_none() const { return !has_filters(); }

    // The computed filter list, or a null handle for none.
    RustStyleValueHandle filter_list() const
    {
        if (!has_filters())
            return {};
        return RustStyleValueHandle::retained(static_cast<StyleValueFFI::StyleValueData const*>(m_filter.filter_list.pointer));
    }

    static Utf16String url_fragment(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
    {
        auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer);
        VERIFY(value && value->tag == StyleValueFFI::StyleValueData::Tag::Url);
        auto url = url_from_rust_data(value->url._0->url, value->url._0->url_type, value->url._0->modifiers);
        auto const& url_string = url.url();
        if (url_string.is_empty() || !url_string.starts_with('#'))
            return {};
        return Utf16String::from_utf16(url_string.substring_view(1));
    }

private:
    ComputedValuesFFI::ComputedFilter const& m_filter;
};

class ComputedStyleWorkingSet;
class ComputedStyleRecordView;
class StyleComputer;
class StyleScope;

using ClipRule = FillRule;

struct AspectRatio {
    bool use_natural_aspect_ratio_if_available;
    Optional<Ratio> preferred_ratio;
    bool computed_use_natural_aspect_ratio_if_available;
    Optional<Ratio> computed_ratio;

    bool operator==(AspectRatio const& other) const
    {
        // NB: Ratio's own equality is proportional (1 / 2 equals 0.5 / 1), but these ratios are
        //     observable through serialization, so compare the components exactly.
        auto ratios_identical = [](Optional<Ratio> const& a, Optional<Ratio> const& b) {
            if (a.has_value() != b.has_value())
                return false;
            if (!a.has_value())
                return true;
            return a->numerator() == b->numerator() && a->denominator() == b->denominator();
        };
        return use_natural_aspect_ratio_if_available == other.use_natural_aspect_ratio_if_available
            && computed_use_natural_aspect_ratio_if_available == other.computed_use_natural_aspect_ratio_if_available
            && ratios_identical(preferred_ratio, other.preferred_ratio)
            && ratios_identical(computed_ratio, other.computed_ratio);
    }
};

struct GridAutoFlow {
    bool row { true };
    bool dense { false };

    bool operator==(GridAutoFlow const&) const = default;
};

struct NormalGap {
    bool operator==(NormalGap const&) const = default;
};

struct Position {
    PositionEdge edge_x { PositionEdge::Left };
    LengthPercentage offset_x { Percentage(50) };
    PositionEdge edge_y { PositionEdge::Top };
    LengthPercentage offset_y { Percentage(50) };

    bool operator==(Position const&) const = default;

    CSSPixelPoint resolved(CSSPixelRect const& rect) const
    {
        CSSPixels x = offset_x.to_px(rect.width());
        CSSPixels y = offset_y.to_px(rect.height());
        if (edge_x == PositionEdge::Right)
            x = rect.width() - x;
        if (edge_y == PositionEdge::Bottom)
            y = rect.height() - y;
        return CSSPixelPoint { rect.x() + x, rect.y() + y };
    }
};

// https://drafts.csswg.org/css-contain-2/#containment-types
struct Containment {
    bool size_containment : 1 { false };
    bool inline_size_containment : 1 { false };
    bool layout_containment : 1 { false };
    bool style_containment : 1 { false };
    bool paint_containment : 1 { false };

    bool is_empty() const { return !(size_containment || inline_size_containment || layout_containment || style_containment || paint_containment); }

    bool operator==(Containment const&) const = default;
};

struct ContainerType {
    bool is_size_container : 1 { false };
    bool is_inline_size_container : 1 { false };
    bool is_scroll_state_container : 1 { false };

    bool operator==(ContainerType const&) const = default;

    bool is_empty() const { return !(is_size_container || is_inline_size_container || is_scroll_state_container); }
};

struct ScrollbarColorData {
    Color thumb_color { Color::Transparent };
    Color track_color { Color::Transparent };
    bool is_auto { true };

    bool operator==(ScrollbarColorData const&) const = default;
};

struct TextIndentData {
    LengthPercentage length_percentage;
    bool each_line { false };
    bool hanging { false };

    bool operator==(TextIndentData const&) const = default;
};

struct TextUnderlinePosition {
    TextUnderlinePositionHorizontal horizontal { TextUnderlinePositionHorizontal::Auto };
    TextUnderlinePositionVertical vertical { TextUnderlinePositionVertical::Auto };

    bool operator==(TextUnderlinePosition const&) const = default;
};

class InitialValues {
public:
    static CSSPixels font_size() { return 16; }
    static double font_weight() { return 400; }
    static CSSPixels line_height() { return 0; }
    static Color stop_color() { return Color::Black; }

    // https://www.w3.org/TR/SVG/geometry.html
    static int math_depth() { return 0; }
};

// A group's color, with `currentcolor`, which the group leaves to its reader, resolved to the
// element's color.
inline Color resolve_style_color(ComputedValuesFFI::StyleColor color, Color current_color)
{
    return color.is_current_color ? current_color : Color::from_bgra(color.rgba);
}

struct BorderData {
public:
    Color color { Color::Transparent };
    LineStyle line_style { LineStyle::None };
    CSSPixels width { 0 };

    bool operator==(BorderData const&) const = default;
};

struct TransformOrigin {
    LengthPercentage x { Percentage(50) };
    LengthPercentage y { Percentage(50) };
    // FIXME: We can store this as a CSSPixels since we know it's always an absolute length
    LengthPercentage z { Percentage(0) };

    bool operator==(TransformOrigin const&) const = default;
};

// One computed shadow layer, whose color may be `currentcolor`, which `resolved_color()` resolves.
struct ShadowData {
    CSSPixels offset_x { 0 };
    CSSPixels offset_y { 0 };
    CSSPixels blur_radius { 0 };
    CSSPixels spread_distance { 0 };
    ComputedValuesFFI::StyleColor color {};
    ColorSyntax color_syntax { ColorSyntax::Legacy };
    ShadowPlacement placement { ShadowPlacement::Outer };

    Color resolved_color(Color current_color) const { return resolve_style_color(color, current_color); }
};

// FIXME: Find a better place for this helper.
inline Gfx::ScalingMode to_gfx_scaling_mode(ImageRendering css_value, Gfx::IntSize source, Gfx::IntSize target)
{
    switch (css_value) {
    case ImageRendering::Auto:
    case ImageRendering::HighQuality:
    case ImageRendering::Optimizequality:
    case ImageRendering::Smooth:
        if (target.width() < source.width() && target.height() < source.height())
            return Gfx::ScalingMode::BilinearMipmap;
        return Gfx::ScalingMode::Bilinear;
    case ImageRendering::CrispEdges:
    case ImageRendering::Optimizespeed:
    case ImageRendering::Pixelated:
        return Gfx::ScalingMode::NearestNeighbor;
    }
    VERIFY_NOT_REACHED();
}

// FIXME: Find a better place for this helper.
inline Gfx::InterpolationColorSpace to_interpolation_color_space(ColorInterpolation css_value)
{
    switch (css_value) {
    case ColorInterpolation::Linearrgb:
        return Gfx::InterpolationColorSpace::LinearRGB;
    case ColorInterpolation::Auto:
    case ColorInterpolation::Srgb:
        return Gfx::InterpolationColorSpace::SRGB;
    }
    VERIFY_NOT_REACHED();
}

// Every ComputedValues style value group, in vtable registration order:
// G(enumerator, member path, sharing-info name).
#define LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(G)                          \
    G(InheritedListTableValues, m_inherited.list_table, "inheritedListTable")    \
    G(InheritedUIValues, m_inherited.ui, "inheritedUI")                          \
    G(InheritedSVGValues, m_inherited.svg, "inheritedSVG")                       \
    G(InheritedTextValues, m_inherited.text, "inheritedText")                    \
    G(FontValues, m_inherited.font, "font")                                      \
    G(AnimationValues, m_noninherited.animation, "animation")                    \
    G(SVGResetValues, m_noninherited.svg_reset, "svgReset")                      \
    G(GridValues, m_noninherited.grid, "grid")                                   \
    G(AnchorValues, m_noninherited.anchor, "anchor")                             \
    G(ContentValues, m_noninherited.content_data, "content")                     \
    G(AlignmentValues, m_noninherited.alignment, "alignment")                    \
    G(MiscLayoutValues, m_noninherited.misc_layout, "miscLayout")                \
    G(MiscInteractionValues, m_noninherited.misc_interaction, "miscInteraction") \
    G(EffectsValues, m_noninherited.effects, "effects")                          \
    G(MaskValues, m_noninherited.mask_data, "mask")                              \
    G(TextResetValues, m_noninherited.text_reset, "textReset")                   \
    G(TransformValues, m_noninherited.transform, "transform")                    \
    G(BackgroundValues, m_noninherited.background, "background")                 \
    G(BorderValues, m_noninherited.border, "border")                             \
    G(SizingValues, m_noninherited.sizing, "sizing")                             \
    G(SurroundValues, m_noninherited.surround, "surround")                       \
    G(BoxValues, m_noninherited.box, "box")

class StyleEngine;

enum class StyleGroupIndex : size_t {
#define LIBWEB_STYLE_GROUP_ENUMERATOR(name, ...) name,
    LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_ENUMERATOR)
#undef LIBWEB_STYLE_GROUP_ENUMERATOR
        Count,
};

// Bits of the dependency flags published alongside a style record's group payloads.
enum class StyleRecordDependencyFlag : u8 {
    DependsOnViewportMetrics = 1 << 0,
    FontMetricsDependOnViewportMetrics = 1 << 1,
    InDisplayNoneSubtree = 1 << 2,
    // Bit 3 is the engine's publication-time inherited-group swap eligibility, never stored on a record.
    // The record holds an <image> in a property whose images a layout node loads and observes.
    HoldsImageValues = 1 << 4,
    // A highlight pseudo-element whose color or background-color the author origin decided, on itself or up
    // its highlight chain.
    HighlightColorsAuthored = 1 << 5,
    // A highlight pseudo-element whose color is currentColor: the layer below shows through.
    HighlightColorIsCurrentColor = 1 << 6,
};
AK_ENUM_BITWISE_OPERATORS(StyleRecordDependencyFlag);

inline Display display_from_ffi_display(ComputedValuesFFI::FfiDisplay const& display)
{
    switch (static_cast<Display::Type>(display.tag)) {
    case Display::Type::OutsideAndInside:
        return Display {
            static_cast<DisplayOutside>(display.outside),
            static_cast<DisplayInside>(display.inside),
            display.list_item ? Display::ListItem::Yes : Display::ListItem::No,
        };
    case Display::Type::Internal:
        return Display { static_cast<DisplayInternal>(display.internal) };
    case Display::Type::Box:
        return Display { static_cast<DisplayBox>(display.box_value) };
    }
    VERIFY_NOT_REACHED();
}

// The inherited longhands a style holds as bits beside its inherited groups, each read at its
// field's shift and width.
inline u8 inherited_bits_field(ComputedValuesFFI::InheritedBits bits, u32 shift, u32 width)
{
    return static_cast<u8>((bits >> shift) & ((1u << width) - 1));
}

inline WritingMode writing_mode_from_inherited_bits(ComputedValuesFFI::InheritedBits bits)
{
    return static_cast<WritingMode>(inherited_bits_field(bits, ComputedValuesFFI::INHERITED_BITS_WRITING_MODE_SHIFT, ComputedValuesFFI::INHERITED_BITS_WRITING_MODE_WIDTH));
}

inline Direction direction_from_inherited_bits(ComputedValuesFFI::InheritedBits bits)
{
    return static_cast<Direction>(inherited_bits_field(bits, ComputedValuesFFI::INHERITED_BITS_DIRECTION_SHIFT, ComputedValuesFFI::INHERITED_BITS_DIRECTION_WIDTH));
}

inline Visibility visibility_from_inherited_bits(ComputedValuesFFI::InheritedBits bits)
{
    return static_cast<Visibility>(inherited_bits_field(bits, ComputedValuesFFI::INHERITED_BITS_VISIBILITY_SHIFT, ComputedValuesFFI::INHERITED_BITS_VISIBILITY_WIDTH));
}

inline WhiteSpaceCollapse white_space_collapse_from_inherited_bits(ComputedValuesFFI::InheritedBits bits)
{
    return static_cast<WhiteSpaceCollapse>(inherited_bits_field(bits, ComputedValuesFFI::INHERITED_BITS_WHITE_SPACE_COLLAPSE_SHIFT, ComputedValuesFFI::INHERITED_BITS_WHITE_SPACE_COLLAPSE_WIDTH));
}

inline PointerEvents pointer_events_from_inherited_bits(ComputedValuesFFI::InheritedBits bits)
{
    return static_cast<PointerEvents>(inherited_bits_field(bits, ComputedValuesFFI::INHERITED_BITS_POINTER_EVENTS_SHIFT, ComputedValuesFFI::INHERITED_BITS_POINTER_EVENTS_WIDTH));
}

// https://drafts.csswg.org/css-viewport/#effective-zoom
inline float effective_zoom_of(ComputedValuesFFI::InheritedSetValues const& values)
{
    return bit_cast<float>(values.effective_zoom_bits);
}

// The properties that do not inherit a style holds as bits in its root, each read at its field's
// shift and width.
inline u8 reset_bits_field(ComputedValuesFFI::ResetBits bits, u32 shift, u32 width)
{
    return static_cast<u8>((bits >> shift) & ((1u << width) - 1));
}

inline Float float_from_reset_bits(ComputedValuesFFI::ResetBits bits) { return static_cast<Float>(reset_bits_field(bits, ComputedValuesFFI::RESET_BITS_FLOAT_SHIFT, ComputedValuesFFI::RESET_BITS_FLOAT_WIDTH)); }
inline Clear clear_from_reset_bits(ComputedValuesFFI::ResetBits bits) { return static_cast<Clear>(reset_bits_field(bits, ComputedValuesFFI::RESET_BITS_CLEAR_SHIFT, ComputedValuesFFI::RESET_BITS_CLEAR_WIDTH)); }
inline Positioning position_from_reset_bits(ComputedValuesFFI::ResetBits bits) { return static_cast<Positioning>(reset_bits_field(bits, ComputedValuesFFI::RESET_BITS_POSITION_SHIFT, ComputedValuesFFI::RESET_BITS_POSITION_WIDTH)); }
inline Overflow overflow_x_from_reset_bits(ComputedValuesFFI::ResetBits bits) { return static_cast<Overflow>(reset_bits_field(bits, ComputedValuesFFI::RESET_BITS_OVERFLOW_X_SHIFT, ComputedValuesFFI::RESET_BITS_OVERFLOW_X_WIDTH)); }
inline Overflow overflow_y_from_reset_bits(ComputedValuesFFI::ResetBits bits) { return static_cast<Overflow>(reset_bits_field(bits, ComputedValuesFFI::RESET_BITS_OVERFLOW_Y_SHIFT, ComputedValuesFFI::RESET_BITS_OVERFLOW_Y_WIDTH)); }
inline BoxSizing box_sizing_from_reset_bits(ComputedValuesFFI::ResetBits bits) { return static_cast<BoxSizing>(reset_bits_field(bits, ComputedValuesFFI::RESET_BITS_BOX_SIZING_SHIFT, ComputedValuesFFI::RESET_BITS_BOX_SIZING_WIDTH)); }

// The reset bits of the initial style.
constexpr ComputedValuesFFI::ResetBits initial_reset_bits()
{
    return static_cast<ComputedValuesFFI::ResetBits>(
        (static_cast<u32>(to_underlying(Positioning::Static)) << ComputedValuesFFI::RESET_BITS_POSITION_SHIFT)
        | (static_cast<u32>(to_underlying(Float::None)) << ComputedValuesFFI::RESET_BITS_FLOAT_SHIFT)
        | (static_cast<u32>(to_underlying(Clear::None)) << ComputedValuesFFI::RESET_BITS_CLEAR_SHIFT)
        | (static_cast<u32>(to_underlying(Overflow::Visible)) << ComputedValuesFFI::RESET_BITS_OVERFLOW_X_SHIFT)
        | (static_cast<u32>(to_underlying(Overflow::Visible)) << ComputedValuesFFI::RESET_BITS_OVERFLOW_Y_SHIFT)
        | (static_cast<u32>(to_underlying(BoxSizing::ContentBox)) << ComputedValuesFFI::RESET_BITS_BOX_SIZING_SHIFT)
        | (static_cast<u32>(to_underlying(UnicodeBidi::Normal)) << ComputedValuesFFI::RESET_BITS_UNICODE_BIDI_SHIFT)
        | (static_cast<u32>(to_underlying(TableLayout::Auto)) << ComputedValuesFFI::RESET_BITS_TABLE_LAYOUT_SHIFT));
}

// What the inherited set of the initial style holds beside its groups.
constexpr ComputedValuesFFI::InheritedSetValues initial_inherited_set_values()
{
    return {
        .bits = static_cast<ComputedValuesFFI::InheritedBits>(
            (static_cast<u32>(to_underlying(WritingMode::HorizontalTb)) << ComputedValuesFFI::INHERITED_BITS_WRITING_MODE_SHIFT)
            | (static_cast<u32>(to_underlying(Direction::Ltr)) << ComputedValuesFFI::INHERITED_BITS_DIRECTION_SHIFT)
            | (static_cast<u32>(to_underlying(Visibility::Visible)) << ComputedValuesFFI::INHERITED_BITS_VISIBILITY_SHIFT)
            | (static_cast<u32>(to_underlying(TextOrientation::Mixed)) << ComputedValuesFFI::INHERITED_BITS_TEXT_ORIENTATION_SHIFT)
            | (static_cast<u32>(to_underlying(PointerEvents::Auto)) << ComputedValuesFFI::INHERITED_BITS_POINTER_EVENTS_SHIFT)
            | (static_cast<u32>(to_underlying(ListStylePosition::Outside)) << ComputedValuesFFI::INHERITED_BITS_LIST_STYLE_POSITION_SHIFT)
            | (static_cast<u32>(to_underlying(BorderCollapse::Separate)) << ComputedValuesFFI::INHERITED_BITS_BORDER_COLLAPSE_SHIFT)
            | (static_cast<u32>(to_underlying(CaptionSide::Top)) << ComputedValuesFFI::INHERITED_BITS_CAPTION_SIDE_SHIFT)
            | (static_cast<u32>(to_underlying(EmptyCells::Show)) << ComputedValuesFFI::INHERITED_BITS_EMPTY_CELLS_SHIFT)
            | (static_cast<u32>(to_underlying(TextAlign::Start)) << ComputedValuesFFI::INHERITED_BITS_TEXT_ALIGN_SHIFT)
            | (static_cast<u32>(to_underlying(WhiteSpaceCollapse::Collapse)) << ComputedValuesFFI::INHERITED_BITS_WHITE_SPACE_COLLAPSE_SHIFT)
            | (static_cast<u32>(to_underlying(TextWrapMode::Wrap)) << ComputedValuesFFI::INHERITED_BITS_TEXT_WRAP_MODE_SHIFT)
            | (static_cast<u32>(to_underlying(WordBreak::Normal)) << ComputedValuesFFI::INHERITED_BITS_WORD_BREAK_SHIFT)),
        .effective_zoom_bits = bit_cast<u32>(1.0f),
    };
}

// One counted reference to a style record, or none.
class StyleRecordRef {
public:
    StyleRecordRef() = default;
    explicit StyleRecordRef(void const* record);
    // Takes over the reference to `record` the caller holds.
    static StyleRecordRef adopt(void const* record)
    {
        StyleRecordRef adopted;
        adopted.m_record = record;
        return adopted;
    }
    StyleRecordRef(StyleRecordRef const& other)
        : StyleRecordRef(other.m_record)
    {
    }
    StyleRecordRef(StyleRecordRef&& other)
        : m_record(exchange(other.m_record, nullptr))
    {
    }
    ~StyleRecordRef();

    StyleRecordRef& operator=(StyleRecordRef const& other)
    {
        StyleRecordRef copy { other };
        swap(m_record, copy.m_record);
        return *this;
    }

    StyleRecordRef& operator=(StyleRecordRef&& other)
    {
        StyleRecordRef moved { move(other) };
        swap(m_record, moved.m_record);
        return *this;
    }

    void const* get() const { return m_record; }

private:
    void const* m_record { nullptr };
};

class WEB_API ComputedValues final : public RefCounted<ComputedValues> {
    AK_MAKE_NONCOPYABLE(ComputedValues);
    AK_MAKE_NONMOVABLE(ComputedValues);

public:
    class Builder;
    class Mutator;

    enum class WithAnimationsApplied {
        No,
        Yes,
    };

    static constexpr u32 all_style_groups = (1u << to_underlying(StyleGroupIndex::Count)) - 1;

    RefPtr<StyleValue const> computed_style_value(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;

    // The stored Rust style value that IS the property's computed value, for the properties that
    // keep one; null for every other property, and for a stored value that is currently absent.
    // Callers can serialize straight from the returned handle without minting a wrapper.
    RustStyleValueHandle const* stored_style_value_handle(PropertyID) const;

    // The style record this style is a view of, or is copied from, where it has one.
    StyleRecordRef const& record() const { return m_record; }

    ComputedValues const& base_values() const { return m_borrowed_base_values ? *m_borrowed_base_values : m_base_values ? *m_base_values
                                                                                                                        : *this; }
    bool has_animated_values() const { return m_borrowed_base_values || m_base_values; }
    AnimatedProperties const* animated_properties() const { return m_animated_properties.ptr(); }

    struct Statistics {
        u64 live_instance_count { 0 };
        u64 total_instances_created { 0 };
    };
    static Statistics const& statistics() { return s_statistics; }

    // Returns the Rust-owned payload for direct read-only layout access. The
    // pointer is borrowed from this immutable ComputedValues instance.
    void const* style_group_payload(StyleGroupIndex) const;

    static constexpr size_t inherited_style_group_count = 7;

    // Calls back with (name, shared_with_parent, is_default) for every style value group,
    // for introspecting how well group sharing is working (see internals.styleGroupSharingInfo()).
    template<typename Callback>
    void for_each_style_group_sharing_state(ComputedValues const* parent, Callback callback) const
    {
#define LIBWEB_VISIT_STYLE_GROUP(name, path, sharing_name) \
    callback(sharing_name##sv, parent ? path.ptr_equals(parent->path) : false, path.is_default());
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_VISIT_STYLE_GROUP)
#undef LIBWEB_VISIT_STYLE_GROUP
    }

    bool is_property_important(PropertyID) const;

    bool depends_on_viewport_metrics() const { return m_depends_on_viewport_metrics; }
    bool font_metrics_depend_on_viewport_metrics() const { return m_font_metrics_depend_on_viewport_metrics; }
    bool in_display_none_subtree() const { return m_in_display_none_subtree; }

    ~ComputedValues();

private:
public:
    ReadonlySpan<Utf16FlyString> anchor_names() const { return m_noninherited.anchor->anchor_names_span(); }
    // The animation-name entries other than none.

    Float float_() const { return float_from_reset_bits(m_noninherited.reset_bits); }
    Clear clear() const { return clear_from_reset_bits(m_noninherited.reset_bits); }
    // https://drafts.csswg.org/css-ui-4/#caret-color
    // auto: "User agents should use currentColor." It computes to auto, so the element's own color is read here.
    Color caret_color() const { return m_inherited.ui->caret_color_value(color()); }
    ColorInterpolation color_interpolation() const { return m_inherited.svg->color_interpolation_value(); }
    FillRule fill_rule() const { return m_inherited.svg->fill_rule_value(); }
    PreferredColorScheme color_scheme() const { return m_inherited.ui->color_scheme_value(); }
    ContentVisibility content_visibility() const { return static_cast<ContentVisibility>(m_noninherited.misc_layout->content_visibility); }
    ReadonlySpan<ComputedValuesFFI::ComputedCursor> cursor() const { return m_inherited.ui->cursor_span(); }
    NonnullRefPtr<StyleValue const> computed_content() const { return m_noninherited.content_data->computed_content_value(); }
    bool content_is_normal() const { return m_noninherited.content_data->content_is_normal(); }
    // The ComputedValuesFFI::GENERATED_CONTENT_* facts of the content and counter properties.
    u8 generated_content_facts() const { return ComputedValuesFFI::rust_content_group_generated_content_facts(&*m_noninherited.content_data); }
    bool content_uses_list_item_counter() const { return generated_content_facts() & ComputedValuesFFI::GENERATED_CONTENT_SHOWS_LIST_ITEM_COUNTER; }
    Display display() const { return display_from_ffi_display(m_noninherited.box->display); }
    Display display_before_box_type_transformation() const { return display_from_ffi_display(m_noninherited.box->display_before_box_type_transformation); }
    Color text_decoration_color() const { return resolve_style_color(m_noninherited.text_reset->text_decoration_color, color()); }
    ReadonlySpan<ShadowData> text_shadow() const { return m_inherited.text->text_shadow_span(); }
    Positioning position() const { return position_from_reset_bits(m_noninherited.reset_bits); }
    Appearance appearance() const { return static_cast<Appearance>(m_noninherited.misc_layout->appearance); }
    float opacity() const { return m_noninherited.effects->opacity; }
    Visibility visibility() const { return visibility_from_inherited_bits(m_inherited.values.bits); }
    ComputedFilterView backdrop_filter() const { return m_noninherited.effects->backdrop_filter_value(); }
    ComputedFilterView filter() const { return m_noninherited.effects->filter_value(); }
    ReadonlySpan<ShadowData> box_shadow() const { return m_noninherited.effects->box_shadow_span(); }
    ComputedValuesFFI::ComputedSize const& width() const { return m_noninherited.sizing->width; }
    ComputedValuesFFI::ComputedSize const& height() const { return m_noninherited.sizing->height; }
    Direction direction() const { return direction_from_inherited_bits(m_inherited.values.bits); }
    WritingMode writing_mode() const { return writing_mode_from_inherited_bits(m_inherited.values.bits); }
    // https://drafts.csswg.org/css-viewport/#effective-zoom
    double effective_zoom() const { return effective_zoom_of(m_inherited.values); }
    ComputedValuesFFI::InheritedSetValues const& inherited_set_values() const { return m_inherited.values; }

    ContainerType container_type() const
    {
        auto const& box = *m_noninherited.box;
        return ContainerType {
            .is_size_container = box.is_size_container,
            .is_inline_size_container = box.is_inline_size_container,
            .is_scroll_state_container = box.is_scroll_state_container,
        };
    }
    MixBlendMode mix_blend_mode() const { return m_noninherited.effects->mix_blend_mode_value(); }
    Optional<Utf16FlyString> view_transition_name() const { return m_noninherited.misc_interaction->view_transition_name_value(); }

    LengthBox inset() const { return length_box(m_noninherited.surround->inset); }
    RefPtr<StyleValue const> anchor_inset(PropertyID property_id) const
    {
        auto const* handle = anchor_inset_handle(property_id);
        if (!handle)
            return {};
        static_assert(sizeof(RustStyleValueHandle) == sizeof(*handle));
        return style_value_from_handle(property_id, reinterpret_cast<RustStyleValueHandle const&>(*handle));
    }
    LengthBox margin() const { return length_box(m_noninherited.surround->margin); }
    LengthBox scroll_margin() const { return length_box(m_noninherited.misc_interaction->scroll_margin); }

    BorderData border_left() const { return m_noninherited.border->border_left_value(color()); }
    BorderData border_top() const { return m_noninherited.border->border_top_value(color()); }
    BorderData border_right() const { return m_noninherited.border->border_right_value(color()); }
    BorderData border_bottom() const { return m_noninherited.border->border_bottom_value(color()); }

    Overflow overflow_x() const { return overflow_x_from_reset_bits(m_noninherited.reset_bits); }
    Overflow overflow_y() const { return overflow_y_from_reset_bits(m_noninherited.reset_bits); }
    ComputedValuesFFI::ResetBits reset_bits() const { return m_noninherited.reset_bits; }

    Color color() const { return m_inherited.text->color_value(); }
    Color background_color() const { return m_noninherited.background->background_color_value(color()); }
    RefPtr<StyleValue const> background_color_style_value() const;

    Color webkit_text_fill_color() const { return m_inherited.text->webkit_text_fill_color_value(); }

    void const* list_style_type_data() const { return m_inherited.list_table->list_style_type.pointer; }
    RefPtr<AbstractImageStyleValue const> list_style_image() const { return m_inherited.list_table->list_style_image_value(); }
    // Whether this style reads its tree scope's counter-style registry, which a pseudo-element's does for any named
    // counter style.
    bool reads_counter_style_environment(bool is_pseudo) const { return ComputedValuesFFI::rust_style_reads_counter_style_environment(static_cast<StyleValueFFI::StyleValueData const*>(m_noninherited.content_data->content.pointer), static_cast<StyleValueFFI::StyleValueData const*>(list_style_type_data()), is_pseudo); }

    Color flood_color() const { return resolve_style_color(m_noninherited.svg_reset->flood_color, color()); }
    float flood_opacity() const { return m_noninherited.svg_reset->flood_opacity; }

    NonnullRefPtr<StyleValue const> d() const
    {
        auto const* handle = &m_noninherited.svg_reset->d;
        static_assert(sizeof(RustStyleValueHandle) == sizeof(*handle));
        return style_value_from_handle(PropertyID::D, reinterpret_cast<RustStyleValueHandle const&>(*handle)).release_nonnull();
    }
    LengthPercentage const& x() const { return LengthPercentage::view(m_noninherited.svg_reset->x); }
    LengthPercentage const& y() const { return LengthPercentage::view(m_noninherited.svg_reset->y); }

    bool has_transformations() const { return m_noninherited.transform->has_transformations(); }
    TransformOrigin transform_origin() const { return m_noninherited.transform->transform_origin_value(); }
    RefPtr<TransformationStyleValue const> rotate() const { return m_noninherited.transform->rotate_value(); }
    RefPtr<TransformationStyleValue const> translate() const { return m_noninherited.transform->translate_value(); }
    RefPtr<TransformationStyleValue const> scale() const { return m_noninherited.transform->scale_value(); }
    bool has_rotate() const { return rotate() != nullptr; }
    bool has_translate() const { return translate() != nullptr; }
    bool has_scale() const { return scale() != nullptr; }
    Optional<CSSPixels> perspective() const { return m_noninherited.transform->perspective_value(); }

    Gfx::FontCascadeList const& font_list() const { return m_inherited.font->font_list_value(); }
    CSSPixels font_size() const { return m_inherited.font->font_size; }
    double font_weight() const { return m_inherited.font->font_weight; }
    CSSPixels line_height() const { return m_inherited.font->line_height_used_value(); }

    Color outline_color() const { return resolve_style_color(m_noninherited.misc_layout->outline_color, color()); }
    Color column_rule_color() const { return resolve_style_color(m_noninherited.misc_layout->column_rule_color, color()); }
    OutlineStyle outline_style() const { return static_cast<OutlineStyle>(m_noninherited.misc_layout->outline_style); }

    int math_depth() const { return m_inherited.font->math_depth; }

private:
    friend class ComputedStyleRecordView;
    enum class BorrowedStyleRecord { Yes };
    ComputedValues();

public:
    explicit ComputedValues(BorrowedStyleRecord);

private:
    void borrow_style_record_groups(void const* groups);

    RefPtr<StyleValue const> style_value_from_handle(PropertyID, RustStyleValueHandle const&) const;

    ComputedValuesFFI::ComputedStyleValueHandle const* anchor_inset_handle(PropertyID property_id) const
    {
        switch (property_id) {
        case PropertyID::Top:
            return &m_noninherited.surround->top_anchor_inset;
        case PropertyID::Right:
            return &m_noninherited.surround->right_anchor_inset;
        case PropertyID::Bottom:
            return &m_noninherited.surround->bottom_anchor_inset;
        case PropertyID::Left:
            return &m_noninherited.surround->left_anchor_inset;
        default:
            return nullptr;
        }
    }

    static LengthPercentageOrAuto length_percentage_or_auto(ComputedValuesFFI::ComputedLengthPercentageOrAuto const& value)
    {
        if (value.is_auto)
            return LengthPercentageOrAuto::make_auto();
        return LengthPercentage::view(value.value);
    }

    static LengthBox length_box(ComputedValuesFFI::ComputedLengthBox const& box)
    {
        return {
            length_percentage_or_auto(box.top),
            length_percentage_or_auto(box.right),
            length_percentage_or_auto(box.bottom),
            length_percentage_or_auto(box.left),
        };
    }

    static Statistics s_statistics;

    static size_t property_bitmap_index(PropertyID property_id)
    {
        VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
        return to_underlying(property_id) - to_underlying(first_longhand_property_id);
    }

public:
    struct InheritedListTableValues : ComputedValuesFFI::InheritedListTableValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedListTableValues);

        bool operator==(InheritedListTableValues const& other) const
        {
            return list_style_type.pointer == other.list_style_type.pointer
                && list_style_image.pointer == other.list_style_image.pointer
                && quotes.pointer == other.quotes.pointer
                && border_spacing_horizontal == other.border_spacing_horizontal
                && border_spacing_vertical == other.border_spacing_vertical;
        }

        RefPtr<AbstractImageStyleValue const> list_style_image_value() const;
    };

    struct InheritedUIValues : ComputedValuesFFI::InheritedUIValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedUIValues);

        // An auto caret-color holds `currentcolor`, so it resolves to the element's color too.
        Color caret_color_value(Color current_color) const { return resolve_style_color(caret_color.color, current_color); }
        ReadonlySpan<ComputedValuesFFI::ComputedCursor> cursor_span() const { return { cursor.pointer, cursor.length }; }
        static RefPtr<CursorStyleValue const> cursor_style_value(ComputedValuesFFI::ComputedCursor const& value)
        {
            if (!value.is_cursor_value)
                return nullptr;
            auto style_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(value.cursor.pointer)));
            return style_value->as_cursor();
        }
        PreferredColorScheme color_scheme_value() const { return static_cast<PreferredColorScheme>(color_scheme); }
        ReadonlySpan<Utf16FlyString> color_schemes_span() const
        {
            static_assert(sizeof(Utf16FlyString) == sizeof(size_t));
            static_assert(alignof(Utf16FlyString) == alignof(size_t));
            return { reinterpret_cast<Utf16FlyString const*>(color_schemes.pointer), color_schemes.length };
        }
    };

    struct InheritedSVGValues : ComputedValuesFFI::InheritedSVGValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedSVGValues);

        // The paint server a fill or stroke names by URL, if it does.
        static Optional<URL> paint_url_value(ComputedValuesFFI::ComputedSvgPaint const& paint)
        {
            if (paint.kind != 2)
                return {};
            auto const* url = static_cast<StyleValueFFI::StyleValueData const*>(paint.url.pointer);
            VERIFY(url && url->tag == StyleValueFFI::StyleValueData::Tag::Url);
            return url_from_rust_data(url->url._0->url, url->url._0->url_type, url->url._0->modifiers);
        }

        Optional<URL> fill_url_value() const { return paint_url_value(fill); }
        Optional<URL> stroke_url_value() const { return paint_url_value(stroke); }
        FillRule fill_rule_value() const { return static_cast<FillRule>(fill_rule); }
        ColorInterpolation color_interpolation_value() const { return static_cast<ColorInterpolation>(color_interpolation); }
        ColorInterpolation color_interpolation_filters_value() const { return static_cast<ColorInterpolation>(color_interpolation_filters); }
    };

    struct InheritedTextValues : ComputedValuesFFI::InheritedTextValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedTextValues);

        CSSPixels letter_spacing_value() const { return letter_spacing; }
        Color color_value() const { return Color::from_bgra(color); }
        Color webkit_text_fill_color_value() const { return Color::from_bgra(webkit_text_fill_color); }
        ReadonlySpan<ShadowData> text_shadow_span() const
        {
            static_assert(sizeof(ShadowData) == sizeof(ComputedValuesFFI::ComputedShadow));
            return { reinterpret_cast<ShadowData const*>(text_shadow.pointer), text_shadow.length };
        }
    };

    // Rust owns the canonical font values and the derived layout-facing facts.
    // The payload holds its own reference to the platform font cascade list.
    struct FontValues : ComputedValuesFFI::FontValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::FontValues);

        WEB_API Gfx::FontCascadeList const& font_list_value() const;
        FontStyleKeyword font_style_keyword() const;
        CSSPixels line_height_used_value() const { return CSSPixels::from_raw(ComputedValuesFFI::rust_font_values_line_height_used_raw(this)); }
    };

private:
    struct InheritedValues {
        InheritedValues() = default;

        StyleStructRef<InheritedListTableValues> list_table;
        StyleStructRef<InheritedUIValues> ui;
        StyleStructRef<InheritedSVGValues> svg;
        StyleStructRef<InheritedTextValues> text;
        StyleStructRef<FontValues> font;
        // The inherited longhands held as bits beside the inherited groups, and the effective zoom.
        ComputedValuesFFI::InheritedSetValues values { initial_inherited_set_values() };
    };

    InheritedValues m_inherited;

public:
    struct AnimationValues : ComputedValuesFFI::AnimationValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AnimationValues);
    };

    // The layout and lifecycle of this group are defined in Rust (computed_values.rs).
    struct SVGResetValues : ComputedValuesFFI::SVGResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SVGResetValues);
    };

    struct GridValues : ComputedValuesFFI::GridValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::GridValues);
    };

    struct AnchorValues : ComputedValuesFFI::AnchorValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AnchorValues);

        ReadonlySpan<Utf16FlyString> anchor_names_span() const { return fly_strings(anchor_names); }

    private:
        static ReadonlySpan<Utf16FlyString> fly_strings(ComputedValuesFFI::RetainedUtf16FlyStringList const& values)
        {
            static_assert(sizeof(Utf16FlyString) == sizeof(size_t));
            static_assert(alignof(Utf16FlyString) == alignof(size_t));
            return { reinterpret_cast<Utf16FlyString const*>(values.pointer), values.length };
        }
    };

    struct EffectsValues : ComputedValuesFFI::EffectsValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::EffectsValues);

        ComputedFilterView filter_value() const { return ComputedFilterView { filter }; }
        ComputedFilterView backdrop_filter_value() const { return ComputedFilterView { backdrop_filter }; }
        MixBlendMode mix_blend_mode_value() const { return static_cast<MixBlendMode>(mix_blend_mode); }
        ReadonlySpan<ShadowData> box_shadow_span() const
        {
            static_assert(sizeof(ShadowData) == sizeof(ComputedValuesFFI::ComputedShadow));
            static_assert(alignof(ShadowData) == alignof(ComputedValuesFFI::ComputedShadow));
            return { reinterpret_cast<ShadowData const*>(box_shadows.pointer), box_shadows.length };
        }
    };

    struct MaskValues : ComputedValuesFFI::MaskValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MaskValues);

        // The mask layer the first mask-image names by URL, if it does.
        Optional<URL> mask_url_value() const;
        Vector<RefPtr<AbstractImageStyleValue const>> mask_images_value() const;
        // https://drafts.fxtf.org/css-masking/#the-clip-path
        // TODO: Support basic shapes and geometry boxes.
        Optional<URL> clip_path_value() const;
    };

    struct TextResetValues : ComputedValuesFFI::TextResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::TextResetValues);

        ReadonlySpan<TextDecorationLine> decoration_lines() const
        {
            static_assert(sizeof(TextDecorationLine) == sizeof(u8));
            return { reinterpret_cast<TextDecorationLine const*>(text_decoration_lines.pointer), text_decoration_lines.length };
        }
    };

    struct ContentValues : ComputedValuesFFI::ContentValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::ContentValues);

        NonnullRefPtr<StyleValue const> computed_content_value() const;
        bool content_is_normal() const;
    };

    struct TransformValues : public ComputedValuesFFI::TransformValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::TransformValues);

        static RefPtr<StyleValue const> style_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
        {
            if (!handle.pointer)
                return nullptr;
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer)));
        }

        bool has_transformations() const { return transformations.pointer; }
        template<typename Callback>
        void for_each_transformation(Callback callback) const
        {
            auto value = style_value(transformations);
            if (!value)
                return;
            for (auto const& transformation : transformations_for_style_value(*value))
                callback(*transformation);
        }

        template<typename Callback>
        void for_each_resolved_transform(Callback callback) const
        {
            for (size_t index = 0; index < resolved_transforms.length; ++index) {
                auto const& entry = resolved_transforms.pointer[index];
                if (entry.is_translate) {
                    callback(ResolvedTransform { ResolvedTransform::Translate {
                        .x = { .px = entry.x_px, .percentage_value = style_value(entry.x_percentage) },
                        .y = { .px = entry.y_px, .percentage_value = style_value(entry.y_percentage) },
                        .z = entry.z_px,
                    } });
                    continue;
                }
                auto const& matrix = entry.matrix;
                callback(ResolvedTransform { FloatMatrix4x4(
                    matrix[0], matrix[1], matrix[2], matrix[3],
                    matrix[4], matrix[5], matrix[6], matrix[7],
                    matrix[8], matrix[9], matrix[10], matrix[11],
                    matrix[12], matrix[13], matrix[14], matrix[15]) });
            }
        }

        TransformOrigin transform_origin_value() const
        {
            return {
                LengthPercentage::view(transform_origin_x),
                LengthPercentage::view(transform_origin_y),
                LengthPercentage::view(transform_origin_z),
            };
        }
        TransformStyle transform_style_value() const { return static_cast<TransformStyle>(transform_style); }
        RefPtr<TransformationStyleValue const> rotate_value() const { return transformation_value(rotate); }
        RefPtr<TransformationStyleValue const> translate_value() const { return transformation_value(translate); }
        RefPtr<TransformationStyleValue const> scale_value() const { return transformation_value(scale); }
        Optional<CSSPixels> perspective_value() const
        {
            if (!has_perspective)
                return {};
            return CSSPixels::from_raw(perspective_px);
        }

    private:
        static RefPtr<TransformationStyleValue const> transformation_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
        {
            auto value = style_value(handle);
            if (!value)
                return nullptr;
            return value->as_transformation();
        }
    };

    struct BackgroundValues : ComputedValuesFFI::BackgroundValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BackgroundValues);

        Color background_color_value(Color current_color) const { return resolve_style_color(background_color, current_color); }
        Vector<RefPtr<AbstractImageStyleValue const>> background_images_value() const;
    };

    struct BorderValues : ComputedValuesFFI::BorderValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BorderValues);

        BorderData border_left_value(Color current_color) const { return side(border_left, current_color); }
        BorderData border_top_value(Color current_color) const { return side(border_top, current_color); }
        BorderData border_right_value(Color current_color) const { return side(border_right, current_color); }
        BorderData border_bottom_value(Color current_color) const { return side(border_bottom, current_color); }

        static BorderData side(ComputedValuesFFI::ComputedBorderSide const& side, Color current_color)
        {
            return { resolve_style_color(side.color, current_color), static_cast<LineStyle>(side.line_style), bit_cast<CSSPixels>(side.width) };
        }
        RefPtr<AbstractImageStyleValue const> border_image_source_value() const;
    };

    struct AlignmentValues : ComputedValuesFFI::AlignmentValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AlignmentValues);
    };

    struct MiscLayoutValues : ComputedValuesFFI::MiscLayoutValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MiscLayoutValues);
    };

    struct MiscInteractionValues : ComputedValuesFFI::MiscInteractionValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MiscInteractionValues);

        Optional<Utf16FlyString> view_transition_name_value() const;
        ScrollSnapStrictness scroll_snap_strictness_value() const { return static_cast<ScrollSnapStrictness>(scroll_snap_strictness); }
    };

    struct SizingValues : ComputedValuesFFI::SizingValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SizingValues);
    };

    struct SurroundValues : ComputedValuesFFI::SurroundValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SurroundValues);
    };

    struct BoxValues : ComputedValuesFFI::BoxValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BoxValues);
    };

private:
    struct NonInheritedValues {
        NonInheritedValues() = default;

        StyleStructRef<AnimationValues> animation;
        StyleStructRef<BoxValues> box;
        // The properties that do not inherit held as bits in the style's root.
        ComputedValuesFFI::ResetBits reset_bits { initial_reset_bits() };
        StyleStructRef<SurroundValues> surround;
        StyleStructRef<SizingValues> sizing;
        StyleStructRef<MiscLayoutValues> misc_layout;
        StyleStructRef<MiscInteractionValues> misc_interaction;
        StyleStructRef<AlignmentValues> alignment;
        StyleStructRef<BorderValues> border;
        StyleStructRef<BackgroundValues> background;
        StyleStructRef<TransformValues> transform;
        StyleStructRef<EffectsValues> effects;
        StyleStructRef<MaskValues> mask_data;
        StyleStructRef<TextResetValues> text_reset;
        StyleStructRef<ContentValues> content_data;
        StyleStructRef<AnchorValues> anchor;
        StyleStructRef<GridValues> grid;
        StyleStructRef<SVGResetValues> svg_reset;
    };

    NonInheritedValues m_noninherited;
    mutable OwnPtr<HashMap<PropertyID, NonnullRefPtr<StyleValue const>>> m_style_value_cache;
    // The style record the values are a view of, or a copy of a view of, which they keep alive
    // and read their longhands from.
    StyleRecordRef m_record;
    RefPtr<ComputedValues const> m_base_values;
    ComputedValues const* m_borrowed_base_values { nullptr };
    RefPtr<AnimatedProperties const> m_animated_properties;
    bool m_depends_on_viewport_metrics { false };
    bool m_font_metrics_depend_on_viewport_metrics { false };
    bool m_in_display_none_subtree { false };
    bool m_is_style_record_view { false };
};

// Reads one group of the style whose groups `record` points at: a style record's, or a style layout
// made of its own. This is the only place C++ knows how a style finds a group: the groups from
// ROOT_START on are in the style's root, and a pointer to each of its group sets follows them.
template<size_t index>
void const* style_record_group_payload(void const* record)
{
    VERIFY(record);
    auto const* slots = static_cast<void const* const*>(record);
    constexpr auto root_count = to_underlying(StyleGroupIndex::Count) - ComputedValuesFFI::STYLE_GROUP_ROOT_START;
    if constexpr (index >= ComputedValuesFFI::STYLE_GROUP_ROOT_START)
        return slots[index - ComputedValuesFFI::STYLE_GROUP_ROOT_START];
    else if constexpr (index >= ComputedValuesFFI::STYLE_GROUP_VISUAL_SET_START)
        return static_cast<void const* const*>(slots[root_count + 2])[index - ComputedValuesFFI::STYLE_GROUP_VISUAL_SET_START];
    else if constexpr (index >= ComputedValuesFFI::STYLE_GROUP_RARE_SET_START)
        return static_cast<void const* const*>(slots[root_count + 1])[index - ComputedValuesFFI::STYLE_GROUP_RARE_SET_START];
    else
        return static_cast<void const* const*>(slots[root_count])[index];
}

// Reads what the inherited set of the style whose groups `record` points at holds beside its
// groups, which follow them in the set.
inline ComputedValuesFFI::InheritedSetValues const& style_record_inherited_values(void const* record)
{
    VERIFY(record);
    auto const* slots = static_cast<void const* const*>(record);
    constexpr auto root_count = to_underlying(StyleGroupIndex::Count) - ComputedValuesFFI::STYLE_GROUP_ROOT_START;
    auto const* inherited_set = static_cast<void const* const*>(slots[root_count]);
    return *reinterpret_cast<ComputedValuesFFI::InheritedSetValues const*>(inherited_set + ComputedValuesFFI::STYLE_GROUP_RARE_SET_START);
}

// Reads the reset bits of the style whose groups `record` points at, which follow its set pointers.
inline ComputedValuesFFI::ResetBits style_record_reset_bits(void const* record)
{
    VERIFY(record);
    auto const* slots = static_cast<void const* const*>(record);
    constexpr auto root_count = to_underlying(StyleGroupIndex::Count) - ComputedValuesFFI::STYLE_GROUP_ROOT_START;
    return *reinterpret_cast<ComputedValuesFFI::ResetBits const*>(slots + root_count + 3);
}

// The groups of a style, laid out as a style record's are, over `payloads`, one per group in group
// order, with `values` beside the inherited ones and `reset` after the set pointers, for a reader of
// a style record's groups to read a style no record holds. It counts no reference, and `payloads`
// must outlive it.
struct StyleRecordGroupsOverPayloads {
    AK_MAKE_NONCOPYABLE(StyleRecordGroupsOverPayloads);
    AK_MAKE_NONMOVABLE(StyleRecordGroupsOverPayloads);

public:
    StyleRecordGroupsOverPayloads(Array<void const*, to_underlying(StyleGroupIndex::Count)> const& payloads, ComputedValuesFFI::InheritedSetValues values, ComputedValuesFFI::ResetBits reset)
        : root { payloads[ComputedValuesFFI::STYLE_GROUP_ROOT_START], payloads[ComputedValuesFFI::STYLE_GROUP_ROOT_START + 1], payloads[ComputedValuesFFI::STYLE_GROUP_ROOT_START + 2] }
        , inherited(inherited_set.groups)
        , rare(payloads.data() + ComputedValuesFFI::STYLE_GROUP_RARE_SET_START)
        , visual(payloads.data() + ComputedValuesFFI::STYLE_GROUP_VISUAL_SET_START)
        , reset(reset)
    {
        static_assert(to_underlying(StyleGroupIndex::Count) - ComputedValuesFFI::STYLE_GROUP_ROOT_START == 3);
        for (size_t index = 0; index < ComputedValuesFFI::STYLE_GROUP_RARE_SET_START; ++index)
            inherited_set.groups[index] = payloads[index];
        inherited_set.values = values;
    }

    void const* root[3];
    void const* const* inherited;
    void const* const* rare;
    void const* const* visual;
    ComputedValuesFFI::ResetBits reset;

private:
    // Laid out as a style's inherited set is: its groups, then what it holds beside them, with no
    // custom properties.
    struct {
        void const* groups[ComputedValuesFFI::STYLE_GROUP_RARE_SET_START];
        ComputedValuesFFI::InheritedSetValues values;
        void const* custom_properties { nullptr };
    } inherited_set;
};

template<typename StyleGroup>
StyleGroup const& style_record_group(void const* record)
{
    auto const* payload = style_record_group_payload<StyleGroup::style_group_index>(record);
    VERIFY(payload);
    return *static_cast<StyleGroup const*>(payload);
}

// A synchronous, allocation-free compatibility surface over the payloads of
// one authoritative StyleRecord. It owns no group or metadata payload.
class WEB_API ComputedStyleRecordView {
    AK_MAKE_NONCOPYABLE(ComputedStyleRecordView);
    AK_MAKE_NONMOVABLE(ComputedStyleRecordView);

public:
    ComputedStyleRecordView() = default;
    // Holds `style`, which must be present, for as long as the view reads it.
    explicit ComputedStyleRecordView(InstalledStyle style);
    ~ComputedStyleRecordView();

    explicit operator bool() const { return m_values.has_value(); }
    ComputedValues const* operator->() const
    {
        if (!m_values.has_value())
            return nullptr;
        return &*m_values;
    }
    ComputedValues const& operator*() const { return m_values.value(); }

private:
    Optional<ComputedValues> m_base_values;
    // NB: Only a present view builds its values, so a view of no record costs nothing to make.
    Optional<ComputedValues> m_values;
    InstalledStyle m_style;
};

class ComputedValues::Mutator final {
private:
    friend class Builder;

    explicit Mutator(ComputedValues& values)
        : m_values(values)
    {
    }

public:
    void set_animated_properties(AnimatedProperties const*);

    ComputedValues& m_values;
};

class ComputedValues::Builder {
public:
    Builder()
        : m_values(adopt_ref(*new ComputedValues))
        , m_mutator(*m_values)
    {
    }

    explicit Builder(ComputedValues const& values)
        : Builder()
    {
        m_values->m_inherited = values.m_inherited;
        m_values->m_noninherited = values.m_noninherited;
        m_values->m_record = values.m_record;
        if (values.m_borrowed_base_values)
            m_values->m_base_values = Builder { *values.m_borrowed_base_values }.build();
        else
            m_values->m_base_values = values.m_base_values;
        m_mutator.set_animated_properties(values.m_animated_properties.ptr());
        m_values->m_depends_on_viewport_metrics = values.m_depends_on_viewport_metrics;
        m_values->m_font_metrics_depend_on_viewport_metrics = values.m_font_metrics_depend_on_viewport_metrics;
        m_values->m_in_display_none_subtree = values.m_in_display_none_subtree;
    }

    NonnullRefPtr<ComputedValues const> build() && { return move(m_values); }

private:
    NonnullRefPtr<ComputedValues> m_values;
    Mutator m_mutator;
};

}
