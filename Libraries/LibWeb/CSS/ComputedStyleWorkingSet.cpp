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
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/FontComputer.h>
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
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

extern "C" void ladybird_animated_properties_ref(void const*);
extern "C" void ladybird_animated_properties_unref(void const*);
extern "C" void style_engine_recording_pointer_will_die(void const*);

static Atomic<u64> s_next_animated_properties_identity { 1 };
static u64 s_longhand_wrappers_minted { 0 };

AnimatedProperties::~AnimatedProperties()
{
    style_engine_recording_pointer_will_die(this);
    ComputedValuesFFI::rust_animated_overlay_free(m_overlay);
}

extern "C" void ladybird_animated_properties_ref(void const* values)
{
    static_cast<AnimatedProperties const*>(values)->ref();
}

extern "C" void ladybird_animated_properties_unref(void const* values)
{
    static_cast<AnimatedProperties const*>(values)->unref();
}

static_assert(to_underlying(PseudoElement::KnownPseudoElementCount) <= sizeof(u64) * 8);

ComputedStyleWorkingSet::ComputedStyleWorkingSet()
    : m_computed_longhand_table(ComputedValuesFFI::rust_computed_longhand_table_create())
    , m_mint_cache(adopt_ref(*new WrapperMintCache))
{
}

ComputedStyleWorkingSet::ComputedStyleWorkingSet(ShareFrozenTable, ComputedStyleWorkingSet const& other)
    : m_computed_longhand_table(const_cast<ComputedValuesFFI::ComputedLonghandTable*>(ComputedValuesFFI::rust_computed_longhand_table_retain(other.m_computed_longhand_table)))
    , m_mint_cache(other.m_mint_cache)
    , m_display_before_box_type_transformation(other.m_display_before_box_type_transformation)
    , m_pseudo_element_styles(other.m_pseudo_element_styles)
    , m_effective_color_scheme(other.m_effective_color_scheme)
    , m_raw_cascaded_font_size(other.m_raw_cascaded_font_size)
    , m_depends_on_viewport_metrics(other.m_depends_on_viewport_metrics)
    , m_font_metrics_depend_on_viewport_metrics(other.m_font_metrics_depend_on_viewport_metrics)
    , m_in_display_none_subtree(other.m_in_display_none_subtree)
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

NonnullRefPtr<ComputedStyleWorkingSet> ComputedStyleWorkingSet::create_with_base_values_from(ComputedStyleWorkingSet const& style)
{
    auto working_set = create();
    working_set->m_mint_cache->wrappers = style.m_mint_cache->wrappers;
    working_set->m_mint_cache->style_sheet_sources = style.m_mint_cache->style_sheet_sources;
    // The table copy carries the importance, inheritance and evaluation flags and the recorded
    // inheritance-dependent specified values along with the value slots.
    ComputedValuesFFI::rust_computed_longhand_table_copy_from(working_set->m_computed_longhand_table, style.m_computed_longhand_table);
    working_set->m_display_before_box_type_transformation = style.m_display_before_box_type_transformation;
    working_set->m_pseudo_element_styles = style.m_pseudo_element_styles;
    working_set->m_effective_color_scheme = style.m_effective_color_scheme;
    working_set->m_raw_cascaded_font_size = style.m_raw_cascaded_font_size;
    working_set->m_depends_on_viewport_metrics = style.m_depends_on_viewport_metrics;
    working_set->m_font_metrics_depend_on_viewport_metrics = style.m_font_metrics_depend_on_viewport_metrics;
    working_set->m_in_display_none_subtree = style.m_in_display_none_subtree;
    if (style.m_animated_properties)
        working_set->m_animated_properties = adopt_ref(*new AnimatedProperties(*style.m_animated_properties));
    return working_set;
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
    for (auto const& [property_id, value] : base.inheritance_dependent_specified_values())
        ComputedValuesFFI::rust_computed_longhand_table_add_inheritance_dependent_value(working_set->m_computed_longhand_table, to_underlying(property_id), value->rust_style_value_data());
    for (auto const& entry : base.borrowed_inheritance_dependent_values())
        ComputedValuesFFI::rust_computed_longhand_table_add_inheritance_dependent_value(working_set->m_computed_longhand_table, entry.property, entry.value);
    working_set->m_display_before_box_type_transformation = base.display_before_box_type_transformation();
    working_set->m_pseudo_element_styles = base.pseudo_element_style_mask();
    working_set->m_raw_cascaded_font_size = base.raw_cascaded_font_size();
    working_set->m_depends_on_viewport_metrics = base.depends_on_viewport_metrics();
    working_set->m_font_metrics_depend_on_viewport_metrics = base.font_metrics_depend_on_viewport_metrics();
    working_set->m_in_display_none_subtree = base.in_display_none_subtree();
    return working_set;
}

