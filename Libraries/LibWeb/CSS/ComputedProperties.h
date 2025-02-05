/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleProperty.h>

namespace Web::CSS {

class ComputedProperties final : public JS::Cell {
    GC_CELL(ComputedProperties, JS::Cell);
    GC_DECLARE_ALLOCATOR(ComputedProperties);

public:
    static constexpr size_t number_of_properties = to_underlying(CSS::last_property_id) + 1;

    virtual ~ComputedProperties() override;

    template<typename Callback>
    inline void for_each_property(Callback callback) const
    {
        for (size_t i = 0; i < m_property_values.size(); ++i) {
            if (m_property_values[i])
                callback((CSS::PropertyID)i, *m_property_values[i]);
        }
    }

    enum class Inherited {
        No,
        Yes
    };

    HashMap<CSS::PropertyID, NonnullRefPtr<CSSStyleValue const>> const& animated_property_values() const { return m_animated_property_values; }
    void reset_animated_properties();

    bool is_property_important(CSS::PropertyID property_id) const;
    bool is_property_inherited(CSS::PropertyID property_id) const;
    void set_property_important(CSS::PropertyID, Important);
    void set_property_inherited(CSS::PropertyID, Inherited);

    void set_property(CSS::PropertyID, NonnullRefPtr<CSSStyleValue const> value, Inherited = Inherited::No, Important = Important::No);
    void set_animated_property(CSS::PropertyID, NonnullRefPtr<CSSStyleValue const> value);
    enum class WithAnimationsApplied {
        No,
        Yes,
    };
    CSSStyleValue const& property(CSS::PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    CSSStyleValue const* maybe_null_property(CSS::PropertyID) const;
    void revert_property(CSS::PropertyID, ComputedProperties const& style_for_revert);

    GC::Ptr<CSS::CSSStyleDeclaration const> animation_name_source() const { return m_animation_name_source; }
    void set_animation_name_source(GC::Ptr<CSS::CSSStyleDeclaration const> declaration) { m_animation_name_source = declaration; }

    GC::Ptr<CSS::CSSStyleDeclaration const> transition_property_source() const { return m_transition_property_source; }
    void set_transition_property_source(GC::Ptr<CSS::CSSStyleDeclaration const> declaration) { m_transition_property_source = declaration; }

    CSS::Size size_value(CSS::PropertyID) const;
    [[nodiscard]] Variant<LengthPercentage, NormalGap> gap_value(CSS::PropertyID) const;
    LengthPercentage length_percentage_or_fallback(CSS::PropertyID, LengthPercentage const& fallback) const;
    Optional<LengthPercentage> length_percentage(CSS::PropertyID) const;
    LengthBox length_box(CSS::PropertyID left_id, CSS::PropertyID top_id, CSS::PropertyID right_id, CSS::PropertyID bottom_id, const CSS::Length& default_value) const;
    Color color_or_fallback(CSS::PropertyID, Layout::NodeWithStyle const&, Color fallback) const;
    CSS::PreferredColorScheme color_scheme(CSS::PreferredColorScheme, Optional<Vector<String> const&> document_supported_schemes) const;
    TextAnchor text_anchor() const;
    TextAlign text_align() const;
    TextJustify text_justify() const;
    TextOverflow text_overflow() const;
    CSS::Length border_spacing_horizontal(Layout::Node const&) const;
    CSS::Length border_spacing_vertical(Layout::Node const&) const;
    CaptionSide caption_side() const;
    CSS::Clip clip() const;
    CSS::Display display() const;
    Float float_() const;
    Clear clear() const;
    ColumnSpan column_span() const;
    struct ContentDataAndQuoteNestingLevel {
        CSS::ContentData content_data;
        u32 final_quote_nesting_level { 0 };
    };
    ContentDataAndQuoteNestingLevel content(DOM::Element&, u32 initial_quote_nesting_level) const;
    ContentVisibility content_visibility() const;
    Cursor cursor() const;
    Variant<LengthOrCalculated, NumberOrCalculated> tab_size() const;
    WhiteSpace white_space() const;
    WordBreak word_break() const;
    Optional<LengthOrCalculated> word_spacing() const;
    Optional<LengthOrCalculated> letter_spacing() const;
    LineStyle line_style(CSS::PropertyID) const;
    OutlineStyle outline_style() const;
    Vector<CSS::TextDecorationLine> text_decoration_line() const;
    TextDecorationStyle text_decoration_style() const;
    TextTransform text_transform() const;
    Vector<CSS::ShadowData> text_shadow(Layout::Node const&) const;
    ListStyleType list_style_type() const;
    ListStylePosition list_style_position() const;
    FlexDirection flex_direction() const;
    FlexWrap flex_wrap() const;
    FlexBasis flex_basis() const;
    float flex_grow() const;
    float flex_shrink() const;
    int order() const;
    Optional<Color> accent_color(Layout::NodeWithStyle const&) const;
    AlignContent align_content() const;
    AlignItems align_items() const;
    AlignSelf align_self() const;
    Appearance appearance() const;
    CSS::Filter backdrop_filter() const;
    CSS::Filter filter() const;
    float opacity() const;
    Visibility visibility() const;
    ImageRendering image_rendering() const;
    JustifyContent justify_content() const;
    JustifyItems justify_items() const;
    JustifySelf justify_self() const;
    Overflow overflow_x() const;
    Overflow overflow_y() const;
    Vector<CSS::ShadowData> box_shadow(Layout::Node const&) const;
    BoxSizing box_sizing() const;
    PointerEvents pointer_events() const;
    Variant<CSS::VerticalAlign, CSS::LengthPercentage> vertical_align() const;
    Optional<Gfx::FontVariantAlternates> font_variant_alternates() const;
    FontVariantCaps font_variant_caps() const;
    Optional<Gfx::FontVariantEastAsian> font_variant_east_asian() const;
    FontVariantEmoji font_variant_emoji() const;
    Optional<Gfx::FontVariantLigatures> font_variant_ligatures() const;
    Optional<Gfx::FontVariantNumeric> font_variant_numeric() const;
    FontVariantPosition font_variant_position() const;
    Optional<FlyString> font_language_override() const;
    Optional<HashMap<FlyString, IntegerOrCalculated>> font_feature_settings() const;
    Optional<HashMap<FlyString, NumberOrCalculated>> font_variation_settings() const;
    CSS::GridTrackSizeList grid_auto_columns() const;
    CSS::GridTrackSizeList grid_auto_rows() const;
    CSS::GridTrackSizeList grid_template_columns() const;
    CSS::GridTrackSizeList grid_template_rows() const;
    [[nodiscard]] CSS::GridAutoFlow grid_auto_flow() const;
    CSS::GridTrackPlacement grid_column_end() const;
    CSS::GridTrackPlacement grid_column_start() const;
    CSS::GridTrackPlacement grid_row_end() const;
    CSS::GridTrackPlacement grid_row_start() const;
    BorderCollapse border_collapse() const;
    Vector<Vector<String>> grid_template_areas() const;
    ObjectFit object_fit() const;
    CSS::ObjectPosition object_position() const;
    TableLayout table_layout() const;
    Direction direction() const;
    UnicodeBidi unicode_bidi() const;
    WritingMode writing_mode() const;
    UserSelect user_select() const;
    Isolation isolation() const;
    CSS::Containment contain() const;
    MixBlendMode mix_blend_mode() const;

    static Vector<CSS::Transformation> transformations_for_style_value(CSSStyleValue const& value);
    Vector<CSS::Transformation> transformations() const;
    TransformBox transform_box() const;
    CSS::TransformOrigin transform_origin() const;
    Optional<CSS::Transformation> rotate() const;
    Optional<CSS::Transformation> translate() const;
    Optional<CSS::Transformation> scale() const;

    MaskType mask_type() const;
    Color stop_color() const;
    float stop_opacity() const;
    float fill_opacity() const;
    StrokeLinecap stroke_linecap() const;
    StrokeLinejoin stroke_linejoin() const;
    NumberOrCalculated stroke_miterlimit() const;
    float stroke_opacity() const;
    FillRule fill_rule() const;
    ClipRule clip_rule() const;

    Gfx::Font const& first_available_computed_font() const { return m_font_list->first(); }

    Gfx::FontCascadeList const& computed_font_list() const
    {
        VERIFY(m_font_list);
        return *m_font_list;
    }

    void set_computed_font_list(NonnullRefPtr<Gfx::FontCascadeList> font_list) const
    {
        m_font_list = move(font_list);
    }

    [[nodiscard]] CSSPixels compute_line_height(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const;

    [[nodiscard]] CSSPixels line_height() const { return *m_line_height; }
    void set_line_height(Badge<StyleComputer> const&, CSSPixels line_height) { m_line_height = line_height; }

    bool operator==(ComputedProperties const&) const;

    Positioning position() const;
    Optional<int> z_index() const;

    void set_math_depth(int math_depth);
    int math_depth() const { return m_math_depth; }

    QuotesData quotes() const;
    Vector<CounterData> counter_data(PropertyID) const;

    ScrollbarWidth scrollbar_width() const;

    static NonnullRefPtr<Gfx::Font const> font_fallback(bool monospace, bool bold, float point_size);

    static float resolve_opacity_value(CSSStyleValue const& value);

    bool did_match_any_hover_rules() const { return m_did_match_any_hover_rules; }
    void set_did_match_any_hover_rules() { m_did_match_any_hover_rules = true; }

private:
    friend class StyleComputer;

    ComputedProperties();

    virtual void visit_edges(Visitor&) override;

    Overflow overflow(CSS::PropertyID) const;
    Vector<CSS::ShadowData> shadow(CSS::PropertyID, Layout::Node const&) const;

    GC::Ptr<CSS::CSSStyleDeclaration const> m_animation_name_source;
    GC::Ptr<CSS::CSSStyleDeclaration const> m_transition_property_source;

    Array<RefPtr<CSSStyleValue const>, number_of_properties> m_property_values;
    Array<u8, ceil_div(number_of_properties, 8uz)> m_property_important {};
    Array<u8, ceil_div(number_of_properties, 8uz)> m_property_inherited {};

    HashMap<CSS::PropertyID, NonnullRefPtr<CSSStyleValue const>> m_animated_property_values;

    int m_math_depth { InitialValues::math_depth() };
    mutable RefPtr<Gfx::FontCascadeList> m_font_list;

    Optional<CSSPixels> m_line_height;

    bool m_did_match_any_hover_rules { false };
};

}
