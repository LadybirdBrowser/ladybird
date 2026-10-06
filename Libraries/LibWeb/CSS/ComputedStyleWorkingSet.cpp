/*
 * Copyright (c) 2018-2026, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NeverDestroyed.h>
#include <AK/TypeCasts.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontResolution.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWebCommon/CSS/SystemColor.h>

namespace Web::CSS {

static Atomic<u64> s_next_animated_properties_identity { 1 };
static u64 s_longhand_wrappers_minted { 0 };

AnimatedProperties::~AnimatedProperties()
{
    ComputedValuesFFI::rust_animated_overlay_free(m_overlay);
}

static_assert(to_underlying(PseudoElement::KnownPseudoElementCount) <= sizeof(u64) * 8);

ComputedStyleWorkingSet::ComputedStyleWorkingSet()
    : m_computed_longhand_table(ComputedValuesFFI::rust_computed_longhand_table_create())
    , m_mint_cache(adopt_ref(*new WrapperMintCache))
{
}

ComputedStyleWorkingSet::ComputedStyleWorkingSet(ComputedValuesFFI::ComputedLonghandTable* longhand_table)
    : m_computed_longhand_table(longhand_table)
    , m_mint_cache(adopt_ref(*new WrapperMintCache))
{
}

ComputedStyleWorkingSet::ComputedStyleWorkingSet(ShareFrozenTable, ComputedStyleWorkingSet const& other)
    : m_computed_longhand_table(const_cast<ComputedValuesFFI::ComputedLonghandTable*>(ComputedValuesFFI::rust_computed_longhand_table_retain(other.m_computed_longhand_table)))
    , m_computed_longhand_table_is_shared(true)
    , m_mint_cache(other.m_mint_cache)
{
}

ComputedStyleWorkingSet::~ComputedStyleWorkingSet()
{
    ComputedValuesFFI::rust_computed_longhand_table_release(m_computed_longhand_table);
}

NonnullRefPtr<ComputedStyleWorkingSet> ComputedStyleWorkingSet::create()
{
    return adopt_ref(*new ComputedStyleWorkingSet);
}

NonnullRefPtr<ComputedStyleWorkingSet> ComputedStyleWorkingSet::create_with_longhand_table(ComputedValuesFFI::ComputedLonghandTable* longhand_table)
{
    VERIFY(longhand_table);
    return adopt_ref(*new ComputedStyleWorkingSet(longhand_table));
}

NonnullRefPtr<ComputedStyleWorkingSet> ComputedStyleWorkingSet::create_with_base_values_from(ComputedValues const& style)
{
    auto working_set = create();
    auto const& base = style.base_values();
    // Seed the table with the previous drive's computed values so unevaluated longhands read
    // and publish without reverse-materialization; the funnel overwrites what this drive
    // evaluates. A borrowed record view seeds from its interned value span. The base always
    // carries a table: property() has no other source for an unevaluated longhand's value.
    if (auto const* table = base.computed_longhand_table()) {
        ComputedValuesFFI::rust_computed_longhand_table_copy_from(working_set->m_computed_longhand_table, static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(table));
    } else {
        auto longhand_values = base.computed_longhand_values();
        VERIFY(!longhand_values.is_empty());
        ComputedValuesFFI::rust_computed_longhand_table_copy_from_values(working_set->m_computed_longhand_table, longhand_values.data(), longhand_values.size());
    }
    // A fresh drive starts with nothing evaluated and re-records its own inheritance-dependent
    // specified values; the flag bitmaps seed from the base style's published bitmaps, which a
    // borrowed record view carries even though it owns no table.
    ComputedValuesFFI::rust_computed_longhand_table_clear_seeded_state(working_set->m_computed_longhand_table);
    auto importance = base.property_importance_bitmap();
    auto inheritance = base.property_inheritance_bitmap();
    ComputedValuesFFI::rust_computed_longhand_table_load_flag_bitmaps(working_set->m_computed_longhand_table, importance.data(), importance.size(), inheritance.data(), inheritance.size());
    for (auto const& entry : base.inheritance_dependent_specified_values())
        ComputedValuesFFI::rust_computed_longhand_table_add_inheritance_dependent_value(working_set->m_computed_longhand_table, entry.property, entry.value);
    working_set->set_display_before_box_type_transformation(base.display_before_box_type_transformation());
    working_set->set_has_pseudo_element_styles(base.pseudo_element_style_mask());
    if (base.depends_on_viewport_metrics())
        working_set->set_depends_on_viewport_metrics();
    if (base.font_metrics_depend_on_viewport_metrics())
        working_set->set_font_metrics_depend_on_viewport_metrics();
    if (base.in_display_none_subtree())
        working_set->metadata().in_display_none_subtree = true;
    return working_set;
}

NonnullRefPtr<ComputedStyleWorkingSet> ComputedStyleWorkingSet::create_for_animation_update(ComputedValuesFFI::ComputedLonghandTable const* table, ComputedValuesFFI::AnimatedOverlay const* overlay)
{
    VERIFY(table);
    auto* retained_table = const_cast<ComputedValuesFFI::ComputedLonghandTable*>(ComputedValuesFFI::rust_computed_longhand_table_retain(table));
    auto working_set = create_with_longhand_table(retained_table);
    working_set->m_computed_longhand_table_is_shared = true;
    if (overlay) {
        working_set->m_had_animated_post_compute_adjustment_property = ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::Display))
            || ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::Position))
            || ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::Float))
            || ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::LineHeight))
            || ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::OverflowX))
            || ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::OverflowY))
            || ComputedValuesFFI::rust_animated_overlay_contains(overlay, to_underlying(PropertyID::TextAlign));
        auto animated_properties = adopt_ref(*new AnimatedProperties(overlay, AnimatedProperties::InheritedOnly {}));
        working_set->m_animated_properties = move(animated_properties);
    }
    return working_set;
}

void ComputedStyleWorkingSet::ensure_mutable_computed_longhand_table()
{
    if (!m_computed_longhand_table_is_shared)
        return;
    auto* table = ComputedValuesFFI::rust_computed_longhand_table_create();
    ComputedValuesFFI::rust_computed_longhand_table_copy_from(table, m_computed_longhand_table);
    ComputedValuesFFI::rust_computed_longhand_table_release(m_computed_longhand_table);
    m_computed_longhand_table = table;
    m_computed_longhand_table_is_shared = false;
}

void ComputedStyleWorkingSet::freeze_computed_longhand_table()
{
    ComputedValuesFFI::rust_computed_longhand_table_freeze(m_computed_longhand_table);
}

NonnullRefPtr<ComputedStyleWorkingSet> ComputedStyleWorkingSet::copy_without_animations() const
{
    return adopt_ref(*new ComputedStyleWorkingSet(ShareFrozenTable {}, *this));
}

AnimatedProperties::AnimatedProperties()
    : m_identity(s_next_animated_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_overlay(ComputedValuesFFI::rust_animated_overlay_create())
{
}

AnimatedProperties::AnimatedProperties(AnimatedProperties const& other)
    : m_identity(s_next_animated_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_overlay(ComputedValuesFFI::rust_animated_overlay_clone(other.m_overlay))
    , m_wrapper_cache(other.m_wrapper_cache)
{
}

AnimatedProperties::AnimatedProperties(ComputedValuesFFI::AnimatedOverlay const* overlay)
    : m_identity(s_next_animated_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_overlay(ComputedValuesFFI::rust_animated_overlay_clone(overlay))
{
}

AnimatedProperties::AnimatedProperties(ComputedValuesFFI::AnimatedOverlay const* overlay, InheritedOnly)
    : m_identity(s_next_animated_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_overlay(ComputedValuesFFI::rust_animated_overlay_clone_inherited(overlay))
{
}

u64 longhand_wrappers_minted()
{
    return s_longhand_wrappers_minted;
}

void reset_longhand_wrappers_minted()
{
    s_longhand_wrappers_minted = 0;
}

void count_longhand_wrapper_mint()
{
    ++s_longhand_wrappers_minted;
}

AnimatedProperties const& ComputedStyleWorkingSet::animated_properties() const
{
    static NeverDestroyed<AnimatedProperties> empty_animated_properties;
    if (!m_animated_properties)
        return *empty_animated_properties;
    return *m_animated_properties;
}

AnimatedProperties& ComputedStyleWorkingSet::mutable_animated_properties()
{
    if (!m_animated_properties)
        m_animated_properties = adopt_ref(*new AnimatedProperties);
    if (m_animated_properties->ref_count() > 1)
        m_animated_properties = adopt_ref(*new AnimatedProperties(*m_animated_properties));
    return *m_animated_properties;
}

ReadonlySpan<ComputedValuesFFI::FfiAnimatedOverlayEntry> AnimatedProperties::entries() const
{
    size_t count = 0;
    auto const* entries = ComputedValuesFFI::rust_animated_overlay_entries(m_overlay, &count);
    return { entries, count };
}

ComputedValuesFFI::FfiAnimatedOverlayEntry const* AnimatedProperties::entry(PropertyID property_id) const
{
    for (auto const& entry : entries()) {
        if (entry.property == to_underlying(property_id))
            return &entry;
    }
    return nullptr;
}

StyleValue const& AnimatedProperties::property(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    auto const* animated_entry = entry(property_id);
    VERIFY(animated_entry);

    if (auto wrapper = m_wrapper_cache.get(property_id); wrapper.has_value())
        return *wrapper.value();
    auto wrapper = wrap_computed_longhand_slot(animated_entry->value);
    m_wrapper_cache.set(property_id, wrapper);
    return *wrapper;
}

void AnimatedProperties::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, ComputedStyleWorkingSet::Inherited inherited)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    ComputedValuesFFI::rust_animated_overlay_set(m_overlay, to_underlying(id), value->rust_style_value_data(),
        inherited == ComputedStyleWorkingSet::Inherited::Yes,
        animated_property_result_of_transition == AnimatedPropertyResultOfTransition::Yes);
    m_wrapper_cache.set(id, move(value));
}

ReadonlyBytes ComputedStyleWorkingSet::property_importance_bitmap() const
{
    return { ComputedValuesFFI::rust_computed_longhand_table_importance_bits(m_computed_longhand_table), (number_of_longhand_properties + 7) / 8 };
}

ReadonlyBytes ComputedStyleWorkingSet::property_inheritance_bitmap() const
{
    return { ComputedValuesFFI::rust_computed_longhand_table_inheritance_bits(m_computed_longhand_table), (number_of_longhand_properties + 7) / 8 };
}

RefPtr<AnimatedProperties const> ComputedStyleWorkingSet::animated_properties_snapshot() const
{
    return m_animated_properties;
}

ComputedValuesFFI::AnimatedOverlay const* ComputedStyleWorkingSet::animated_overlay() const
{
    return m_animated_properties ? m_animated_properties->overlay() : nullptr;
}

bool ComputedStyleWorkingSet::has_animated_property(PropertyID property_id) const
{
    return animated_properties().has_property(property_id);
}

bool ComputedStyleWorkingSet::has_pseudo_element_style(PseudoElement pseudo_element) const
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    return metadata().pseudo_element_styles & (1ull << to_underlying(pseudo_element));
}

void ComputedStyleWorkingSet::set_has_pseudo_element_styles(u64 pseudo_element_styles)
{
    constexpr auto known_pseudo_element_count = to_underlying(PseudoElement::KnownPseudoElementCount);
    if constexpr (known_pseudo_element_count < sizeof(u64) * 8)
        VERIFY((pseudo_element_styles >> known_pseudo_element_count) == 0);
    metadata().pseudo_element_styles |= pseudo_element_styles;
}

void ComputedStyleWorkingSet::set_depends_on_viewport_metrics()
{
    metadata().dependency_flags |= to_underlying(StyleRecordDependencyFlag::DependsOnViewportMetrics);
}

void ComputedStyleWorkingSet::set_font_metrics_depend_on_viewport_metrics()
{
    metadata().dependency_flags |= to_underlying(StyleRecordDependencyFlag::FontMetricsDependOnViewportMetrics);
}

static bool property_affects_computed_font_list(PropertyID id)
{
    return first_is_one_of(id, PropertyID::FontFamily, PropertyID::FontSize, PropertyID::FontStyle, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::FontVariationSettings);
}

Display ComputedStyleWorkingSet::display_before_box_type_transformation() const
{
    return bit_cast<Display>(metadata().display_before_box_type_transformation);
}

void ComputedStyleWorkingSet::set_display_before_box_type_transformation(Display value)
{
    metadata().display_before_box_type_transformation = bit_cast<u32>(value);
}

void ComputedStyleWorkingSet::set_animated_property_internal(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    mutable_animated_properties().set_property(id, move(value), animated_property_result_of_transition, inherited);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedStyleWorkingSet::set_animated_property(Badge<StyleComputer>, PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    set_animated_property_internal(id, move(value), animated_property_result_of_transition, inherited);
}

ComputedValuesFFI::AnimatedOverlay* ComputedStyleWorkingSet::prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer>)
{
    auto& animated_properties = mutable_animated_properties();
    animated_properties.clear_wrapper_cache();
    clear_computed_font_list_cache();
    return const_cast<ComputedValuesFFI::AnimatedOverlay*>(animated_properties.overlay());
}

ComputedValuesFFI::AnimatedOverlay* ComputedStyleWorkingSet::prepare_animated_overlay_for_rust_finalization(Badge<StyleComputer>)
{
    auto& animated_properties = mutable_animated_properties();
    animated_properties.clear_wrapper_cache();
    return const_cast<ComputedValuesFFI::AnimatedOverlay*>(animated_properties.overlay());
}

ComputedValuesFFI::AnimatedOverlay const* ComputedStyleWorkingSet::animated_overlay(Badge<StyleComputer>) const
{
    return m_animated_properties ? m_animated_properties->overlay() : nullptr;
}

void ComputedStyleWorkingSet::finish_animated_overlay_rust_mutation(Badge<StyleComputer>)
{
    if (m_animated_properties && m_animated_properties->is_empty())
        m_animated_properties = nullptr;
}

bool ComputedStyleWorkingSet::requires_animated_post_compute_adjustments() const
{
    return m_had_animated_post_compute_adjustment_property
        || has_animated_property(PropertyID::Display)
        || has_animated_property(PropertyID::Position)
        || has_animated_property(PropertyID::Float)
        || has_animated_property(PropertyID::LineHeight)
        || has_animated_property(PropertyID::OverflowX)
        || has_animated_property(PropertyID::OverflowY)
        || has_animated_property(PropertyID::TextAlign);
}

void ComputedStyleWorkingSet::prepare_for_animated_post_compute_adjustments(Badge<StyleComputer>)
{
    ensure_mutable_computed_longhand_table();
}

void ComputedStyleWorkingSet::set_animated_custom_property(Badge<StyleComputer>, Utf16FlyString name, NonnullRefPtr<StyleValue const> value)
{
    m_animated_custom_properties.set(move(name), move(value));
}

void ComputedStyleWorkingSet::install_animated_overlay(Badge<StyleComputer>, ComputedValuesFFI::AnimatedOverlay const* overlay)
{
    m_animated_properties = adopt_ref(*new AnimatedProperties(overlay));
    clear_computed_font_list_cache();
}

void ComputedStyleWorkingSet::clear_animated_properties(Badge<StyleComputer>)
{
    m_animated_custom_properties.clear();
    if (!m_animated_properties)
        return;

    // NB: Ending this element's effects must preserve animated values inherited from its parent.
    m_animated_properties = adopt_ref(*new AnimatedProperties(m_animated_properties->overlay(), AnimatedProperties::InheritedOnly {}));
    if (m_animated_properties->is_empty())
        m_animated_properties = nullptr;
    clear_computed_font_list_cache();
}

NonnullRefPtr<StyleValue const> wrap_computed_longhand_slot(void const* value_data)
{
    ++s_longhand_wrappers_minted;
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(value_data)));
}

StyleValue const& ComputedStyleWorkingSet::property(PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    auto& cache = m_mint_cache->wrappers;
    // Without an animated overlay, a cached wrapper is always the effective value: the store
    // funnels replace or invalidate the entry on every table write, and the recorded specified
    // values invalidate it when they change.
    if (!m_animated_properties) {
        if (auto it = cache.find(property_id); it != cache.end())
            return *it->value;
    }
    auto effective = ComputedValuesFFI::rust_computed_longhand_table_effective_value(
        m_computed_longhand_table,
        m_animated_properties ? m_animated_properties->overlay() : nullptr,
        to_underlying(property_id),
        return_animated_value == WithAnimationsApplied::Yes);
    // The animated wrapper keeps its identity on AnimatedProperties and is never cached here.
    if (effective.source == ComputedValuesFFI::EFFECTIVE_LONGHAND_SOURCE_OVERLAY)
        return animated_properties().property(property_id);
    VERIFY(effective.value);
    if (auto it = cache.find(property_id); it != cache.end() && it->value->rust_style_value_data() == effective.value)
        return *it->value;
    // Mints the wrapper on demand, before any group fallback consumes the value.
    auto initial_value = property_initial_value(property_id);
    if (initial_value->rust_style_value_data() == effective.value) {
        cache.set(property_id, initial_value);
        return *initial_value;
    }
    auto wrapper = wrap_computed_longhand_slot(effective.value);
    cache.set(property_id, wrapper);
    return *wrapper;
}

void const* ComputedStyleWorkingSet::effective_property_data(PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    auto effective = ComputedValuesFFI::rust_computed_longhand_table_effective_value(
        m_computed_longhand_table,
        m_animated_properties ? m_animated_properties->overlay() : nullptr,
        to_underlying(property_id),
        return_animated_value == WithAnimationsApplied::Yes);
    VERIFY(effective.value);
    return effective.value;
}

Color ComputedStyleWorkingSet::color(PropertyID id, ColorResolutionContext color_resolution_context) const
{
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_storage;
    auto input = make_rust_color_resolution_input(color_resolution_context, length_storage);
    auto resolved = StyleValueFFI::rust_style_value_to_color(effective_property_data(id), &input);
    VERIFY(resolved.resolved);
    return Color(resolved.rgba[0], resolved.rgba[1], resolved.rgba[2], resolved.rgba[3]);
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
PreferredColorScheme ComputedStyleWorkingSet::color_scheme(PreferredColorScheme preferred_scheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const
{
    if (!has_animated_property(PropertyID::ColorScheme) && has_effective_color_scheme())
        return static_cast<PreferredColorScheme>(metadata().effective_color_scheme);

    Vector<u8> document_supported_scheme_codes;
    if (document_supported_schemes.has_value()) {
        document_supported_scheme_codes.ensure_capacity(document_supported_schemes->size());
        for (auto const& scheme : *document_supported_schemes)
            document_supported_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
    }
    ComputedValuesFFI::FfiEffectiveColorSchemeInput input {
        .preferred_color_scheme = static_cast<u8>(to_underlying(preferred_scheme)),
        .has_document_supported_schemes = document_supported_schemes.has_value(),
        .document_supported_scheme_codes = document_supported_scheme_codes.data(),
        .document_supported_scheme_count = document_supported_scheme_codes.size(),
    };
    return static_cast<PreferredColorScheme>(ComputedValuesFFI::rust_resolve_effective_color_scheme(effective_property_data(PropertyID::ColorScheme), &input));
}

CSSPixels normal_line_height(Gfx::FontPixelMetrics const& font_metrics)
{
    return CSSPixels { round_to<i32>(font_metrics.ascent) + round_to<i32>(font_metrics.descent) };
}

CSSPixels ComputedStyleWorkingSet::line_height(FontComputer const& font_computer) const
{
    // https://drafts.csswg.org/css-inline-3/#line-height-property
    auto const& line_height = property(PropertyID::LineHeight);

    // normal
    // Determine the preferred line height automatically based on font metrics.
    if (line_height.is_keyword() && line_height.to_keyword() == Keyword::Normal)
        return normal_line_height(first_available_computed_font(font_computer)->pixel_metrics());

    // <length [0,∞]>
    // The specified length is used as the preferred line height. Negative values are illegal.
    if (line_height.is_length())
        return line_height.as_length().length().absolute_length_to_px();

    // <number [0,∞]>
    // The preferred line height is this number multiplied by the element’s computed font-size.
    if (line_height.is_number())
        return CSSPixels { font_size() * line_height.as_number().number() };

    VERIFY_NOT_REACHED();
}

FontVariantEmoji ComputedStyleWorkingSet::font_variant_emoji() const
{
    auto const& value = property(PropertyID::FontVariantEmoji);
    return keyword_to_font_variant_emoji(value.to_keyword()).release_value();
}

// What a font resolution reads beside the family, as a style engine request names it: the values whose property does
// not have its initial value.
FontResolutionFeatureValues ComputedStyleWorkingSet::font_resolution_feature_values() const
{
    FontResolutionFeatureValues values;
    auto set = [&](FontResolutionFeatureInput input, PropertyID property_id, Keyword initial) {
        auto const& value = property(property_id);
        if (!value.is_keyword() || value.to_keyword() != initial)
            values[to_underlying(input)] = &value;
    };
    set(FontResolutionFeatureInput::FontFeatureSettings, PropertyID::FontFeatureSettings, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariationSettings, PropertyID::FontVariationSettings, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantCaps, PropertyID::FontVariantCaps, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantEastAsian, PropertyID::FontVariantEastAsian, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantEmoji, PropertyID::FontVariantEmoji, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantLigatures, PropertyID::FontVariantLigatures, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantNumeric, PropertyID::FontVariantNumeric, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantPosition, PropertyID::FontVariantPosition, Keyword::Normal);
    set(FontResolutionFeatureInput::FontVariantAlternates, PropertyID::FontVariantAlternates, Keyword::Normal);
    set(FontResolutionFeatureInput::FontKerning, PropertyID::FontKerning, Keyword::Auto);
    set(FontResolutionFeatureInput::TextRendering, PropertyID::TextRendering, Keyword::Auto);
    return values;
}

ComputedValuesFFI::FfiFontGroupBuildInputs ComputedStyleWorkingSet::font_group_build_inputs(DOM::Document const& document, TreeScopeID tree_scope) const
{
    // FIXME: A tree-scoped name is resolved in the tree of the declaration that named it, and inherits with that
    //        tree (css-scoping). This resolves feature value names in the element's own tree scope instead.
    auto font_list = computed_font_list(document.font_computer(), tree_scope);
    auto const& first_available_font = font_list->first_available_font();
    auto const metrics = first_available_font.pixel_metrics();
    auto math_shift = keyword_to_math_shift(property(PropertyID::MathShift).to_keyword()).release_value();
    auto math_style = keyword_to_math_style(property(PropertyID::MathStyle).to_keyword()).release_value();
    return {
        .font_size_raw = font_size().raw_value(),
        .line_height_used_raw = line_height(document.font_computer()).raw_value(),
        .font_variant_emoji = to_underlying(font_variant_emoji()),
        .font_ascent = metrics.ascent,
        .font_descent = metrics.descent,
        .font_x_height = metrics.x_height,
        .font_zero_advance = metrics.advance_of_ascii_zero,
        .first_available_font = &first_available_font,
        .font_cascade_list = font_list.ptr(),
        .font_weight = font_weight(),
        .font_width = font_width().value(),
        .math_shift = to_underlying(math_shift),
        .math_style = to_underlying(math_style),
        .math_depth = math_depth(),
    };
}

ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> ComputedStyleWorkingSet::computed_font_list(FontComputer const& font_computer, TreeScopeID tree_scope) const
{
    if (!m_cached_computed_font_list || m_cached_computed_font_list_scope != tree_scope) {
        m_cached_computed_font_list = resolve_font_for_style_values(font_computer,
            {
                .font_families = computed_font_families(),
                .font_optical_sizing = font_optical_sizing(),
                .font_size = font_size(),
                .font_slope = font_slope(),
                .font_weight = font_weight(),
                .font_width = font_width(),
                .feature_values = font_resolution_feature_values(),
                .font_feature_values_scope = tree_scope,
            });
        m_cached_computed_font_list_scope = tree_scope;
        VERIFY(!m_cached_computed_font_list->is_empty());
    }

    return *m_cached_computed_font_list;
}

ValueComparingNonnullRefPtr<Gfx::Font const> ComputedStyleWorkingSet::first_available_computed_font(FontComputer const& font_computer) const
{
    // NB: Feature values only change how a font shapes text, not which font is first available, so the list
    //     resolved for any tree scope answers.
    if (!m_cached_first_available_computed_font)
        m_cached_first_available_computed_font = computed_font_list(font_computer, m_cached_computed_font_list_scope)->first_available_font();
    return *m_cached_first_available_computed_font;
}

int ComputedStyleWorkingSet::math_depth() const
{
    return property(PropertyID::MathDepth).as_integer().integer();
}

CSSPixels ComputedStyleWorkingSet::font_size() const
{
    return CSSPixels { StyleValueFFI::rust_style_value_computed_length_value(effective_property_data(PropertyID::FontSize)) };
}

Vector<ComputedFontFamily> ComputedStyleWorkingSet::computed_font_families() const
{
    auto const* data = effective_property_data(PropertyID::FontFamily);
    auto count = StyleValueFFI::rust_style_value_copy_computed_font_families(data, nullptr, 0);
    Vector<StyleValueFFI::FfiComputedFontFamilyEntry> entries;
    entries.resize(count);
    VERIFY(StyleValueFFI::rust_style_value_copy_computed_font_families(data, entries.data(), entries.size()) == count);

    Vector<ComputedFontFamily> families;
    families.ensure_capacity(count);
    for (auto const& entry : entries) {
        if (entry.kind == StyleValueFFI::COMPUTED_FONT_FAMILY_GENERIC) {
            auto family = keyword_to_generic_font_family(static_cast<Keyword>(entry.keyword));
            VERIFY(family.has_value());
            families.unchecked_append(family.release_value());
            continue;
        }
        VERIFY(entry.kind == StyleValueFFI::COMPUTED_FONT_FAMILY_CUSTOM_IDENT || entry.kind == StyleValueFFI::COMPUTED_FONT_FAMILY_STRING);
        families.unchecked_append(ComputedFontFamilyName {
            .name = css_string_from_rust(entry.string),
            .syntax = entry.kind == StyleValueFFI::COMPUTED_FONT_FAMILY_STRING
                ? ComputedFontFamilySyntax::String
                : ComputedFontFamilySyntax::CustomIdent,
        });
    }
    return families;
}

double ComputedStyleWorkingSet::font_weight() const
{
    return StyleValueFFI::rust_style_value_computed_number(effective_property_data(PropertyID::FontWeight));
}

Percentage ComputedStyleWorkingSet::font_width() const
{
    return Percentage { StyleValueFFI::rust_style_value_computed_percentage(effective_property_data(PropertyID::FontWidth)) };
}

int ComputedStyleWorkingSet::font_slope() const
{
    return property(PropertyID::FontStyle).as_font_style().to_font_slope();
}

FontOpticalSizing ComputedStyleWorkingSet::font_optical_sizing() const
{
    auto const& value = property(PropertyID::FontOpticalSizing);
    return keyword_to_font_optical_sizing(value.to_keyword()).release_value();
}

}