void ComputedStyleWorkingSet::freeze_computed_longhand_table()
{
    if (m_depends_on_viewport_metrics)
        ComputedValuesFFI::rust_computed_longhand_table_set_dependency_flag(m_computed_longhand_table, 0);
    if (m_font_metrics_depend_on_viewport_metrics)
        ComputedValuesFFI::rust_computed_longhand_table_set_dependency_flag(m_computed_longhand_table, 1);
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
    auto wrapper = wrap_computed_longhand_slot(animated_entry->value, nullptr);
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

void AnimatedProperties::reset_non_inherited_properties()
{
    ComputedValuesFFI::rust_animated_overlay_reset_non_inherited(m_overlay);
    m_wrapper_cache.clear();
}

bool ComputedStyleWorkingSet::is_property_important(PropertyID property_id) const
{
    return ComputedValuesFFI::rust_computed_longhand_table_is_important(m_computed_longhand_table, to_underlying(property_id));
}

void ComputedStyleWorkingSet::set_property_important(PropertyID property_id, Important important)
{
    ComputedValuesFFI::rust_computed_longhand_table_set_important(m_computed_longhand_table, to_underlying(property_id), important == Important::Yes);
}

bool ComputedStyleWorkingSet::is_property_inherited(PropertyID property_id) const
{
    return ComputedValuesFFI::rust_computed_longhand_table_is_inherited(m_computed_longhand_table, to_underlying(property_id));
}

ReadonlyBytes ComputedStyleWorkingSet::property_importance_bitmap() const
{
    return { ComputedValuesFFI::rust_computed_longhand_table_importance_bits(m_computed_longhand_table), (number_of_longhand_properties + 7) / 8 };
}

ReadonlyBytes ComputedStyleWorkingSet::property_inheritance_bitmap() const
{
    return { ComputedValuesFFI::rust_computed_longhand_table_inheritance_bits(m_computed_longhand_table), (number_of_longhand_properties + 7) / 8 };
}

ReadonlySpan<StyleEngineFFI::FfiInheritanceDependentValue const> ComputedStyleWorkingSet::inheritance_dependent_value_span() const
{
    size_t count = 0;
    auto const* entries = ComputedValuesFFI::rust_computed_longhand_table_inheritance_dependent_values(m_computed_longhand_table, &count);
    static_assert(sizeof(StyleEngineFFI::FfiInheritanceDependentValue) == sizeof(ComputedValuesFFI::FfiTableInheritanceDependentValue));
    static_assert(alignof(StyleEngineFFI::FfiInheritanceDependentValue) == alignof(ComputedValuesFFI::FfiTableInheritanceDependentValue));
    return { reinterpret_cast<StyleEngineFFI::FfiInheritanceDependentValue const*>(entries), count };
}

void ComputedStyleWorkingSet::add_inheritance_dependent_specified_value(PropertyID property_id, NonnullRefPtr<StyleValue const> value)
{
    ComputedValuesFFI::rust_computed_longhand_table_add_inheritance_dependent_value(m_computed_longhand_table, to_underlying(property_id), value->rust_style_value_data());
    // An unevaluated longhand's cached wrapper may predate the recorded specified value; the
    // next property() re-derives the effective value.
    m_mint_cache->wrappers.remove(property_id);
}

void ComputedStyleWorkingSet::remove_inheritance_dependent_specified_value(PropertyID property_id)
{
    ComputedValuesFFI::rust_computed_longhand_table_remove_inheritance_dependent_value(m_computed_longhand_table, to_underlying(property_id));
    m_mint_cache->wrappers.remove(property_id);
}

RefPtr<AnimatedProperties const> ComputedStyleWorkingSet::animated_properties_snapshot() const
{
    return m_animated_properties;
}

bool ComputedStyleWorkingSet::has_animated_property(PropertyID property_id) const
{
    return animated_properties().has_property(property_id);
}

bool ComputedStyleWorkingSet::is_animated_property_inherited(PropertyID property_id) const
{
    return animated_properties().is_property_inherited(property_id);
}

bool ComputedStyleWorkingSet::is_animated_property_result_of_transition(PropertyID property_id) const
{
    return animated_properties().is_property_result_of_transition(property_id);
}

bool ComputedStyleWorkingSet::has_pseudo_element_style(PseudoElement pseudo_element) const
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    return m_pseudo_element_styles & (1ull << to_underlying(pseudo_element));
}

void ComputedStyleWorkingSet::set_has_pseudo_element_styles(u64 pseudo_element_styles)
{
    constexpr auto known_pseudo_element_count = to_underlying(PseudoElement::KnownPseudoElementCount);
    if constexpr (known_pseudo_element_count < sizeof(u64) * 8)
        VERIFY((pseudo_element_styles >> known_pseudo_element_count) == 0);
    m_pseudo_element_styles |= pseudo_element_styles;
}

void ComputedStyleWorkingSet::set_property_inherited(PropertyID property_id, Inherited inherited)
{
    ComputedValuesFFI::rust_computed_longhand_table_set_inherited(m_computed_longhand_table, to_underlying(property_id), inherited == Inherited::Yes);
}

void ComputedStyleWorkingSet::set_depends_on_viewport_metrics()
{
    m_depends_on_viewport_metrics = true;
}

void ComputedStyleWorkingSet::set_font_metrics_depend_on_viewport_metrics()
{
    m_font_metrics_depend_on_viewport_metrics = true;
}

void ComputedStyleWorkingSet::set_in_display_none_subtree()
{
    m_in_display_none_subtree = true;
}

void ComputedStyleWorkingSet::clear_in_display_none_subtree()
{
    m_in_display_none_subtree = false;
}

void ComputedStyleWorkingSet::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, Inherited inherited, Important important)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    set_property_without_modifying_flags(id, move(value));
    set_property_important(id, important);
    set_property_inherited(id, inherited);
}

static bool property_affects_computed_font_list(PropertyID id)
{
    return first_is_one_of(id, PropertyID::FontFamily, PropertyID::FontSize, PropertyID::FontStyle, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::FontVariationSettings);
}

