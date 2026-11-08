/*
 * Copyright (c) 2018-2026, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::CSS {

class AnimatedProperties;
class StyleSheetState;
class StyleComputer;

}

namespace Web::CSS {

// What the host computes an element's style update in on the main thread, as an animation or
// transition update or the document's own style does: the style record whose longhands it reads,
// what it settles beside them, plus the genuinely-C++ residue no record can carry - the sparse
// wrapper mint cache, the computed font list cache, and the AnimatedProperties overlay
// association.
class ComputedStyleWorkingSet final : public RefCounted<ComputedStyleWorkingSet> {
public:
    enum class WithAnimationsApplied {
        No,
        Yes,
    };

    static NonnullRefPtr<ComputedStyleWorkingSet> create_over_record(StyleRecordRef);
    static NonnullRefPtr<ComputedStyleWorkingSet> create_with_base_values_from(ComputedValues const&);
    static NonnullRefPtr<ComputedStyleWorkingSet> create_for_animation_update(void const* record, ComputedValuesFFI::AnimatedOverlay const*);
    ~ComputedStyleWorkingSet();

    void set_depends_on_viewport_metrics();
    void set_font_metrics_depend_on_viewport_metrics();

    void set_display_before_box_type_transformation(Display);

    bool has_effective_color_scheme() const { return metadata().effective_color_scheme >= 0; }

    RefPtr<AnimatedProperties const> animated_properties_snapshot() const;
    ComputedValuesFFI::AnimatedOverlay const* animated_overlay() const;
    bool has_animated_property(PropertyID property_id) const;
    bool depends_on_viewport_metrics() const { return metadata().dependency_flags & to_underlying(StyleRecordDependencyFlag::DependsOnViewportMetrics); }
    bool font_metrics_depend_on_viewport_metrics() const { return metadata().dependency_flags & to_underlying(StyleRecordDependencyFlag::FontMetricsDependOnViewportMetrics); }
    // Whether the element this style was computed for has computed display none, or is a descendant of one that does.
    bool in_display_none_subtree() const { return metadata().in_display_none_subtree; }
    ComputedValuesFFI::AnimatedOverlay* prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer>);
    ComputedValuesFFI::AnimatedOverlay* prepare_animated_overlay_for_rust_finalization(Badge<StyleComputer>);
    ComputedValuesFFI::AnimatedOverlay const* animated_overlay(Badge<StyleComputer>) const;
    void finish_animated_overlay_rust_mutation(Badge<StyleComputer>);
    // Hold every value of `overlay` as the working set's animated values, as the computation that sampled them did.
    void install_animated_overlay(Badge<StyleComputer>, ComputedValuesFFI::AnimatedOverlay const*);
    bool requires_animated_post_compute_adjustments() const;
    void set_animated_custom_property(Badge<StyleComputer>, Utf16FlyString name, NonnullRefPtr<StyleValue const> value);
    void clear_animated_properties(Badge<StyleComputer>);
    OrderedHashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> const& animated_custom_properties() const { return m_animated_custom_properties; }
    bool animated_custom_properties_trigger_restyle() const { return m_animated_custom_properties_trigger_restyle; }
    void set_animated_custom_properties_trigger_restyle(Badge<StyleComputer>, bool value) { m_animated_custom_properties_trigger_restyle = value; }
    StyleValue const& property(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    StyleValueFFI::StyleValueData const* effective_property_data(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;

    PreferredColorScheme color_scheme(PreferredColorScheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const;
    FontVariantEmoji font_variant_emoji() const;
    Display display_before_box_type_transformation() const;

    // The font list for text in the given tree scope, whose @font-feature-values font-variant-alternates reads.
    ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> computed_font_list(FontComputer const&, TreeScopeID) const;
    // The platform font and font facts the font group is built from. The font list it names stays cached here.
    ComputedValuesFFI::FfiFontGroupBuildInputs font_group_build_inputs(DOM::Document const&, TreeScopeID) const;
    ValueComparingNonnullRefPtr<Gfx::Font const> first_available_computed_font(FontComputer const&) const;

    [[nodiscard]] CSSPixels line_height(FontComputer const&) const;
    [[nodiscard]] CSSPixels font_size() const;
    [[nodiscard]] Optional<FontSizeAdjust> font_size_adjust() const;
    Vector<ComputedFontFamily> computed_font_families() const;
    FontOpticalSizing font_optical_sizing() const;

    // The style record whose longhands this working set reads.
    void const* record() const { return m_record.get(); }

private:
    explicit ComputedStyleWorkingSet(StyleRecordRef);

    AnimatedProperties const& animated_properties() const;
    AnimatedProperties& mutable_animated_properties();

    FontResolutionFeatureValues font_resolution_feature_values() const;
    int math_depth() const;
    double font_weight() const;
    Percentage font_width() const;
    int font_slope() const;
    double font_oblique_angle() const;
    u8 font_synthesis() const;
    ComputedValuesFFI::FfiComputedStyleMetadata& metadata() { return m_metadata; }
    ComputedValuesFFI::FfiComputedStyleMetadata const& metadata() const { return m_metadata; }
    void clear_computed_font_list_cache()
    {
        m_cached_computed_font_list = nullptr;
        m_cached_first_available_computed_font = nullptr;
    }

    // The source of truth for computed longhand values and their importance and inheritance
    // flags, with the recorded inheritance-dependent specified values.
    StyleRecordRef m_record;
    // What the host settles beside the record's values, starting from what the record's
    // computation settled.
    ComputedValuesFFI::FfiComputedStyleMetadata m_metadata;
    // The sparse per-longhand mint cache over the effective values: an entry holds the
    // wrapper property() minted on demand. Overlay values are never cached here; their
    // wrappers live on AnimatedProperties.
    mutable HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_minted_wrappers;
    RefPtr<AnimatedProperties> m_animated_properties;
    bool m_had_animated_post_compute_adjustment_property { false };
    OrderedHashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> m_animated_custom_properties;
    bool m_animated_custom_properties_trigger_restyle { true };

    mutable RefPtr<Gfx::FontCascadeList const> m_cached_computed_font_list;
    mutable TreeScopeID m_cached_computed_font_list_scope;
    mutable RefPtr<Gfx::Font const> m_cached_first_available_computed_font;
};

class AnimatedProperties final : public RefCounted<AnimatedProperties> {
public:
    struct InheritedOnly { };

    AnimatedProperties();
    AnimatedProperties(AnimatedProperties const&);
    explicit AnimatedProperties(ComputedValuesFFI::AnimatedOverlay const*);
    AnimatedProperties(ComputedValuesFFI::AnimatedOverlay const*, InheritedOnly);
    ~AnimatedProperties();

    u64 identity() const { return m_identity; }
    bool is_empty() const { return ComputedValuesFFI::rust_animated_overlay_is_empty(m_overlay); }
    ReadonlySpan<ComputedValuesFFI::FfiAnimatedOverlayEntry> entries() const;

    // The Rust overlay is the authoritative store. C++ wrappers are minted lazily and cached
    // only for native readers that ask for property().
    ComputedValuesFFI::AnimatedOverlay const* overlay() const { return m_overlay; }

    bool has_property(PropertyID property_id) const { return entry(property_id); }
    StyleValue const& property(PropertyID) const;

    void clear_wrapper_cache() { m_wrapper_cache.clear(); }

private:
    ComputedValuesFFI::FfiAnimatedOverlayEntry const* entry(PropertyID) const;

    u64 m_identity;
    ComputedValuesFFI::AnimatedOverlay* m_overlay { nullptr };
    mutable HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_wrapper_cache;
};

// Mints a C++ StyleValue wrapper for a record or table slot's value data, stamping it with the
// style sheet the winning declaration came from when the caller resolved one from the sheet
// sidecar. Counts toward the process-wide longhand wrapper mint statistic.
NonnullRefPtr<StyleValue const> wrap_computed_longhand_slot(void const* value_data);

// https://drafts.csswg.org/css-inline-3/#valdef-line-height-normal
[[nodiscard]] CSSPixels normal_line_height(Gfx::FontPixelMetrics const&);

// How many C++ longhand wrappers have been minted process-wide, counting the on-demand mints
// property() performs and the specified-value wrappers the drive's side effects still need.
// Exposed through internals for the laziness measurements.
u64 longhand_wrappers_minted();
void reset_longhand_wrappers_minted();
void count_longhand_wrapper_mint();

}
