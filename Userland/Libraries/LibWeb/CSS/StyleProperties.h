/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleProperty.h>

namespace Web::CSS {

class StyleProperties : public RefCounted<StyleProperties> {
public:
    static constexpr size_t number_of_properties = to_underlying(CSS::last_property_id) + 1;

private:
    struct Data : public RefCounted<Data> {
        friend class StyleComputer;

        NonnullRefPtr<Data> clone() const;

        // FIXME: These need protection from GC!
        JS::GCPtr<CSS::CSSStyleDeclaration const> m_animation_name_source;
        JS::GCPtr<CSS::CSSStyleDeclaration const> m_transition_property_source;

        Array<RefPtr<CSSStyleValue const>, number_of_properties> m_property_values;
        Array<u8, ceil_div(number_of_properties, 8uz)> m_property_important {};
        Array<u8, ceil_div(number_of_properties, 8uz)> m_property_inherited {};

        HashMap<CSS::PropertyID, NonnullRefPtr<CSSStyleValue const>> m_animated_property_values;

        int m_math_depth { InitialValues::math_depth() };
        mutable RefPtr<Gfx::FontCascadeList> m_font_list;

        Optional<CSSPixels> m_line_height;
    };

public:
    StyleProperties() = default;

    static NonnullRefPtr<StyleProperties> create() { return adopt_ref(*new StyleProperties); }
    NonnullRefPtr<StyleProperties> clone() const;

    template<typename Callback>
    inline void for_each_property(Callback callback) const
    {
        for (size_t i = 0; i < m_data->m_property_values.size(); ++i) {
            if (m_data->m_property_values[i])
                callback((CSS::PropertyID)i, *m_data->m_property_values[i]);
        }
    }

    enum class Inherited {
        No,
        Yes
    };