void ComputedStyleWorkingSet::set_property_without_modifying_flags(PropertyID id, NonnullRefPtr<StyleValue const> value, i64 style_sheet_source_slot)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    ComputedValuesFFI::rust_computed_longhand_table_set(m_computed_longhand_table, to_underlying(id), value->rust_style_value_data(), style_sheet_source_slot);
    m_mint_cache->wrappers.set(id, move(value));
    // The cached wrapper carries whatever sheet context its maker gave it; a mint from the
    // table must not stamp a stale source recorded by an earlier store.
    m_mint_cache->style_sheet_sources.remove(id);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedStyleWorkingSet::set_property_data_from_drive(PropertyID id, void const* value_data, i64 style_sheet_source_slot, GC::Ptr<CSSStyleSheet> style_sheet)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);
    VERIFY(value_data);

    ComputedValuesFFI::rust_computed_longhand_table_set(m_computed_longhand_table, to_underlying(id), value_data, style_sheet_source_slot);
    m_mint_cache->wrappers.remove(id);
    if (style_sheet)
        m_mint_cache->style_sheet_sources.set(id, GC::Weak<CSSStyleSheet> { *style_sheet });
    else
        m_mint_cache->style_sheet_sources.remove(id);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedStyleWorkingSet::did_store_property_data_from_drive(PropertyID id, GC::Ptr<CSSStyleSheet> style_sheet)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_mint_cache->wrappers.remove(id);
    if (style_sheet)
        m_mint_cache->style_sheet_sources.set(id, GC::Weak<CSSStyleSheet> { *style_sheet });
    else
        m_mint_cache->style_sheet_sources.remove(id);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedStyleWorkingSet::cache_property_wrapper_from_drive(PropertyID id, NonnullRefPtr<StyleValue const> value)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);
    m_mint_cache->wrappers.set(id, move(value));
}

Display ComputedStyleWorkingSet::display_before_box_type_transformation() const
{
    return m_display_before_box_type_transformation;
}

void ComputedStyleWorkingSet::set_display_before_box_type_transformation(Display value)
{
    m_display_before_box_type_transformation = value;
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

ComputedValuesFFI::AnimatedOverlay* ComputedStyleWorkingSet::prepare_animated_overlay_for_rust_finalization(Badge<StyleComputer>, CreateAnimatedOverlay create)
{
    if (!m_animated_properties && create == CreateAnimatedOverlay::No)
        return nullptr;
    auto& animated_properties = mutable_animated_properties();
    animated_properties.clear_wrapper_cache();
    return const_cast<ComputedValuesFFI::AnimatedOverlay*>(animated_properties.overlay());
}

void ComputedStyleWorkingSet::finish_animated_overlay_rust_mutation(Badge<StyleComputer>)
{
    if (m_animated_properties && m_animated_properties->is_empty())
        m_animated_properties = nullptr;
}

void ComputedStyleWorkingSet::did_apply_style_finalization_from_rust(u16 invalidated_longhands)
{
    auto invalidate = [&](u16 flag, PropertyID property_id) {
        if (invalidated_longhands & flag)
            did_store_property_data_from_drive(property_id, nullptr);
    };
    invalidate(ComputedValuesFFI::FINALIZED_FLOAT, PropertyID::Float);
    invalidate(ComputedValuesFFI::FINALIZED_DISPLAY, PropertyID::Display);
    invalidate(ComputedValuesFFI::FINALIZED_LINE_HEIGHT, PropertyID::LineHeight);
    invalidate(ComputedValuesFFI::FINALIZED_POSITION, PropertyID::Position);
    invalidate(ComputedValuesFFI::FINALIZED_TEXT_ALIGN, PropertyID::TextAlign);
    invalidate(ComputedValuesFFI::FINALIZED_OVERFLOW_X, PropertyID::OverflowX);
    invalidate(ComputedValuesFFI::FINALIZED_OVERFLOW_Y, PropertyID::OverflowY);
}

void ComputedStyleWorkingSet::clear_animated_properties(Badge<StyleComputer>)
{
    if (!m_animated_properties)
        return;

    m_animated_properties = nullptr;
    clear_computed_font_list_cache();
}

void ComputedStyleWorkingSet::reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>)
{
    bool has_non_inherited_property = false;
    bool should_clear_computed_font_list_cache = false;
    for (auto const& property : animated_properties().entries()) {
        if (property.inherited)
            continue;
        has_non_inherited_property = true;
        if (property_affects_computed_font_list(static_cast<PropertyID>(property.property))) {
            should_clear_computed_font_list_cache = true;
            break;
        }
    }

    if (!has_non_inherited_property)
        return;

    auto& animated_properties = mutable_animated_properties();
    animated_properties.reset_non_inherited_properties();
    if (animated_properties.is_empty())
        m_animated_properties = nullptr;

    if (should_clear_computed_font_list_cache)
        clear_computed_font_list_cache();
}

