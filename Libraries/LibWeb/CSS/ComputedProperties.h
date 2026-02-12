/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
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
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/PseudoClassBitmap.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

struct TransitionProperties {
    Vector<PropertyID> properties;
    double duration;
    EasingFunction timing_function;
    double delay;
    TransitionBehavior transition_behavior;
};

enum class AnimatedPropertyResultOfTransition : u8 {
    No,
    Yes
};

class WEB_API ComputedProperties final : public JS::Cell {
    GC_CELL(ComputedProperties, JS::Cell);
    GC_DECLARE_ALLOCATOR(ComputedProperties);

public:
    static constexpr double normal_line_height_scale = 1.15;

    virtual ~ComputedProperties() override;

    template<typename Callback>
    inline void for_each_property(Callback callback) const
    {
        for (size_t i = 0; i < m_property_values.size(); ++i) {
            if (m_property_values[i])
                callback(static_cast<PropertyID>(i + to_underlying(first_longhand_property_id)), *m_property_values[i]);
        }
    }

    enum class Inherited {
        No,
        Yes
    };

    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& animated_property_values() const { return m_animated_property_values; }
    void reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>);

    bool is_property_important(PropertyID property_id) const;
    bool is_property_inherited(PropertyID property_id) const;
    bool is_animated_property_inherited(PropertyID property_id) const;
    bool is_animated_property_result_of_transition(PropertyID property_id) const;
    void set_property_important(PropertyID, Important);
    void set_property_inherited(PropertyID, Inherited);
    void set_animated_property_inherited(PropertyID, Inherited);
    void set_animated_property_result_of_transition(PropertyID, AnimatedPropertyResultOfTransition);

    void set_property(PropertyID, NonnullRefPtr<StyleValue const> value, Inherited = Inherited::No, Important = Important::No);
    void set_property_without_modifying_flags(PropertyID, NonnullRefPtr<StyleValue const> value);
    void set_animated_property(PropertyID, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition, Inherited = Inherited::No);
    void remove_animated_property(PropertyID);
    enum class WithAnimationsApplied {
        No,
        Yes,
    };
    StyleValue const& property(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    void revert_property(PropertyID, ComputedProperties const& style_for_revert);

    Size size_value(PropertyID) const;
    [[nodiscard]] Variant<LengthPercentage, NormalGap> gap_value(PropertyID) const;
    Length length(PropertyID) const;
    LengthBox length_box(PropertyID left_id, PropertyID top_id, PropertyID right_id, PropertyID bottom_id, LengthPercentageOrAuto const& default_value) const;
    Color color_or_fallback(PropertyID, ColorResolutionContext, Color fallback) const;
    HashMap<PropertyID, StyleValueVector> assemble_coordinated_value_list(PropertyID base_property_id, Vector<PropertyID> const& property_ids) const;
    ColorInterpolation color_interpolation() const;
    PreferredColorScheme color_scheme(PreferredColorScheme, Optional<Vector<String> const&> document_supported_schemes) const;
    TextAnchor text_anchor() const;
    TextAlign text_align() const;
    TextJustify text_justify() const;
    TextOverflow text_overflow() const;
    TextRendering text_rendering() const;
    CSSPixels text_underline_offset() const;
    TextUnderlinePosition text_underline_position() const;
    Vector<BackgroundLayerData> background_layers() const;
    BackgroundBox background_color_clip() const;
    Length border_spacing_horizontal(Layout::Node const&) const;
    Length border_spacing_vertical(Layout::Node const&) const;
    CaptionSide caption_side() const;
    Clip clip() const;
    Display display() const;
    Float float_() const;
    Color caret_color(Layout::NodeWithStyle const&) const;
    Clear clear() const;
    ColumnSpan column_span() const;
    struct ContentDataAndQuoteNestingLevel {
        ContentData content_data;
        u32 final_quote_nesting_level { 0 };
    };
    ContentDataAndQuoteNestingLevel content(DOM::AbstractElement&, u32 initial_quote_nesting_level) const;
    ContentVisibility content_visibility() const;
    Vector<CursorData> cursor() const;
    Variant<Length, double> tab_size() const;
    WhiteSpaceCollapse white_space_collapse() const;
    WhiteSpaceTrimData white_space_trim() const;
    WordBreak word_break() const;
    CSSPixels word_spacing() const;
    CSSPixels letter_spacing() const;
    LineStyle line_style(PropertyID) const;
    OutlineStyle outline_style() const;
    Vector<TextDecorationLine> text_decoration_line() const;
    TextDecorationStyle text_decoration_style() const;
    TextDecorationThickness text_decoration_thickness() const;
    TextTransform text_transform() const;
    Vector<ShadowData> text_shadow(Layout::Node const&) const;
    TextIndentData text_indent() const;
    TextWrapMode text_wrap_mode() const;
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
    Filter backdrop_filter() const;
    Filter filter() const;
    float opacity() const;
    Visibility visibility() const;
    ImageRendering image_rendering() const;
    JustifyContent justify_content() const;
    JustifyItems justify_items() const;
    JustifySelf justify_self() const;
    Overflow overflow_x() const;
    Overflow overflow_y() const;
    Vector<ShadowData> box_shadow(Layout::Node const&) const;
    BoxSizing box_sizing() const;
    PointerEvents pointer_events() const;
    Variant<VerticalAlign, LengthPercentage> vertical_align() const;
    FontFeatureData font_feature_data() const;
    Optional<Gfx::FontVariantAlternates> font_variant_alternates() const;
    FontVariantCaps font_variant_caps() const;
    Optional<Gfx::FontVariantEastAsian> font_variant_east_asian() const;
    FontVariantEmoji font_variant_emoji() const;
    Optional<Gfx::FontVariantLigatures> font_variant_ligatures() const;
    Optional<Gfx::FontVariantNumeric> font_variant_numeric() const;
    FontVariantPosition font_variant_position() const;
    FontKerning font_kerning() const;
    Optional<FlyString> font_language_override() const;
    HashMap<FlyString, u8> font_feature_settings() const;
    HashMap<FlyString, double> font_variation_settings() const;
    GridTrackSizeList grid_auto_columns() const;
    GridTrackSizeList grid_auto_rows() const;
    GridTrackSizeList grid_template_columns() const;
    GridTrackSizeList grid_template_rows() const;
    [[nodiscard]] GridAutoFlow grid_auto_flow() const;
    GridTrackPlacement grid_column_end() const;
    GridTrackPlacement grid_column_start() const;
    GridTrackPlacement grid_row_end() const;
    GridTrackPlacement grid_row_start() const;
    BorderCollapse border_collapse() const;
    CSS::EmptyCells empty_cells() const;
    Vector<Vector<String>> grid_template_areas() const;
    ObjectFit object_fit() const;
    Position object_position() const;
    TableLayout table_layout() const;
    Direction direction() const;
    UnicodeBidi unicode_bidi() const;
    WritingMode writing_mode() const;
    UserSelect user_select() const;
    Isolation isolation() const;
    TouchActionData touch_action() const;
    Containment contain() const;
    ContainerType container_type() const;
    MixBlendMode mix_blend_mode() const;
    Optional<FlyString> view_transition_name() const;
    struct AnimationProperties {
        Variant<double, String> duration;
        EasingFunction timing_function;
        double iteration_count;
        AnimationDirection direction;
        AnimationPlayState play_state;
        double delay;
        AnimationFillMode fill_mode;
        AnimationComposition composition;
        FlyString name;
        GC::Ptr<Animations::AnimationTimeline> timeline;
    };
    Vector<AnimationProperties> animations(DOM::AbstractElement const&) const;
    Vector<TransitionProperties> transitions() const;

    Display display_before_box_type_transformation() const;
    void set_display_before_box_type_transformation(Display value);

    static Vector<NonnullRefPtr<TransformationStyleValue const>> transformations_for_style_value(StyleValue const& value);
    Vector<NonnullRefPtr<TransformationStyleValue const>> transformations() const;
    TransformBox transform_box() const;
    TransformOrigin transform_origin() const;
    TransformStyle transform_style() const;
    RefPtr<TransformationStyleValue const> rotate() const;
    RefPtr<TransformationStyleValue const> translate() const;
    RefPtr<TransformationStyleValue const> scale() const;
    Optional<CSSPixels> perspective() const;
    Position perspective_origin() const;

    MaskType mask_type() const;
    float stop_opacity() const;
    float fill_opacity() const;
    Vector<Variant<LengthPercentage, float>> stroke_dasharray() const;
    StrokeLinecap stroke_linecap() const;
    StrokeLinejoin stroke_linejoin() const;
    double stroke_miterlimit() const;
    float stroke_opacity() const;
    FillRule fill_rule() const;
    ClipRule clip_rule() const;
    float flood_opacity() const;
    CSS::ShapeRendering shape_rendering() const;
    PaintOrderList paint_order() const;

    WillChange will_change() const;

    ValueComparingRefPtr<Gfx::FontCascadeList const> cached_computed_font_list() const { return m_cached_computed_font_list; }
    ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> computed_font_list(FontComputer const&) const;
    ValueComparingNonnullRefPtr<Gfx::Font const> first_available_computed_font(FontComputer const&) const;

    MathStyle math_style() const;
    int math_depth() const;
    [[nodiscard]] CSSPixels line_height() const;
    [[nodiscard]] CSSPixels font_size() const;
    double font_weight() const;
    Percentage font_width() const;
    int font_slope() const;
    FontOpticalSizing font_optical_sizing() const;

    bool operator==(ComputedProperties const&) const;

    Positioning position() const;
    Optional<int> z_index() const;

    QuotesData quotes() const;
    Vector<CounterData> counter_data(PropertyID) const;

    ScrollbarColorData scrollbar_color(Layout::NodeWithStyle const& layout_node) const;
    ScrollbarWidth scrollbar_width() const;
    Resize resize() const;

    static NonnullRefPtr<Gfx::Font const> font_fallback(bool monospace, bool bold, float point_size);

    bool has_attempted_match_against_pseudo_class(PseudoClass pseudo_class) const
    {
        return m_attempted_pseudo_class_matches.get(pseudo_class);
    }

    void set_attempted_pseudo_class_matches(PseudoClassBitmap const& results)
    {
        m_attempted_pseudo_class_matches = results;
    }

private:
    ComputedProperties();

    virtual void visit_edges(Visitor&) override;

    Overflow overflow(PropertyID) const;
    Vector<ShadowData> shadow(PropertyID, Layout::Node const&) const;
    Position position_value(PropertyID) const;

    Array<RefPtr<StyleValue const>, number_of_longhand_properties> m_property_values;
    Array<u8, ceil_div(number_of_longhand_properties, 8uz)> m_property_important {};
    Array<u8, ceil_div(number_of_longhand_properties, 8uz)> m_property_inherited {};
    Array<u8, ceil_div(number_of_longhand_properties, 8uz)> m_animated_property_inherited {};
    Array<u8, ceil_div(number_of_longhand_properties, 8uz)> m_animated_property_result_of_transition {};

    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_animated_property_values;

    Display m_display_before_box_type_transformation { InitialValues::display() };

    RefPtr<Gfx::FontCascadeList const> m_cached_computed_font_list;
    RefPtr<Gfx::Font const> m_cached_first_available_computed_font;
    void clear_computed_font_list_cache()
    {
        m_cached_computed_font_list = nullptr;
        m_cached_first_available_computed_font = nullptr;
    }

    Optional<CSSPixels> m_line_height;

    PseudoClassBitmap m_attempted_pseudo_class_matches;
};

}
