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
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::CSS {

class AnimatedProperties;
class CSSStyleSheet;
class StyleComputer;

}

namespace Web::CSS {

enum class AnimatedPropertyResultOfTransition : u8 {
    No,
    Yes
};

// The style computation drive's working state: the Rust computed longhand table the drive
// stores into, plus the genuinely-C++ residue no record can carry - the GC style sheet
// sidecar for on-demand wrapper stamping, the sparse wrapper mint cache, the computed font
// list cache, and the AnimatedProperties overlay association. Post-drive readers consume the
// published ComputedValues / style record instead; this object only survives the drive for
// the animation refresh paths that re-sample the overlay against the frozen table.
class ComputedStyleWorkingSet final : public RefCounted<ComputedStyleWorkingSet> {
public:
    enum class WithAnimationsApplied {
        No,
        Yes,
    };

    enum class Inherited {
        No,
        Yes
    };

    enum class CreateAnimatedOverlay {
        No,
        Yes,
    };

    static NonnullRefPtr<ComputedStyleWorkingSet> create();
    static NonnullRefPtr<ComputedStyleWorkingSet> create_with_longhand_table(ComputedValuesFFI::ComputedLonghandTable*);
    static NonnullRefPtr<ComputedStyleWorkingSet> create_with_base_values_from(ComputedStyleWorkingSet const&);
    static NonnullRefPtr<ComputedStyleWorkingSet> create_with_base_values_from(ComputedValues const&);
    ~ComputedStyleWorkingSet();

    // Freezes the computed longhand table once the drive has stored every longhand. The
    // animated overlay and the dependency flags stay mutable for the refresh paths.
    void freeze_computed_longhand_table();

    // A working set sharing this one's frozen table and mint cache, without the animated
    // overlay: the base half of an animated style build.
    NonnullRefPtr<ComputedStyleWorkingSet> copy_without_animations() const;

    void set_has_pseudo_element_styles(u64);
    void set_property_important(PropertyID, Important);
    void set_property_inherited(PropertyID, Inherited);
    void set_depends_on_viewport_metrics();
    void set_font_metrics_depend_on_viewport_metrics();
    void set_in_display_none_subtree();

    void set_property(PropertyID, NonnullRefPtr<StyleValue const> value, Inherited = Inherited::No, Important = Important::No);
    // The wrapper-carrying store funnel: dual-writes the Rust table and the wrapper cache.
    // `style_sheet_source_slot` is the winning declaration's cascade source slot when its
    // value carries style sheet context, and -1 otherwise.
    void set_property_without_modifying_flags(PropertyID, NonnullRefPtr<StyleValue const> value, i64 style_sheet_source_slot = -1);
    // Invalidates C++ sidecars after the Rust driver stores a value directly in the table.
    void did_store_property_data_from_drive(PropertyID);
    void set_style_sheet_for_source_slot(u32, GC::Ptr<CSSStyleSheet>);
    void cache_property_wrapper_from_drive(PropertyID, NonnullRefPtr<StyleValue const>);
    void set_display_before_box_type_transformation(Display);

    bool has_effective_color_scheme() const { return metadata().effective_color_scheme >= 0; }
    void set_effective_color_scheme(PreferredColorScheme color_scheme) { metadata().effective_color_scheme = to_underlying(color_scheme); }
    void clear_effective_color_scheme() { metadata().effective_color_scheme = -1; }

    void add_inheritance_dependent_specified_value(PropertyID, NonnullRefPtr<StyleValue const> value);
    void remove_inheritance_dependent_specified_value(PropertyID);

    RefPtr<AnimatedProperties const> animated_properties_snapshot() const;
    bool has_animated_property(PropertyID property_id) const;
    void reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>);