NonnullRefPtr<StyleValue const> wrap_computed_longhand_slot(void const* value_data, GC::Ptr<CSSStyleSheet> style_sheet)
{
    auto wrapper = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(value_data)));
    ++s_longhand_wrappers_minted;
    if (style_sheet)
        const_cast<StyleValue&>(*wrapper).set_style_sheet(style_sheet);
    return wrapper;
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
    // Mints the wrapper on demand, stamping a table-stored value with the sheet the winning
    // declaration came from when the drive recorded one; image fetches read that context, and
    // the mint happens before any group fallback consumes the value.
    GC::Ptr<CSSStyleSheet> style_sheet;
    if (effective.source == ComputedValuesFFI::EFFECTIVE_LONGHAND_SOURCE_TABLE) {
        if (auto source = m_mint_cache->style_sheet_sources.get(property_id); source.has_value())
            style_sheet = source->ptr();
    }
    if (!style_sheet) {
        auto initial_value = property_initial_value(property_id);
        if (initial_value->rust_style_value_data() == effective.value) {
            cache.set(property_id, initial_value);
            return *initial_value;
        }
    }
    auto wrapper = wrap_computed_longhand_slot(effective.value, style_sheet);
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

void ComputedStyleWorkingSet::collect_effective_longhand_overrides(Vector<u16>& properties, Vector<void const*>& values) const
{
    auto const* table = m_computed_longhand_table;
    auto const* overlay = m_animated_properties ? m_animated_properties->overlay() : nullptr;
    auto capacity = ComputedValuesFFI::rust_computed_longhand_table_effective_override_capacity(table, overlay);
    if (capacity == 0)
        return;
    properties.resize(capacity);
    values.resize(capacity);
    auto count = ComputedValuesFFI::rust_computed_longhand_table_collect_effective_overrides(table, overlay, properties.data(), values.data(), capacity);
    properties.resize(count);
    values.resize(count);
}

Color ComputedStyleWorkingSet::color(PropertyID id, ColorResolutionContext color_resolution_context) const
{
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_storage;
    auto input = make_rust_color_resolution_input(color_resolution_context, length_storage);
    auto resolved = StyleValueFFI::rust_style_value_to_color(effective_property_data(id), &input);
    VERIFY(resolved.resolved);
    return Color(resolved.rgba[0], resolved.rgba[1], resolved.rgba[2], resolved.rgba[3]);
}

// https://drafts.csswg.org/css-values-4/#linked-properties
static HashMap<PropertyID, StyleValueVector> assemble_coordinated_value_list(ComputedStyleWorkingSet const& style, PropertyID base_property_id, Vector<PropertyID> const& property_ids)
{
    // A coordinating list property group creates a coordinated value list, which has, for each entry, a value from each
    // property in the group; these are used together to define a single effect, such as a background image layer or an
    // animation. The coordinated value list is assembled as follows:
    // - The length of the coordinated value list is determined by the number of items specified in one particular
    //   coordinating list property, the coordinating list base property. (In the case of backgrounds, this is the
    //   background-image property.)
    // - The Nth value of the coordinated value list is constructed by collecting the Nth use value of each coordinating
    //   list property
    // - If a coordinating list property has too many values specified, excess values at the end of its list are not
    //   used.
    // - If a coordinating list property has too few values specified, its value list is repeated to add more used
    //   values.
    // - The computed values of the coordinating list properties are not affected by such truncation or repetition.
    HashMap<PropertyID, StyleValueVector> coordinated_value_list;

    for (size_t i = 0; i < style.property(base_property_id).as_value_list().size(); i++) {
        for (auto property_id : property_ids) {
            auto const& list = style.property(property_id).as_value_list().values();

            coordinated_value_list.ensure(property_id).append(list[i % list.size()]);
        }
    }

    return coordinated_value_list;
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
PreferredColorScheme ComputedStyleWorkingSet::color_scheme(PreferredColorScheme preferred_scheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const
{
    if (!has_animated_property(PropertyID::ColorScheme) && m_effective_color_scheme.has_value())
        return *m_effective_color_scheme;

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

LineHeightData ComputedStyleWorkingSet::line_height_data() const
{
    auto const& value = property(PropertyID::LineHeight);
    LineHeightData data;

    if (value.is_keyword() && value.to_keyword() == Keyword::Normal) {
        data.computed_value = LineHeightData::Normal {};
    } else if (value.is_number()) {
        data.computed_value = value.as_number().number();
    } else {
        data.computed_value = value.as_length().length();
    }

    return data;
}

float ComputedStyleWorkingSet::stop_opacity() const
{
    return property(PropertyID::StopOpacity).as_opacity_value().resolved();
}

float ComputedStyleWorkingSet::flood_opacity() const
{
    return property(PropertyID::FloodOpacity).as_opacity_value().resolved();
}

ImageRendering ComputedStyleWorkingSet::image_rendering() const
{
    auto const& value = property(PropertyID::ImageRendering);
    return keyword_to_image_rendering(value.to_keyword()).release_value();
}

CSSPixels ComputedStyleWorkingSet::border_spacing_horizontal() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 0);
        return Length::from_style_value(list.value_at(0, false), {}).absolute_length_to_px();
    }

    return Length::from_style_value(style_value, {}).absolute_length_to_px();
}

CSSPixels ComputedStyleWorkingSet::border_spacing_vertical() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 1);
        return Length::from_style_value(list.value_at(1, false), {}).absolute_length_to_px();
    }

    return Length::from_style_value(style_value, {}).absolute_length_to_px();
}

CaptionSide ComputedStyleWorkingSet::caption_side() const
{
    auto const& value = property(PropertyID::CaptionSide);
    return keyword_to_caption_side(value.to_keyword()).release_value();
}