    HashMap<CSS::PropertyID, NonnullRefPtr<CSSStyleValue const>> const& animated_property_values() const { return m_data->m_animated_property_values; }
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
    NonnullRefPtr<CSSStyleValue const> property(CSS::PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    RefPtr<CSSStyleValue const> maybe_null_property(CSS::PropertyID) const;
    void revert_property(CSS::PropertyID, StyleProperties const& style_for_revert);

    JS::GCPtr<CSS::CSSStyleDeclaration const> animation_name_source() const { return m_data->m_animation_name_source; }
    void set_animation_name_source(JS::GCPtr<CSS::CSSStyleDeclaration const> declaration) { m_data->m_animation_name_source = declaration; }

    JS::GCPtr<CSS::CSSStyleDeclaration const> transition_property_source() const { return m_data->m_transition_property_source; }
    void set_transition_property_source(JS::GCPtr<CSS::CSSStyleDeclaration const> declaration) { m_data->m_transition_property_source = declaration; }

    CSS::Size size_value(CSS::PropertyID) const;
    LengthPercentage length_percentage_or_fallback(CSS::PropertyID, LengthPercentage const& fallback) const;
    Optional<LengthPercentage> length_percentage(CSS::PropertyID) const;
    LengthBox length_box(CSS::PropertyID left_id, CSS::PropertyID top_id, CSS::PropertyID right_id, CSS::PropertyID bottom_id, const CSS::Length& default_value) const;
    Color color_or_fallback(CSS::PropertyID, Layout::NodeWithStyle const&, Color fallback) const;
    Optional<CSS::TextAnchor> text_anchor() const;
    Optional<CSS::TextAlign> text_align() const;
    Optional<CSS::TextJustify> text_justify() const;
    Optional<CSS::TextOverflow> text_overflow() const;
    CSS::Length border_spacing_horizontal() const;
    CSS::Length border_spacing_vertical() const;
    Optional<CSS::CaptionSide> caption_side() const;
    CSS::Clip clip() const;
    CSS::Display display() const;
    Optional<CSS::Float> float_() const;
    Optional<CSS::Clear> clear() const;
    Optional<CSS::ColumnSpan> column_span() const;
    struct ContentDataAndQuoteNestingLevel {
        CSS::ContentData content_data;
        u32 final_quote_nesting_level { 0 };
    };
    ContentDataAndQuoteNestingLevel content(DOM::Element&, u32 initial_quote_nesting_level) const;
    Optional<CSS::ContentVisibility> content_visibility() const;
    Optional<CSS::Cursor> cursor() const;
    Variant<LengthOrCalculated, NumberOrCalculated> tab_size() const;
    Optional<CSS::WhiteSpace> white_space() const;
    Optional<CSS::LineStyle> line_style(CSS::PropertyID) const;
    Optional<CSS::OutlineStyle> outline_style() const;
    Vector<CSS::TextDecorationLine> text_decoration_line() const;
    Optional<CSS::TextDecorationStyle> text_decoration_style() const;
    Optional<CSS::TextTransform> text_transform() const;
    Vector<CSS::ShadowData> text_shadow(Layout::Node const&) const;
    Optional<CSS::ListStyleType> list_style_type() const;
    Optional<CSS::ListStylePosition> list_style_position() const;
    Optional<CSS::FlexDirection> flex_direction() const;
    Optional<CSS::FlexWrap> flex_wrap() const;
    Optional<CSS::FlexBasis> flex_basis() const;
    float flex_grow() const;
    float flex_shrink() const;
    int order() const;
    Optional<Color> accent_color(Layout::NodeWithStyle const&) const;
    Optional<CSS::AlignContent> align_content() const;
    Optional<CSS::AlignItems> align_items() const;
    Optional<CSS::AlignSelf> align_self() const;
    Optional<CSS::Appearance> appearance() const;
    CSS::BackdropFilter backdrop_filter() const;
    float opacity() const;
    Optional<CSS::Visibility> visibility() const;
    Optional<CSS::ImageRendering> image_rendering() const;
    Optional<CSS::JustifyContent> justify_content() const;
    Optional<CSS::JustifyItems> justify_items() const;
    Optional<CSS::JustifySelf> justify_self() const;
    Optional<CSS::Overflow> overflow_x() const;
    Optional<CSS::Overflow> overflow_y() const;
    Vector<CSS::ShadowData> box_shadow(Layout::Node const&) const;
    Optional<CSS::BoxSizing> box_sizing() const;
    Optional<CSS::PointerEvents> pointer_events() const;
    Variant<CSS::VerticalAlign, CSS::LengthPercentage> vertical_align() const;
    Optional<CSS::FontVariant> font_variant() const;
    Optional<FlyString> font_language_override() const;
    CSS::GridTrackSizeList grid_auto_columns() const;
    CSS::GridTrackSizeList grid_auto_rows() const;
    CSS::GridTrackSizeList grid_template_columns() const;
    CSS::GridTrackSizeList grid_template_rows() const;
    [[nodiscard]] CSS::GridAutoFlow grid_auto_flow() const;
    CSS::GridTrackPlacement grid_column_end() const;
    CSS::GridTrackPlacement grid_column_start() const;
    CSS::GridTrackPlacement grid_row_end() const;
    CSS::GridTrackPlacement grid_row_start() const;
    Optional<CSS::BorderCollapse> border_collapse() const;
    Vector<Vector<String>> grid_template_areas() const;
    Optional<CSS::ObjectFit> object_fit() const;
    CSS::ObjectPosition object_position() const;
    Optional<CSS::TableLayout> table_layout() const;
    Optional<CSS::Direction> direction() const;

    static Vector<CSS::Transformation> transformations_for_style_value(CSSStyleValue const& value);
    Vector<CSS::Transformation> transformations() const;
    Optional<CSS::TransformBox> transform_box() const;
    CSS::TransformOrigin transform_origin() const;

    Optional<CSS::MaskType> mask_type() const;
    Color stop_color() const;
    float stop_opacity() const;
    float fill_opacity() const;
    float stroke_opacity() const;
    Optional<CSS::FillRule> fill_rule() const;
    Optional<CSS::ClipRule> clip_rule() const;

    Gfx::Font const& first_available_computed_font() const { return m_data->m_font_list->first(); }

    Gfx::FontCascadeList const& computed_font_list() const
    {
        VERIFY(m_data->m_font_list);
        return *m_data->m_font_list;
    }

    void set_computed_font_list(NonnullRefPtr<Gfx::FontCascadeList> font_list) const
    {
        m_data->m_font_list = move(font_list);
    }

    [[nodiscard]] CSSPixels compute_line_height(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const;

    [[nodiscard]] CSSPixels line_height() const { return *m_data->m_line_height; }
    void set_line_height(Badge<StyleComputer> const&, CSSPixels line_height) { m_data->m_line_height = line_height; }

    bool operator==(StyleProperties const&) const;

    Optional<CSS::Positioning> position() const;
    Optional<int> z_index() const;

    void set_math_depth(int math_depth);
    int math_depth() const { return m_data->m_math_depth; }

    QuotesData quotes() const;
    Vector<CounterData> counter_data(PropertyID) const;

    Optional<CSS::ScrollbarWidth> scrollbar_width() const;

    static NonnullRefPtr<Gfx::Font const> font_fallback(bool monospace, bool bold);

    static float resolve_opacity_value(CSSStyleValue const& value);

private:
    friend class StyleComputer;

    Optional<CSS::Overflow> overflow(CSS::PropertyID) const;
    Vector<CSS::ShadowData> shadow(CSS::PropertyID, Layout::Node const&) const;

    AK::CopyOnWrite<StyleProperties::Data> m_data;
};

}
