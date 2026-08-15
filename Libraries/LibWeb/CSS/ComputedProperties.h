/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedBitmap.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/CSSAnimationProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/PseudoClassBitmap.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::CSS {

class AnimatedProperties;
class CSSStyleSheet;
class StyleComputer;

}

namespace Web::DOM {

class Element;

}

namespace Web::Layout {

class NodeWithStyle;

}

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

class ComputedProperties final : public RefCounted<ComputedProperties> {
public:
    ~ComputedProperties();

    enum class WithAnimationsApplied {
        No,
        Yes,
    };

    enum class Inherited {
        No,
        Yes
    };

private:
    class Data;

public:
    class Builder {
    public:
        Builder();

        ComputedProperties& style() { return *m_style; }
        ComputedProperties const& style() const { return *m_style; }
        NonnullRefPtr<ComputedProperties> build() &&;

        bool font_metrics_depend_on_viewport_metrics() const { return m_font_metrics_depend_on_viewport_metrics; }
        Display display() const { return style().display(); }
        StyleValue const& property(PropertyID property_id, WithAnimationsApplied with_animations_applied = WithAnimationsApplied::Yes) const { return style().property(property_id, with_animations_applied); }
        [[nodiscard]] CSSPixels line_height(FontComputer const& font_computer) const { return style().line_height(font_computer); }
        ValueComparingNonnullRefPtr<Gfx::Font const> first_available_computed_font(FontComputer const& font_computer) const { return style().first_available_computed_font(font_computer); }

        void set_has_pseudo_element_styles(u64);
        void set_property_important(PropertyID, Important);
        void set_property_inherited(PropertyID, Inherited);
        void set_depends_on_viewport_metrics();
        void set_font_metrics_depend_on_viewport_metrics();
        void set_in_display_none_subtree();
        void clear_in_display_none_subtree();
        void set_has_pseudo_element_style(PseudoElement);

        void set_property(PropertyID, NonnullRefPtr<StyleValue const> value, Inherited = Inherited::No, Important = Important::No);
        // The wrapper-carrying store funnel: dual-writes the Rust table and the wrapper cache.
        // `style_sheet_source_slot` is the winning declaration's cascade source slot when its
        // value carries style sheet context, and -1 otherwise; only the longhand drive's batch
        // executor passes one.
        void set_property_without_modifying_flags(PropertyID, NonnullRefPtr<StyleValue const> value, i64 style_sheet_source_slot = -1);
        // The wrapper-free store the longhand drive uses for values it already holds as Rust
        // data: writes the table and invalidates the cached wrapper; property() mints one on
        // demand. `style_sheet` is the sheet whose rule supplied the winning declaration, kept
        // for stamping sheet context onto the wrapper the mint produces.
        void set_property_data_from_drive(PropertyID, void const* value_data, i64 style_sheet_source_slot, GC::Ptr<CSSStyleSheet> style_sheet);
        void set_display_before_box_type_transformation(Display);

        bool has_effective_color_scheme() const { return m_data->effective_color_scheme.has_value(); }
        void set_effective_color_scheme(PreferredColorScheme color_scheme) { m_data->effective_color_scheme = color_scheme; }
        void clear_effective_color_scheme() { m_data->effective_color_scheme.clear(); }

        HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& inheritance_dependent_specified_values() const { return m_data->inheritance_dependent_specified_values; }
        void add_inheritance_dependent_specified_value(PropertyID property_id, NonnullRefPtr<StyleValue const> value) { m_data->inheritance_dependent_specified_values.set(property_id, move(value)); }
        void remove_inheritance_dependent_specified_value(PropertyID property_id) { m_data->inheritance_dependent_specified_values.remove(property_id); }

        RefPtr<StyleValue const> raw_cascaded_font_size() const { return m_data->raw_cascaded_font_size; }
        void set_raw_cascaded_font_size(NonnullRefPtr<StyleValue const> value) { m_data->raw_cascaded_font_size = move(value); }

    private:
        friend class ComputedProperties;

        Builder(ComputedProperties const&);
        Builder(ComputedValues const&);
        Data& data() { return *m_data; }
        Data const& data() const { return *m_data; }