Color ComputedStyleWorkingSet::accent_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::AccentColor);

    if (value.to_keyword() == Keyword::Auto)
        return CSS::SystemColor::accent_color(color_resolution_context.color_scheme.value());

    return value.to_color(color_resolution_context).value();
}

TextRendering ComputedStyleWorkingSet::text_rendering() const
{
    auto const& value = property(PropertyID::TextRendering);
    return keyword_to_text_rendering(value.to_keyword()).release_value();
}

CSSPixels ComputedStyleWorkingSet::text_underline_offset() const
{
    auto const& computed_text_underline_offset = property(PropertyID::TextUnderlineOffset);

    // auto
    if (computed_text_underline_offset.to_keyword() == Keyword::Auto)
        return InitialValues::text_underline_offset();

    // <length>
    // <percentage>
    return Length::from_style_value(computed_text_underline_offset, Length::make_px(font_size())).absolute_length_to_px();
}

CSSPixels ComputedStyleWorkingSet::word_spacing() const
{
    auto const& value = property(PropertyID::WordSpacing);
    if (value.is_keyword() && value.to_keyword() == Keyword::Normal)
        return 0;

    return Length::from_style_value(value, Length::make_px(font_size())).absolute_length_to_px();
}

CSSPixels ComputedStyleWorkingSet::letter_spacing() const
{
    auto const& value = property(PropertyID::LetterSpacing);
    if (value.is_keyword() && value.to_keyword() == Keyword::Normal)
        return 0;

    return Length::from_style_value(value, Length::make_px(font_size())).absolute_length_to_px();
}

Color ComputedStyleWorkingSet::caret_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::CaretColor);
    if (value.is_keyword() && value.to_keyword() == Keyword::Auto)
        return color_resolution_context.current_color.value_or(InitialValues::color());

    if (value.has_color())
        return value.to_color(color_resolution_context).value();

    return InitialValues::caret_color();
}

ContentVisibility ComputedStyleWorkingSet::content_visibility() const
{
    auto const& value = property(PropertyID::ContentVisibility);
    return keyword_to_content_visibility(value.to_keyword()).release_value();
}

Visibility ComputedStyleWorkingSet::visibility() const
{
    auto const& value = property(PropertyID::Visibility);
    if (!value.is_keyword())
        return {};
    return keyword_to_visibility(value.to_keyword()).release_value();
}

Display ComputedStyleWorkingSet::display() const
{
    auto const* value = static_cast<StyleValueFFI::StyleValueData const*>(effective_property_data(PropertyID::Display));
    VERIFY(value->tag == StyleValueFFI::StyleValueData::Tag::Display);
    return bit_cast<Display>(value->display.raw);
}

ListStyleType ComputedStyleWorkingSet::list_style_type(StyleScope const& style_scope) const
{
    auto const& value = property(PropertyID::ListStyleType);

    if (value.to_keyword() == Keyword::None)
        return Empty {};

    if (value.is_string())
        return value.as_string().string_value().to_utf16_string();

    auto counter_style = value.as_counter_style().resolve_counter_style(style_scope);
    if (counter_style)
        return counter_style;

    VERIFY(value.as_counter_style().value().has<Utf16FlyString>());
    return UnresolvedCounterStyleName { value.as_counter_style().value().get<Utf16FlyString>() };
}

FontKerning ComputedStyleWorkingSet::font_kerning() const
{
    auto const& value = property(PropertyID::FontKerning);
    return keyword_to_font_kerning(value.to_keyword()).release_value();
}

Optional<Utf16FlyString> ComputedStyleWorkingSet::font_language_override() const
{
    auto const& value = property(PropertyID::FontLanguageOverride);
    if (value.is_string())
        return value.as_string().string_value();
    return {};
}

FontFeatureData ComputedStyleWorkingSet::font_feature_data() const
{
    return {
        .font_variant_alternates = font_variant_alternates(),
        .font_variant_caps = font_variant_caps(),
        .font_variant_east_asian = font_variant_east_asian(),
        .font_variant_emoji = font_variant_emoji(),
        .font_variant_ligatures = font_variant_ligatures(),
        .font_variant_numeric = font_variant_numeric(),
        .font_variant_position = font_variant_position(),
        .font_feature_settings = font_feature_settings(),
        .font_kerning = font_kerning(),
        .text_rendering = text_rendering(),
    };
}

Optional<FontVariantAlternates> ComputedStyleWorkingSet::font_variant_alternates() const
{
    auto const& value = property(PropertyID::FontVariantAlternates);

    // normal
    if (value.is_keyword()) {
        VERIFY(value.to_keyword() == Keyword::Normal);
        return {};
    }

    FontVariantAlternates alternates;

    for (auto const& value : value.as_value_list().values()) {
        // historical-forms
        if (value->is_keyword() && value->to_keyword() == Keyword::HistoricalForms) {
            alternates.historical_forms = true;
            continue;
        }

        if (value->is_function()) {
            auto function_type = font_feature_value_type_from_string(value->as_function().name()).release_value();
            auto const& names = value->as_function().value()->as_value_list().values();

            for (auto const& name : names)
                alternates.font_feature_value_entries.append({ function_type, string_from_style_value(name) });

            continue;
        }

        VERIFY_NOT_REACHED();
    }

    return alternates;
}

FontVariantCaps ComputedStyleWorkingSet::font_variant_caps() const
{
    auto const& value = property(PropertyID::FontVariantCaps);
    return keyword_to_font_variant_caps(value.to_keyword()).release_value();
}