    bool is_property_important(PropertyID property_id) const;
    bool is_property_inherited(PropertyID property_id) const;
    bool is_animated_property_inherited(PropertyID property_id) const;
    bool is_animated_property_result_of_transition(PropertyID property_id) const;
    bool depends_on_viewport_metrics() const { return metadata().dependency_flags & 1; }
    bool font_metrics_depend_on_viewport_metrics() const { return metadata().dependency_flags & 2; }
    // Whether the element this style was computed for has computed display none, or is a descendant of one that does.
    bool in_display_none_subtree() const { return metadata().in_display_none_subtree; }
    bool has_pseudo_element_style(PseudoElement) const;
    void set_animated_property(Badge<StyleComputer>, PropertyID, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition, Inherited = Inherited::No);
    ComputedValuesFFI::AnimatedOverlay* prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer>);
    ComputedValuesFFI::AnimatedOverlay* prepare_animated_overlay_for_rust_finalization(Badge<StyleComputer>, CreateAnimatedOverlay);
    ComputedValuesFFI::AnimatedOverlay const* animated_overlay(Badge<StyleComputer>) const;
    void finish_animated_overlay_rust_mutation(Badge<StyleComputer>);
    void did_apply_style_finalization_from_rust(u16 invalidated_longhands);
    void clear_animated_properties(Badge<StyleComputer>);
    StyleValue const& property(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    void const* effective_property_data(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;

    Color color(PropertyID, ColorResolutionContext) const;
    PreferredColorScheme color_scheme(PreferredColorScheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const;
    TextRendering text_rendering() const;
    CSSPixels text_underline_offset() const;
    CSSPixels border_spacing_horizontal() const;
    CSSPixels border_spacing_vertical() const;
    CaptionSide caption_side() const;
    Display display() const;
    Color caret_color(ColorResolutionContext const&) const;
    ContentVisibility content_visibility() const;
    CSSPixels word_spacing() const;
    CSSPixels letter_spacing() const;
    ListStyleType list_style_type(StyleScope const&) const;
    Color accent_color(ColorResolutionContext const&) const;
    Visibility visibility() const;
    ImageRendering image_rendering() const;
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
    BorderCollapse border_collapse() const;
    CSS::EmptyCells empty_cells() const;
    Direction direction() const;
    WritingMode writing_mode() const;
    Display display_before_box_type_transformation() const;

    float stop_opacity() const;
    float flood_opacity() const;

    ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> computed_font_list(FontComputer const&) const;
    ValueComparingNonnullRefPtr<Gfx::Font const> first_available_computed_font(FontComputer const&) const;

    int math_depth() const;
    [[nodiscard]] CSSPixels line_height(FontComputer const&) const;
    [[nodiscard]] LineHeightData line_height_data() const;
    [[nodiscard]] CSSPixels font_size() const;
    Vector<ComputedFontFamily> computed_font_families() const;
    double font_weight() const;
    Percentage font_width() const;
    int font_slope() const;
    FontOpticalSizing font_optical_sizing() const;

    ScrollbarColorData scrollbar_color(ColorResolutionContext const&) const;

    // The recorded inheritance-dependent specified values, borrowed from the drive's table;
    // the span stays valid while the table does.
    ReadonlySpan<ComputedValuesFFI::FfiTableInheritanceDependentValue const> inheritance_dependent_value_span() const;

    // Whole-bitmap views over the table's importance and inheritance flags, in FixedBitmap
    // byte layout; valid while the table is.
    ReadonlyBytes property_importance_bitmap() const;
    ReadonlyBytes property_inheritance_bitmap() const;

    // The drive's computed longhand table, for transfer onto the ComputedValues built from
    // this working set.
    ComputedValuesFFI::ComputedLonghandTable* mutable_computed_longhand_table() { return m_computed_longhand_table; }
    ComputedValuesFFI::ComputedLonghandTable const* computed_longhand_table() const { return m_computed_longhand_table; }

private:
    // The sparse per-longhand mint cache over the effective values: an entry holds the
    // wrapper a store funnel carried or the one property() minted on demand, and is replaced
    // or invalidated when the drive stores new table data for the longhand. Overlay values
    // are never cached here; their wrappers live on AnimatedProperties. Shared with the
    // without-animations copy so both halves of an animated style build mint each wrapper
    // once, preserving wrapper identity for values with side effects (image loads).
    struct WrapperMintCache final : public RefCounted<WrapperMintCache> {
        HashMap<PropertyID, NonnullRefPtr<StyleValue const>> wrappers;
        // Style sheets are indexed by the source slots stored in the Rust longhand table.
        // Held weakly, like the cascade's own declaration sources.
        Vector<GC::Weak<CSSStyleSheet>> style_sheet_source_slots;
    };

    ComputedStyleWorkingSet();
    explicit ComputedStyleWorkingSet(ComputedValuesFFI::ComputedLonghandTable*);
    // The without-animations copy: shares the frozen table and the mint cache.
    struct ShareFrozenTable { };
    ComputedStyleWorkingSet(ShareFrozenTable, ComputedStyleWorkingSet const&);

    AnimatedProperties const& animated_properties() const;
    AnimatedProperties& mutable_animated_properties();
    ComputedValuesFFI::FfiComputedStyleMetadata& metadata() { return *ComputedValuesFFI::rust_computed_longhand_table_metadata(m_computed_longhand_table); }
    ComputedValuesFFI::FfiComputedStyleMetadata const& metadata() const { return *ComputedValuesFFI::rust_computed_longhand_table_metadata(m_computed_longhand_table); }
    void set_animated_property_internal(PropertyID, NonnullRefPtr<StyleValue const>, AnimatedPropertyResultOfTransition, Inherited);
    void clear_computed_font_list_cache()
    {
        m_cached_computed_font_list = nullptr;
        m_cached_first_available_computed_font = nullptr;
    }

    // The source of truth for computed longhand values and their importance, inheritance
    // and evaluation flags plus the recorded inheritance-dependent specified values,
    // written by the store funnels and the flag setters and frozen when the drive completes.
    ComputedValuesFFI::ComputedLonghandTable* m_computed_longhand_table { nullptr };
    NonnullRefPtr<WrapperMintCache> m_mint_cache;
    RefPtr<AnimatedProperties> m_animated_properties;

    mutable RefPtr<Gfx::FontCascadeList const> m_cached_computed_font_list;
    mutable RefPtr<Gfx::Font const> m_cached_first_available_computed_font;
};

class AnimatedProperties final : public RefCounted<AnimatedProperties> {
public:
    AnimatedProperties();
    AnimatedProperties(AnimatedProperties const&);
    explicit AnimatedProperties(ComputedValuesFFI::AnimatedOverlay const*);
    ~AnimatedProperties();

    u64 identity() const { return m_identity; }
    bool is_empty() const { return entries().is_empty(); }
    ReadonlySpan<ComputedValuesFFI::FfiAnimatedOverlayEntry> entries() const;

    // The Rust overlay is the authoritative store. C++ wrappers are minted lazily and cached
    // only for native readers that ask for property().
    ComputedValuesFFI::AnimatedOverlay const* overlay() const { return m_overlay; }

    bool has_property(PropertyID property_id) const { return entry(property_id); }
    bool is_property_inherited(PropertyID property_id) const
    {
        auto const* animated_entry = entry(property_id);
        return animated_entry && animated_entry->inherited;
    }
    bool is_property_result_of_transition(PropertyID property_id) const
    {
        auto const* animated_entry = entry(property_id);
        return animated_entry && animated_entry->result_of_transition;
    }
    StyleValue const& property(PropertyID) const;

    void set_property(PropertyID, NonnullRefPtr<StyleValue const>, AnimatedPropertyResultOfTransition, ComputedStyleWorkingSet::Inherited);
    void reset_non_inherited_properties();
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
NonnullRefPtr<StyleValue const> wrap_computed_longhand_slot(void const* value_data, GC::Ptr<CSSStyleSheet> style_sheet);

// https://drafts.csswg.org/css-inline-3/#valdef-line-height-normal
[[nodiscard]] CSSPixels normal_line_height(Gfx::FontPixelMetrics const&);

// How many C++ longhand wrappers have been minted process-wide, counting the on-demand mints
// property() performs and the specified-value wrappers the drive's side effects still need.
// Exposed through internals for the laziness measurements.
u64 longhand_wrappers_minted();
void reset_longhand_wrappers_minted();
void count_longhand_wrapper_mint();

}