        NonnullRefPtr<Data> m_data;
        NonnullRefPtr<ComputedProperties> m_style;
        bool m_depends_on_viewport_metrics { false };
        bool m_font_metrics_depend_on_viewport_metrics { false };
        bool m_in_display_none_subtree { false };
    };

    static NonnullRefPtr<ComputedProperties> create(Builder&&);
    static Builder create_builder();
    static Builder create_builder_with_base_values_from(ComputedProperties const&);
    static Builder create_builder_with_base_values_from(ComputedValues const&);

    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& animated_property_values() const;
    RefPtr<AnimatedProperties const> animated_properties_snapshot() const;
    bool has_animated_property(PropertyID property_id) const;
    void reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>);

    bool is_property_important(PropertyID property_id) const;
    bool is_property_inherited(PropertyID property_id) const;
    bool is_animated_property_inherited(PropertyID property_id) const;
    bool is_animated_property_result_of_transition(PropertyID property_id) const;
    bool depends_on_viewport_metrics() const { return m_depends_on_viewport_metrics; }
    bool font_metrics_depend_on_viewport_metrics() const { return m_font_metrics_depend_on_viewport_metrics; }
    // Whether the element this style was computed for has computed display none, or is a descendant of one that does.
    bool in_display_none_subtree() const { return m_in_display_none_subtree; }
    bool has_pseudo_element_style(PseudoElement) const;
    void set_depends_on_viewport_metrics(Badge<StyleComputer>);
    void set_font_metrics_depend_on_viewport_metrics(Badge<StyleComputer>);
    void set_animated_property(Badge<StyleComputer>, PropertyID, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition, Inherited = Inherited::No);
    void set_animated_property(Badge<DOM::Element>, PropertyID, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition, Inherited = Inherited::No);
    void clear_animated_properties(Badge<StyleComputer>);
    StyleValue const& property(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;

    Color color(PropertyID, ColorResolutionContext) const;
    HashMap<PropertyID, StyleValueVector> assemble_coordinated_value_list(PropertyID base_property_id, Vector<PropertyID> const& property_ids) const;
    PreferredColorScheme color_scheme(PreferredColorScheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const;
    TextOverflow text_overflow() const;
    TextRendering text_rendering() const;
    CSSPixels text_underline_offset() const;
    CSSPixels border_spacing_horizontal() const;
    CSSPixels border_spacing_vertical() const;
    CaptionSide caption_side() const;
    Display display() const;
    Float float_() const;
    Color caret_color(ColorResolutionContext const&) const;
    Clear clear() const;
    ContentVisibility content_visibility() const;
    WhiteSpaceTrimData white_space_trim() const;
    CSSPixels word_spacing() const;
    CSSPixels letter_spacing() const;
    Vector<TextDecorationLine> text_decoration_line() const;
    TextDecorationStyle text_decoration_style() const;
    TextDecorationThickness text_decoration_thickness() const;
    ListStyleType list_style_type(StyleScope const&) const;
    double flex_grow() const;
    double flex_shrink() const;
    i32 order() const;
    Color accent_color(ColorResolutionContext const&) const;
    Filter backdrop_filter() const;
    Filter filter() const;
    float opacity() const;
    Visibility visibility() const;
    ImageRendering image_rendering() const;
    Overflow overflow_x() const;
    Overflow overflow_y() const;
    BoxSizing box_sizing() const;
    Variant<VerticalAlign, LengthPercentage> vertical_align() const;
    FontFeatureData font_feature_data() const;
    Optional<FontVariantAlternates> font_variant_alternates() const;
    FontVariantCaps font_variant_caps() const;
    Optional<FontVariantEastAsian> font_variant_east_asian() const;
    FontVariantEmoji font_variant_emoji() const;
    Optional<FontVariantLigatures> font_variant_ligatures() const;
    Optional<FontVariantNumeric> font_variant_numeric() const;
    FontVariantPosition font_variant_position() const;
    FontKerning font_kerning() const;
    Optional<Utf16FlyString> font_language_override() const;
    HashMap<Utf16FlyString, u8> font_feature_settings() const;
    HashMap<Utf16FlyString, double> font_variation_settings() const;
    [[nodiscard]] GridAutoFlow grid_auto_flow() const;
    BorderCollapse border_collapse() const;
    CSS::EmptyCells empty_cells() const;
    TableLayout table_layout() const;
    Direction direction() const;
    UnicodeBidi unicode_bidi() const;
    WritingMode writing_mode() const;
    Isolation isolation() const;
    AspectRatio aspect_ratio() const;
    Containment contain() const;
    Vector<Utf16FlyString> container_name() const;
    ContainerType container_type() const;
    MixBlendMode mix_blend_mode() const;
    Vector<AnimationProperties> animations(DOM::AbstractElement const&) const;
    Vector<TransitionProperties> transitions() const;

    Display display_before_box_type_transformation() const;

    Vector<NonnullRefPtr<TransformationStyleValue const>> transformations() const;
    TransformBox transform_box() const;
    TransformOrigin transform_origin() const;
    TransformStyle transform_style() const;
    BackfaceVisibility backface_visibility() const;
    RefPtr<TransformationStyleValue const> rotate() const;
    RefPtr<TransformationStyleValue const> translate() const;
    RefPtr<TransformationStyleValue const> scale() const;
    Optional<CSSPixels> perspective() const;
    Position perspective_origin() const;

    float stop_opacity() const;
    float flood_opacity() const;

    ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> computed_font_list(FontComputer const&) const;
    ValueComparingNonnullRefPtr<Gfx::Font const> first_available_computed_font(FontComputer const&) const;

    int math_depth() const;
    [[nodiscard]] static CSSPixels normal_line_height(Gfx::FontPixelMetrics const&);
    [[nodiscard]] CSSPixels line_height(FontComputer const&) const;
    [[nodiscard]] LineHeightData line_height_data() const;
    [[nodiscard]] CSSPixels font_size() const;
    double font_weight() const;
    Percentage font_width() const;
    int font_slope() const;
    FontOpticalSizing font_optical_sizing() const;

    Positioning position() const;
    Optional<int> z_index() const;

    ScrollbarColorData scrollbar_color(ColorResolutionContext const&) const;
    Resize resize() const;

    static NonnullRefPtr<Gfx::Font const> font_fallback(bool monospace, bool bold, float point_size);

    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& inheritance_dependent_specified_values() const { return m_data->inheritance_dependent_specified_values; }

    RefPtr<StyleValue const> raw_cascaded_font_size() const { return m_data->raw_cascaded_font_size; }
    [[nodiscard]] size_t retained_size_in_bytes() const;

    // The drive's frozen computed longhand table, for transfer onto the ComputedValues built
    // from these properties.
    ComputedValuesFFI::ComputedLonghandTable const* computed_longhand_table() const { return m_data->computed_longhand_table; }

    // The sparse set of longhands whose effective value differs from the table's stored value:
    // the animated overlay and the unevaluated longhands' currentcolor-dependent specified-value
    // preference. Exactly what property() returns, without minting a wrapper per longhand to
    // find out.
    void collect_effective_longhand_overrides(Vector<u16>& properties, Vector<void const*>& values) const;

    // How many C++ longhand wrappers have been minted process-wide, counting the on-demand
    // mints property() performs and the specified-value wrappers the drive's side effects
    // still need. Exposed through internals for the laziness measurements.
    static u64 longhand_wrappers_minted();
    static void reset_longhand_wrappers_minted();
    static void count_longhand_wrapper_mint(Badge<StyleComputer>);

private:
    friend class Layout::NodeWithStyle;
    friend class StyleComputer;

    NonnullRefPtr<ComputedProperties> copy_without_animations() const;

    class Data final : public RefCounted<Data> {
    public:
        Data();
        ~Data();

        // The lazily populated wrapper cache over the longhand table: a slot holds the wrapper
        // a store funnel carried or the one property() minted on demand, and is cleared when
        // the drive stores new table data for the longhand.
        mutable Array<RefPtr<StyleValue const>, number_of_longhand_properties> property_values;
        // The source of truth for computed longhand values, written by both store funnels and
        // frozen when the ComputedProperties is created.
        ComputedValuesFFI::ComputedLonghandTable* computed_longhand_table { nullptr };
        AK::FixedBitmap<number_of_longhand_properties> property_important { false };
        AK::FixedBitmap<number_of_longhand_properties> property_inherited { false };
        // Which longhands a drive over these properties (or the styles they were seeded from)
        // has evaluated and stored; an evaluated longhand's value always comes from the table,
        // while an unevaluated one keeps computed_style_value_for_inheritance's preference for
        // a recorded currentcolor-dependent specified value.
        AK::FixedBitmap<number_of_longhand_properties> property_evaluated { false };
        // The style sheet whose rule supplied a longhand's winning declaration, for longhands
        // whose stored value carries style sheet context and awaits an on-demand wrapper mint.
        // Held weakly, like the cascade's own declaration sources; the wrappers themselves
        // only extract state from the sheet, so a collected sheet degrades to no stamping.
        HashMap<PropertyID, GC::Weak<CSSStyleSheet>> style_sheet_sources;

        Display display_before_box_type_transformation { InitialValues::display() };
        u64 pseudo_element_styles { 0 };

        Optional<CSSPixels> line_height;
        Optional<PreferredColorScheme> effective_color_scheme;

        HashMap<PropertyID, NonnullRefPtr<StyleValue const>> inheritance_dependent_specified_values;
        RefPtr<StyleValue const> raw_cascaded_font_size;
    };

    ComputedProperties(NonnullRefPtr<Data const>, bool depends_on_viewport_metrics, bool font_metrics_depend_on_viewport_metrics);

    Overflow overflow(PropertyID) const;
    Position position_value(PropertyID) const;

    Data const& data() const { return *m_data; }
    AnimatedProperties const& animated_properties() const;
    AnimatedProperties& mutable_animated_properties();
    void set_animated_property_internal(PropertyID, NonnullRefPtr<StyleValue const>, AnimatedPropertyResultOfTransition, Inherited);
    NonnullRefPtr<Data const> m_data;
    RefPtr<AnimatedProperties> m_animated_properties;
    bool m_depends_on_viewport_metrics { false };
    bool m_font_metrics_depend_on_viewport_metrics { false };
    bool m_in_display_none_subtree { false };

    mutable RefPtr<Gfx::FontCascadeList const> m_cached_computed_font_list;
    mutable RefPtr<Gfx::Font const> m_cached_first_available_computed_font;
    void clear_computed_font_list_cache()
    {
        m_cached_computed_font_list = nullptr;
        m_cached_first_available_computed_font = nullptr;
    }
};

class AnimatedProperties final : public RefCounted<AnimatedProperties> {
public:
    using PropertyMap = HashMap<PropertyID, NonnullRefPtr<StyleValue const>>;

    AnimatedProperties();
    AnimatedProperties(AnimatedProperties const&);
    ~AnimatedProperties();

    u64 identity() const { return m_identity; }
    bool is_empty() const { return m_values.is_empty(); }
    PropertyMap const& values() const { return m_values; }

    bool has_property(PropertyID) const;
    bool is_property_inherited(PropertyID) const;
    bool is_property_result_of_transition(PropertyID) const;
    StyleValue const& property(PropertyID) const;

    void set_property(PropertyID, NonnullRefPtr<StyleValue const>, AnimatedPropertyResultOfTransition, ComputedProperties::Inherited);
    void remove_property(PropertyID);
    void reset_non_inherited_properties();

private:
    void set_property_inherited(PropertyID, ComputedProperties::Inherited);
    void set_property_result_of_transition(PropertyID, AnimatedPropertyResultOfTransition);

    u64 m_identity;
    AK::FixedBitmap<number_of_longhand_properties> m_has_property { false };
    AK::FixedBitmap<number_of_longhand_properties> m_property_inherited { false };
    AK::FixedBitmap<number_of_longhand_properties> m_property_result_of_transition { false };
    PropertyMap m_values;
};

}