Optional<FontVariantEastAsian> ComputedStyleWorkingSet::font_variant_east_asian() const
{
    auto const& value = property(PropertyID::FontVariantEastAsian);

    if (value.to_keyword() == Keyword::Normal)
        return {};

    auto const& tuple = value.as_tuple().tuple();

    FontVariantEastAsian east_asian {};

    if (tuple[TupleStyleValue::Indices::FontVariantEastAsian::Variant])
        east_asian.variant = keyword_to_east_asian_variant(tuple[TupleStyleValue::Indices::FontVariantEastAsian::Variant]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantEastAsian::Width])
        east_asian.width = keyword_to_east_asian_width(tuple[TupleStyleValue::Indices::FontVariantEastAsian::Width]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantEastAsian::Ruby])
        east_asian.ruby = true;

    return east_asian;
}

FontVariantEmoji ComputedStyleWorkingSet::font_variant_emoji() const
{
    auto const& value = property(PropertyID::FontVariantEmoji);
    return keyword_to_font_variant_emoji(value.to_keyword()).release_value();
}

Optional<FontVariantLigatures> ComputedStyleWorkingSet::font_variant_ligatures() const
{
    auto const& value = property(PropertyID::FontVariantLigatures);

    if (value.to_keyword() == Keyword::Normal)
        return {};

    if (value.to_keyword() == Keyword::None)
        return FontVariantLigatures { .none = true };

    auto const& tuple = value.as_tuple().tuple();

    FontVariantLigatures ligatures {};

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Common])
        ligatures.common = keyword_to_common_lig_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Common]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Discretionary])
        ligatures.discretionary = keyword_to_discretionary_lig_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Discretionary]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Historical])
        ligatures.historical = keyword_to_historical_lig_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Historical]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Contextual])
        ligatures.contextual = keyword_to_contextual_alt_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Contextual]->to_keyword()).value();

    return ligatures;
}

Optional<FontVariantNumeric> ComputedStyleWorkingSet::font_variant_numeric() const
{
    auto const& value = property(PropertyID::FontVariantNumeric);

    if (value.to_keyword() == Keyword::Normal)
        return {};

    auto const& tuple = value.as_tuple().tuple();

    FontVariantNumeric numeric {};

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Figure])
        numeric.figure = keyword_to_numeric_figure_value(tuple[TupleStyleValue::Indices::FontVariantNumeric::Figure]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Spacing])
        numeric.spacing = keyword_to_numeric_spacing_value(tuple[TupleStyleValue::Indices::FontVariantNumeric::Spacing]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Fraction])
        numeric.fraction = keyword_to_numeric_fraction_value(tuple[TupleStyleValue::Indices::FontVariantNumeric::Fraction]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Ordinal])
        numeric.ordinal = true;

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::SlashedZero])
        numeric.slashed_zero = true;

    return numeric;
}

FontVariantPosition ComputedStyleWorkingSet::font_variant_position() const
{
    auto const& value = property(PropertyID::FontVariantPosition);
    return keyword_to_font_variant_position(value.to_keyword()).release_value();
}

HashMap<Utf16FlyString, u8> ComputedStyleWorkingSet::font_feature_settings() const
{
    auto const& value = property(PropertyID::FontFeatureSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& feature_tags = value.as_value_list().values();
        HashMap<Utf16FlyString, u8> result;
        result.ensure_capacity(feature_tags.size());
        for (auto const& tag_value : feature_tags) {
            auto const& feature_tag = tag_value->as_open_type_tagged();

            result.set(feature_tag.tag(), int_from_style_value(feature_tag.value()));
        }
        return result;
    }

    return {};
}

HashMap<Utf16FlyString, double> ComputedStyleWorkingSet::font_variation_settings() const
{
    auto const& value = property(PropertyID::FontVariationSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& axis_tags = value.as_value_list().values();
        HashMap<Utf16FlyString, double> result;
        result.ensure_capacity(axis_tags.size());
        for (auto const& tag_value : axis_tags) {
            auto const& axis_tag = tag_value->as_open_type_tagged();

            result.set(axis_tag.tag(), number_from_style_value(axis_tag.value(), {}));
        }
        return result;
    }

    return {};
}

BorderCollapse ComputedStyleWorkingSet::border_collapse() const
{
    auto const& value = property(PropertyID::BorderCollapse);
    return keyword_to_border_collapse(value.to_keyword()).release_value();
}

EmptyCells ComputedStyleWorkingSet::empty_cells() const
{
    auto const& value = property(PropertyID::EmptyCells);
    return keyword_to_empty_cells(value.to_keyword()).release_value();
}

Direction ComputedStyleWorkingSet::direction() const
{
    auto const& value = property(PropertyID::Direction);
    return keyword_to_direction(value.to_keyword()).release_value();
}

WritingMode ComputedStyleWorkingSet::writing_mode() const
{
    auto const& value = property(PropertyID::WritingMode);
    return keyword_to_writing_mode(value.to_keyword()).release_value();
}

Vector<AnimationProperties> ComputedStyleWorkingSet::animations(DOM::AbstractElement const& abstract_element) const
{
    auto const& animation_name_values = property(PropertyID::AnimationName).as_value_list().values();

    // OPTIMIZATION: If all animation names are 'none', there are no animations to process
    if (all_of(animation_name_values, [](auto const& value) { return value->to_keyword() == Keyword::None; }))
        return {};

    // CSS Animations are defined by binding keyframes to an element using the animation-* properties. These list-valued
    // properties, which are all longhands of the animation shorthand, form a coordinating list property group with
    // animation-name as the coordinating list base property and each item in the coordinated value list defining the
    // properties of a single animation effect.
    auto const& coordinated_properties = assemble_coordinated_value_list(
        *this,
        PropertyID::AnimationName,
        { PropertyID::AnimationDuration,
            PropertyID::AnimationTimingFunction,
            PropertyID::AnimationIterationCount,
            PropertyID::AnimationDirection,
            PropertyID::AnimationPlayState,
            PropertyID::AnimationDelay,
            PropertyID::AnimationFillMode,
            PropertyID::AnimationComposition,
            PropertyID::AnimationName,
            PropertyID::AnimationTimeline });

    Vector<AnimationProperties> animations;

    for (size_t i = 0; i < coordinated_properties.get(PropertyID::AnimationName)->size(); i++) {
        // https://drafts.csswg.org/css-animations-1/#propdef-animation-name
        // none: No keyframes are specified at all, so there will be no animation. Any other animations properties
        //       specified for this animation have no effect.
        if (coordinated_properties.get(PropertyID::AnimationName).value()[i]->to_keyword() == Keyword::None)
            continue;

        auto animation_name_style_value = coordinated_properties.get(PropertyID::AnimationName).value()[i];
        auto animation_duration_style_value = coordinated_properties.get(PropertyID::AnimationDuration).value()[i];
        auto animation_timing_function_style_value = coordinated_properties.get(PropertyID::AnimationTimingFunction).value()[i];
        auto animation_iteration_count_style_value = coordinated_properties.get(PropertyID::AnimationIterationCount).value()[i];
        auto animation_direction_style_value = coordinated_properties.get(PropertyID::AnimationDirection).value()[i];
        auto animation_play_state_style_value = coordinated_properties.get(PropertyID::AnimationPlayState).value()[i];
        auto animation_delay_style_value = coordinated_properties.get(PropertyID::AnimationDelay).value()[i];
        auto animation_fill_mode_style_value = coordinated_properties.get(PropertyID::AnimationFillMode).value()[i];
        auto animation_composition_style_value = coordinated_properties.get(PropertyID::AnimationComposition).value()[i];
        auto animation_timeline_style_value = coordinated_properties.get(PropertyID::AnimationTimeline).value()[i];

        // https://drafts.csswg.org/css-animations-2/#animation-duration
        auto duration = [&] -> Variant<double, Utf16String> {
            // auto
            if (animation_duration_style_value->to_keyword() == Keyword::Auto) {
                // Preserve auto until the animation effect is associated with its timeline. Time-driven animations
                // will resolve this to 0s, while scroll-driven animations fill the progress-based timeline.
                return "auto"_utf16;
            }

            // <time [0s,∞]>

            // FIXME: For scroll-driven animations, treated as auto.

            // For time-driven animations, specifies the length of time that an animation takes to complete one cycle.
            // A negative <time> is invalid.
            return Time::from_style_value(animation_duration_style_value, {}).to_milliseconds();
        }();

        auto timing_function = EasingFunction::from_style_value(animation_timing_function_style_value);

        auto iteration_count = [&] {
            if (animation_iteration_count_style_value->to_keyword() == Keyword::Infinite)
                return AK::Infinity<double>;

            return number_from_style_value(animation_iteration_count_style_value, {});
        }();

        auto direction = keyword_to_animation_direction(animation_direction_style_value->to_keyword()).value();
        auto play_state = keyword_to_animation_play_state(animation_play_state_style_value->to_keyword()).value();
        auto delay = Time::from_style_value(animation_delay_style_value, {}).to_milliseconds();
        auto fill_mode = keyword_to_animation_fill_mode(animation_fill_mode_style_value->to_keyword()).value();
        auto composition = keyword_to_animation_composition(animation_composition_style_value->to_keyword()).value();
        auto const& name = string_from_style_value(animation_name_style_value);

        // https://drafts.csswg.org/css-animations-2/#animation-timeline
        auto const& timeline = [&]() -> GC::Ptr<Animations::AnimationTimeline> {
            // auto
            // The animation’s timeline is a DocumentTimeline, more specifically the default document timeline.
            if (animation_timeline_style_value->to_keyword() == Keyword::Auto)
                return abstract_element.document().timeline();

            // none
            // The animation is not associated with a timeline.
            if (animation_timeline_style_value->to_keyword() == Keyword::None)
                return nullptr;

            // <dashed-ident>
            // FIXME: If a named scroll progress timeline or view progress timeline is in scope on this element, use the
            //        referenced timeline as defined in Scroll-driven Animations §  Declaring a Named Timeline’s Scope:
            //        the timeline-scope property. Otherwise the animation is not associated with a timeline.

            // <scroll()>
            // Use the scroll progress timeline indicated by the given scroll() function. See Scroll-driven Animations
            // § 2.2.1 The scroll() notation.
            if (animation_timeline_style_value->is_function() && animation_timeline_style_value->as_function().name() == "scroll"_utf16_fly_string) {
                auto const& arguments = animation_timeline_style_value->as_function().value()->as_tuple().tuple();

                auto const& scroller = arguments[TupleStyleValue::Indices::ScrollFunction::Scroller]
                    ? keyword_to_scroller(arguments[TupleStyleValue::Indices::ScrollFunction::Scroller]->to_keyword()).value()
                    : Scroller::Nearest;

                auto const& axis = arguments[TupleStyleValue::Indices::ScrollFunction::Axis]
                    ? Animations::scroll_axis_from_css_axis(keyword_to_axis(arguments[TupleStyleValue::Indices::ScrollFunction::Axis]->to_keyword()).value())
                    : Animations::ScrollAxis::Block;

                Animations::ScrollTimeline::AnonymousSource source {
                    .scroller = scroller,
                    .target = abstract_element,
                };

                return Animations::ScrollTimeline::create(abstract_element.document(), source, axis);
            }

            //<view()>
            // FIXME: Use the view progress timeline indicated by the given view() function. See Scroll-driven
            //        Animations § 3.3.1 The view() notation.

            // FIXME: We fall back to document timeline for now as though we don't support the `animation-timeline`
            //        property at all
            return abstract_element.document().timeline();
        }();

        animations.append(AnimationProperties {
            .duration = duration,
            .timing_function = timing_function,
            .iteration_count = iteration_count,
            .direction = direction,
            .play_state = play_state,
            .delay = delay,
            .fill_mode = fill_mode,
            .composition = composition,
            .name = name,
            .timeline = timeline,
        });
    }

    return animations;
}

Vector<TransitionProperties> ComputedStyleWorkingSet::transitions() const
{
    auto const& coordinated_properties = assemble_coordinated_value_list(
        *this,
        PropertyID::TransitionProperty,
        { PropertyID::TransitionProperty, PropertyID::TransitionDuration, PropertyID::TransitionTimingFunction, PropertyID::TransitionDelay, PropertyID::TransitionBehavior });

    auto const& property_values = coordinated_properties.get(PropertyID::TransitionProperty).value();
    auto const& duration_values = coordinated_properties.get(PropertyID::TransitionDuration).value();
    auto const& timing_function_values = coordinated_properties.get(PropertyID::TransitionTimingFunction).value();
    auto const& delay_values = coordinated_properties.get(PropertyID::TransitionDelay).value();
    auto const& behavior_values = coordinated_properties.get(PropertyID::TransitionBehavior).value();

    Vector<TransitionProperties> transitions;
    transitions.ensure_capacity(property_values.size());

    for (size_t i = 0; i < property_values.size(); i++) {
        auto properties = [&]() -> Vector<PropertyID> {
            auto const& property_value = property_values[i];

            if (property_value->is_keyword() && property_value->to_keyword() == Keyword::None)
                return {};

            auto maybe_property = property_id_from_string(property_value->as_custom_ident().custom_ident());
            if (!maybe_property.has_value())
                return {};

            Vector<PropertyID> properties;

            auto const append_property_mapping_logical_aliases = [&](PropertyID property_id) {
                if (property_is_logical_alias(property_id))
                    properties.append(map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { writing_mode(), direction() }));
                else if (property_id != PropertyID::Custom)
                    properties.append(property_id);
            };

            auto transition_property = maybe_property.release_value();
            if (property_is_shorthand(transition_property)) {
                auto expanded_longhands = expanded_longhands_for_shorthand(transition_property);

                properties.ensure_capacity(expanded_longhands.size());

                for (auto const& prop : expanded_longhands_for_shorthand(transition_property))
                    append_property_mapping_logical_aliases(prop);
            } else {
                append_property_mapping_logical_aliases(transition_property);
            }

            return properties;
        }();

        transitions.append(TransitionProperties {
            .properties = properties,
            .duration = Time::from_style_value(duration_values[i], {}).to_milliseconds(),
            .timing_function = EasingFunction::from_style_value(timing_function_values[i]),
            .delay = Time::from_style_value(delay_values[i], {}).to_milliseconds(),
            .transition_behavior = keyword_to_transition_behavior(behavior_values[i]->to_keyword()).value(),
        });
    }

    return transitions;
}

ScrollbarColorData ComputedStyleWorkingSet::scrollbar_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::ScrollbarColor);
    if (value.is_keyword() && value.as_keyword().keyword() == Keyword::Auto)
        return InitialValues::scrollbar_color();

    if (value.is_scrollbar_color()) {
        auto& scrollbar_color_value = value.as_scrollbar_color();
        auto thumb_color = scrollbar_color_value.thumb_color()->to_color(color_resolution_context).value();
        auto track_color = scrollbar_color_value.track_color()->to_color(color_resolution_context).value();
        return {
            .thumb_color = thumb_color,
            .track_color = track_color,
            .is_auto = false,
        };
    }

    return {};
}

ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> ComputedStyleWorkingSet::computed_font_list(FontComputer const& font_computer) const
{
    if (!m_cached_computed_font_list) {
        m_cached_computed_font_list = font_computer.compute_font_for_style_values(computed_font_families(), font_size(), font_slope(), font_weight(), font_width(), font_optical_sizing(), font_variation_settings(), font_feature_data());
        VERIFY(!m_cached_computed_font_list->is_empty());
    }

    return *m_cached_computed_font_list;
}

ValueComparingNonnullRefPtr<Gfx::Font const> ComputedStyleWorkingSet::first_available_computed_font(FontComputer const& font_computer) const
{
    if (!m_cached_first_available_computed_font)
        m_cached_first_available_computed_font = computed_font_list(font_computer)->first_available_font();
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
            .name = Utf16FlyString::from_raw(entry.string_raw),
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
