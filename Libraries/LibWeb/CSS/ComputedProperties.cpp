/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16StringBuilder.h>
#include <LibCore/DirIterator.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::CSS {

extern "C" void ladybird_animated_properties_ref(void const*);
extern "C" void ladybird_animated_properties_unref(void const*);
extern "C" void style_engine_recording_pointer_will_die(void const*);

static Atomic<u64> s_next_animated_properties_identity { 1 };
static u64 s_longhand_wrappers_minted { 0 };

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

AnimatedProperties::~AnimatedProperties()
{
    style_engine_recording_pointer_will_die(this);
}

extern "C" void ladybird_animated_properties_ref(void const* values)
{
    static_cast<AnimatedProperties const*>(values)->ref();
}

extern "C" void ladybird_animated_properties_unref(void const* values)
{
    static_cast<AnimatedProperties const*>(values)->unref();
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

static_assert(to_underlying(PseudoElement::KnownPseudoElementCount) <= sizeof(u64) * 8);

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

    // The animated overlay first, under the same rule property() applies: important base values
    // override animated but not transitioned properties.
    if (with_animations_applied == WithAnimationsApplied::Yes && m_animated_properties && m_animated_properties->has_property(property_id)
        && (!is_property_important(property_id) || m_animated_properties->is_property_result_of_transition(property_id)))
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

static size_t property_bitmap_index(PropertyID property_id)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    return to_underlying(property_id) - to_underlying(first_longhand_property_id);
}

ComputedProperties::Data::Data()
    : computed_longhand_table(ComputedValuesFFI::rust_computed_longhand_table_create())
{
}

ComputedProperties::Data::~Data()
{
    ComputedValuesFFI::rust_computed_longhand_table_release(computed_longhand_table);
}

ComputedProperties::Builder::Builder()
    : m_data(adopt_ref(*new Data))
    , m_style(adopt_ref(*new ComputedProperties(m_data, false, false)))
{
}

ComputedProperties::Builder::Builder(ComputedProperties const& style)
    : Builder()
{
    m_data->property_values = style.data().property_values;
    ComputedValuesFFI::rust_computed_longhand_table_copy_from(m_data->computed_longhand_table, style.data().computed_longhand_table);
    m_data->property_important = style.data().property_important;
    m_data->property_inherited = style.data().property_inherited;
    m_data->property_evaluated = style.data().property_evaluated;
    m_data->style_sheet_sources = style.data().style_sheet_sources;
    m_data->display_before_box_type_transformation = style.data().display_before_box_type_transformation;
    m_data->pseudo_element_styles = style.data().pseudo_element_styles;
    m_data->line_height = style.data().line_height;
    m_data->effective_color_scheme = style.data().effective_color_scheme;
    m_data->inheritance_dependent_specified_values = style.data().inheritance_dependent_specified_values;
    m_data->raw_cascaded_font_size = style.data().raw_cascaded_font_size;
    m_depends_on_viewport_metrics = style.depends_on_viewport_metrics();
    m_font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics();
    m_in_display_none_subtree = style.in_display_none_subtree();
    m_style->m_depends_on_viewport_metrics = m_depends_on_viewport_metrics;
    m_style->m_font_metrics_depend_on_viewport_metrics = m_font_metrics_depend_on_viewport_metrics;
    m_style->m_in_display_none_subtree = m_in_display_none_subtree;
    if (style.m_animated_properties)
        m_style->m_animated_properties = adopt_ref(*new AnimatedProperties(*style.m_animated_properties));
}

ComputedProperties::Builder::Builder(ComputedValues const& style)
    : Builder()
{
    auto const& base = style.base_values();
    // Seed the table with the previous drive's computed values so unevaluated longhands read
    // and publish without reverse-materialization; the funnel overwrites what this drive
    // evaluates. A borrowed record view seeds from its interned value span. The base always
    // carries a table: property() has no other source for an unevaluated longhand's value.
    if (auto const* table = base.computed_longhand_table()) {
        ComputedValuesFFI::rust_computed_longhand_table_copy_from(m_data->computed_longhand_table, static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(table));
    } else {
        auto longhand_values = base.computed_longhand_values();
        VERIFY(!longhand_values.is_empty());
        ComputedValuesFFI::rust_computed_longhand_table_copy_from_values(m_data->computed_longhand_table, longhand_values.data(), longhand_values.size());
    }
    m_data->property_important.copy_from(base.property_importance_bitmap());
    m_data->property_inherited.copy_from(base.property_inheritance_bitmap());
    m_data->display_before_box_type_transformation = base.display_before_box_type_transformation();
    m_data->pseudo_element_styles = base.pseudo_element_style_mask();
    m_data->inheritance_dependent_specified_values = base.inheritance_dependent_specified_values_snapshot();
    m_data->raw_cascaded_font_size = base.raw_cascaded_font_size();
    m_depends_on_viewport_metrics = base.depends_on_viewport_metrics();
    m_font_metrics_depend_on_viewport_metrics = base.font_metrics_depend_on_viewport_metrics();
    m_in_display_none_subtree = base.in_display_none_subtree();
    m_style->m_depends_on_viewport_metrics = m_depends_on_viewport_metrics;
    m_style->m_font_metrics_depend_on_viewport_metrics = m_font_metrics_depend_on_viewport_metrics;
    m_style->m_in_display_none_subtree = m_in_display_none_subtree;
}

NonnullRefPtr<ComputedProperties> ComputedProperties::Builder::build() &&
{
    ComputedValuesFFI::rust_computed_longhand_table_freeze(m_data->computed_longhand_table);
    m_style->m_depends_on_viewport_metrics = m_depends_on_viewport_metrics;
    m_style->m_font_metrics_depend_on_viewport_metrics = m_font_metrics_depend_on_viewport_metrics;
    m_style->m_in_display_none_subtree = m_in_display_none_subtree;
    return move(m_style);
}

ComputedProperties::Builder ComputedProperties::create_builder()
{
    return Builder {};
}

ComputedProperties::Builder ComputedProperties::create_builder_with_base_values_from(ComputedProperties const& style)
{
    return Builder { style };
}

ComputedProperties::Builder ComputedProperties::create_builder_with_base_values_from(ComputedValues const& style)
{
    return Builder { style };
}

NonnullRefPtr<ComputedProperties> ComputedProperties::create(Builder&& builder)
{
    return move(builder).build();
}

AnimatedProperties::AnimatedProperties()
    : m_identity(s_next_animated_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
{
}

AnimatedProperties::AnimatedProperties(AnimatedProperties const& other)
    : m_identity(s_next_animated_properties_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_has_property(other.m_has_property)
    , m_property_inherited(other.m_property_inherited)
    , m_property_result_of_transition(other.m_property_result_of_transition)
    , m_values(other.m_values)
{
}

ComputedProperties::ComputedProperties(NonnullRefPtr<Data const> data, bool depends_on_viewport_metrics, bool font_metrics_depend_on_viewport_metrics)
    : m_data(move(data))
    , m_depends_on_viewport_metrics(depends_on_viewport_metrics)
    , m_font_metrics_depend_on_viewport_metrics(font_metrics_depend_on_viewport_metrics)
{
}

ComputedProperties::~ComputedProperties() = default;

size_t ComputedProperties::retained_size_in_bytes() const
{
    auto const& inheritance_dependent_values = m_data->inheritance_dependent_specified_values;
    return sizeof(ComputedProperties) + sizeof(Data)
        + inheritance_dependent_values.capacity()
        * (sizeof(PropertyID) + sizeof(NonnullRefPtr<StyleValue const>) + 1);
}

u64 ComputedProperties::longhand_wrappers_minted()
{
    return s_longhand_wrappers_minted;
}

void ComputedProperties::reset_longhand_wrappers_minted()
{
    s_longhand_wrappers_minted = 0;
}

void ComputedProperties::count_longhand_wrapper_mint(Badge<StyleComputer>)
{
    ++s_longhand_wrappers_minted;
}

NonnullRefPtr<ComputedProperties> ComputedProperties::copy_without_animations() const
{
    auto copy = adopt_ref(*new ComputedProperties(m_data, m_depends_on_viewport_metrics, m_font_metrics_depend_on_viewport_metrics));
    copy->m_in_display_none_subtree = m_in_display_none_subtree;
    return copy;
}

AnimatedProperties const& ComputedProperties::animated_properties() const
{
    static NeverDestroyed<AnimatedProperties> empty_animated_properties;
    if (!m_animated_properties)
        return *empty_animated_properties;
    return *m_animated_properties;
}

AnimatedProperties& ComputedProperties::mutable_animated_properties()
{
    if (!m_animated_properties)
        m_animated_properties = adopt_ref(*new AnimatedProperties);
    if (m_animated_properties->ref_count() > 1)
        m_animated_properties = adopt_ref(*new AnimatedProperties(*m_animated_properties));
    return *m_animated_properties;
}

bool AnimatedProperties::has_property(PropertyID property_id) const
{
    return m_has_property.get(property_bitmap_index(property_id));
}

bool AnimatedProperties::is_property_inherited(PropertyID property_id) const
{
    return m_property_inherited.get(property_bitmap_index(property_id));
}

bool AnimatedProperties::is_property_result_of_transition(PropertyID property_id) const
{
    return m_property_result_of_transition.get(property_bitmap_index(property_id));
}

StyleValue const& AnimatedProperties::property(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    VERIFY(has_property(property_id));

    auto animated_value = m_values.get(property_id);
    VERIFY(animated_value.has_value());
    return *animated_value.value();
}

void AnimatedProperties::set_property_inherited(PropertyID property_id, ComputedProperties::Inherited inherited)
{
    m_property_inherited.set(property_bitmap_index(property_id), inherited == ComputedProperties::Inherited::Yes);
}

void AnimatedProperties::set_property_result_of_transition(PropertyID property_id, AnimatedPropertyResultOfTransition animated_value_result_of_transition)
{
    m_property_result_of_transition.set(property_bitmap_index(property_id), animated_value_result_of_transition == AnimatedPropertyResultOfTransition::Yes);
}

void AnimatedProperties::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, ComputedProperties::Inherited inherited)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_values.set(id, move(value));

    m_has_property.set(property_bitmap_index(id), true);

    set_property_inherited(id, inherited);
    set_property_result_of_transition(id, animated_property_result_of_transition);
}

void AnimatedProperties::remove_property(PropertyID id)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_values.remove(id);

    m_has_property.set(property_bitmap_index(id), false);
    set_property_inherited(id, ComputedProperties::Inherited::No);
    set_property_result_of_transition(id, AnimatedPropertyResultOfTransition::No);
}

void AnimatedProperties::reset_non_inherited_properties()
{
    for (auto property_id : m_values.keys()) {
        if (!is_property_inherited(property_id))
            remove_property(property_id);
    }
}

bool ComputedProperties::is_property_important(PropertyID property_id) const
{
    return data().property_important.get(property_bitmap_index(property_id));
}

void ComputedProperties::Builder::set_property_important(PropertyID property_id, Important important)
{
    data().property_important.set(property_bitmap_index(property_id), important == Important::Yes);
}

bool ComputedProperties::is_property_inherited(PropertyID property_id) const
{
    return data().property_inherited.get(property_bitmap_index(property_id));
}

HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& ComputedProperties::animated_property_values() const
{
    return animated_properties().values();
}

RefPtr<AnimatedProperties const> ComputedProperties::animated_properties_snapshot() const
{
    return m_animated_properties;
}

bool ComputedProperties::has_animated_property(PropertyID property_id) const
{
    return animated_properties().has_property(property_id);
}

bool ComputedProperties::is_animated_property_inherited(PropertyID property_id) const
{
    return animated_properties().is_property_inherited(property_id);
}

bool ComputedProperties::is_animated_property_result_of_transition(PropertyID property_id) const
{
    return animated_properties().is_property_result_of_transition(property_id);
}

bool ComputedProperties::has_pseudo_element_style(PseudoElement pseudo_element) const
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    return data().pseudo_element_styles & (1ull << to_underlying(pseudo_element));
}

void ComputedProperties::Builder::set_has_pseudo_element_styles(u64 pseudo_element_styles)
{
    constexpr auto known_pseudo_element_count = to_underlying(PseudoElement::KnownPseudoElementCount);
    if constexpr (known_pseudo_element_count < sizeof(u64) * 8)
        VERIFY((pseudo_element_styles >> known_pseudo_element_count) == 0);
    data().pseudo_element_styles |= pseudo_element_styles;
}

void ComputedProperties::Builder::set_property_inherited(PropertyID property_id, Inherited inherited)
{
    data().property_inherited.set(property_bitmap_index(property_id), inherited == Inherited::Yes);
}

void ComputedProperties::Builder::set_depends_on_viewport_metrics()
{
    m_depends_on_viewport_metrics = true;
    m_style->m_depends_on_viewport_metrics = true;
}

void ComputedProperties::Builder::set_font_metrics_depend_on_viewport_metrics()
{
    m_font_metrics_depend_on_viewport_metrics = true;
    m_style->m_font_metrics_depend_on_viewport_metrics = true;
}

void ComputedProperties::Builder::set_in_display_none_subtree()
{
    m_in_display_none_subtree = true;
    m_style->m_in_display_none_subtree = true;
}

void ComputedProperties::Builder::clear_in_display_none_subtree()
{
    m_in_display_none_subtree = false;
    m_style->m_in_display_none_subtree = false;
}

void ComputedProperties::set_depends_on_viewport_metrics(Badge<StyleComputer>)
{
    m_depends_on_viewport_metrics = true;
}

void ComputedProperties::set_font_metrics_depend_on_viewport_metrics(Badge<StyleComputer>)
{
    m_font_metrics_depend_on_viewport_metrics = true;
}

void ComputedProperties::Builder::set_has_pseudo_element_style(PseudoElement pseudo_element)
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    data().pseudo_element_styles |= 1ull << to_underlying(pseudo_element);
}

void ComputedProperties::Builder::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, Inherited inherited, Important important)
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

void ComputedProperties::Builder::set_property_without_modifying_flags(PropertyID id, NonnullRefPtr<StyleValue const> value, i64 style_sheet_source_slot)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    ComputedValuesFFI::rust_computed_longhand_table_set(data().computed_longhand_table, to_underlying(id), value->rust_style_value_data(), style_sheet_source_slot);
    data().property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = move(value);
    data().property_evaluated.set(to_underlying(id) - to_underlying(first_longhand_property_id), true);
    // The cached wrapper carries whatever sheet context its maker gave it; a mint from the
    // table must not stamp a stale source recorded by an earlier store.
    data().style_sheet_sources.remove(id);

    if (property_affects_computed_font_list(id))
        style().clear_computed_font_list_cache();
}

void ComputedProperties::Builder::set_property_data_from_drive(PropertyID id, void const* value_data, i64 style_sheet_source_slot, GC::Ptr<CSSStyleSheet> style_sheet)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);
    VERIFY(value_data);

    ComputedValuesFFI::rust_computed_longhand_table_set(data().computed_longhand_table, to_underlying(id), value_data, style_sheet_source_slot);
    data().property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = nullptr;
    data().property_evaluated.set(to_underlying(id) - to_underlying(first_longhand_property_id), true);
    if (style_sheet)
        data().style_sheet_sources.set(id, GC::Weak<CSSStyleSheet> { *style_sheet });
    else
        data().style_sheet_sources.remove(id);

    if (property_affects_computed_font_list(id))
        style().clear_computed_font_list_cache();
}

Display ComputedProperties::display_before_box_type_transformation() const
{
    return data().display_before_box_type_transformation;
}

void ComputedProperties::Builder::set_display_before_box_type_transformation(Display value)
{
    data().display_before_box_type_transformation = value;
}

void ComputedProperties::set_animated_property_internal(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    mutable_animated_properties().set_property(id, move(value), animated_property_result_of_transition, inherited);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedProperties::set_animated_property(Badge<StyleComputer>, PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    set_animated_property_internal(id, move(value), animated_property_result_of_transition, inherited);
}

void ComputedProperties::set_animated_property(Badge<DOM::Element>, PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    set_animated_property_internal(id, move(value), animated_property_result_of_transition, inherited);
}

void ComputedProperties::clear_animated_properties(Badge<StyleComputer>)
{
    if (!m_animated_properties)
        return;

    m_animated_properties = nullptr;
    clear_computed_font_list_cache();
}

void ComputedProperties::reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>)
{
    bool has_non_inherited_property = false;
    bool should_clear_computed_font_list_cache = false;
    for (auto const& property : animated_property_values()) {
        if (is_animated_property_inherited(property.key))
            continue;
        has_non_inherited_property = true;
        if (property_affects_computed_font_list(property.key)) {
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

StyleValue const& ComputedProperties::property(PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    // Important properties override animated but not transitioned properties
    if (return_animated_value == WithAnimationsApplied::Yes
        && has_animated_property(property_id)
        && (!is_property_important(property_id) || is_animated_property_result_of_transition(property_id))) {
        return animated_properties().property(property_id);
    }

    auto& value = data().property_values[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
    if (!value) {
        // Mints the wrapper for a table-stored value on demand, stamping it with the sheet the
        // winning declaration came from when the drive recorded one; image fetches read that
        // context, and the mint happens before any group fallback consumes the value.
        auto mint_from_table = [&](void const* stored) {
            auto wrapper = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(stored)));
            ++s_longhand_wrappers_minted;
            if (auto source = data().style_sheet_sources.get(property_id); source.has_value()) {
                if (auto sheet = source->ptr())
                    const_cast<StyleValue&>(*wrapper).set_style_sheet(sheet);
            }
            return wrapper;
        };
        bool const evaluated = data().property_evaluated.get(to_underlying(property_id) - to_underlying(first_longhand_property_id));
        if (auto specified = data().inheritance_dependent_specified_values.get(property_id); !evaluated && specified.has_value() && (*specified)->depends_on_current_color()) {
            // A partial drive seeds its table from the previous style; an unevaluated longhand
            // keeps a recorded specified value that depends on currentColor, in exactly the
            // order computed_style_value_for_inheritance applies.
            value = *specified;
        } else {
            auto const* stored = ComputedValuesFFI::rust_computed_longhand_table_get(data().computed_longhand_table, to_underlying(property_id));
            VERIFY(stored);
            value = mint_from_table(stored);
        }
    }
    return *value;
}

void ComputedProperties::collect_effective_longhand_overrides(Vector<u16>& properties, Vector<void const*>& values) const
{
    auto const* table = data().computed_longhand_table;
    auto push_override = [&](PropertyID property_id, void const* effective) {
        auto const* stored = ComputedValuesFFI::rust_computed_longhand_table_get(table, to_underlying(property_id));
        if (effective == stored)
            return;
        properties.append(to_underlying(property_id));
        values.append(effective);
    };
    auto already_overridden = [&](PropertyID property_id) {
        return properties.contains_slow(to_underlying(property_id));
    };
    // The animated overlay, under property()'s rule: important base values override animated
    // but not transitioned properties. First in the list, because the group builder's override
    // scan takes the first match, like property() prefers the animated value.
    if (m_animated_properties) {
        for (auto const& [property_id, value] : m_animated_properties->values()) {
            if (is_property_important(property_id) && !m_animated_properties->is_property_result_of_transition(property_id))
                continue;
            push_override(property_id, value->rust_style_value_data());
        }
    }
    // An unevaluated longhand keeps the recorded currentcolor-dependent specified value, in
    // the preference order the lazy property() fill applies.
    for (auto const& [property_id, value] : data().inheritance_dependent_specified_values) {
        if (data().property_evaluated.get(to_underlying(property_id) - to_underlying(first_longhand_property_id)))
            continue;
        if (!value->depends_on_current_color())
            continue;
        if (already_overridden(property_id))
            continue;
        push_override(property_id, value->rust_style_value_data());
    }
}

Color ComputedProperties::color(PropertyID id, ColorResolutionContext color_resolution_context) const
{
    return property(id).to_color(color_resolution_context).value();
}

Position ComputedProperties::position_value(PropertyID id) const
{
    auto const& position = property(id).as_position();
    auto edge_x = position.edge_x();
    auto edge_y = position.edge_y();

    return {
        .offset_x = LengthPercentage::from_style_value(edge_x->offset()),
        .offset_y = LengthPercentage::from_style_value(edge_y->offset()),
    };
}

// https://drafts.csswg.org/css-values-4/#linked-properties
HashMap<PropertyID, StyleValueVector> ComputedProperties::assemble_coordinated_value_list(PropertyID base_property_id, Vector<PropertyID> const& property_ids) const
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

    for (size_t i = 0; i < property(base_property_id).as_value_list().size(); i++) {
        for (auto property_id : property_ids) {
            auto const& list = property(property_id).as_value_list().values();

            coordinated_value_list.ensure(property_id).append(list[i % list.size()]);
        }
    }

    return coordinated_value_list;
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
PreferredColorScheme ComputedProperties::color_scheme(PreferredColorScheme preferred_scheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const
{
    if (!has_animated_property(PropertyID::ColorScheme) && m_data->effective_color_scheme.has_value())
        return *m_data->effective_color_scheme;

    // NB: Animated color-scheme values keep using this path until animations move
    //     into the Rust driver.
    // To determine the used color scheme of an element:
    auto const& scheme_value = property(PropertyID::ColorScheme).as_color_scheme();
    auto schemes = scheme_value.schemes();

    // 1. If the user’s preferred color scheme, as indicated by the prefers-color-scheme media feature,
    //    is present among the listed color schemes, and is supported by the user agent,
    //    that’s the element’s used color scheme.
    if (preferred_scheme != PreferredColorScheme::Auto && schemes.contains_slow(preferred_color_scheme_to_utf16_fly_string(preferred_scheme)))
        return preferred_scheme;

    // 2. Otherwise, if the user has indicated an overriding preference for their chosen color scheme,
    //    and the only keyword is not present in color-scheme for the element,
    //    the user agent must override the color scheme with the user’s preferred color scheme.
    //    See § 2.3 Overriding the Color Scheme.
    // FIXME: We don't currently support setting an "overriding preference" for color schemes.

    // 3. Otherwise, if the user agent supports at least one of the listed color schemes,
    //    the used color scheme is the first supported color scheme in the list.
    auto first_supported = schemes.first_matching([](auto scheme) { return preferred_color_scheme_from_string(scheme) != PreferredColorScheme::Auto; });
    if (first_supported.has_value())
        return preferred_color_scheme_from_string(first_supported.value());

    // 4. Otherwise, the used color scheme is the browser default. (Same as normal.)
    // `normal` indicates that the element supports the page’s supported color schemes, if they are set
    if (document_supported_schemes.has_value()) {
        if (preferred_scheme != PreferredColorScheme::Auto && document_supported_schemes->contains_slow(preferred_color_scheme_to_utf16_fly_string(preferred_scheme)))
            return preferred_scheme;

        auto document_first_supported = document_supported_schemes->first_matching([](auto scheme) { return preferred_color_scheme_from_string(scheme) != PreferredColorScheme::Auto; });
        if (document_first_supported.has_value())
            return preferred_color_scheme_from_string(document_first_supported.value());
    }

    return PreferredColorScheme::Light;
}

NonnullRefPtr<Gfx::Font const> ComputedProperties::font_fallback(bool monospace, bool bold, float point_size)
{
    if (monospace && bold)
        return Platform::FontPlugin::the().default_fixed_width_font().bold_variant();

    if (monospace)
        return Platform::FontPlugin::the().default_fixed_width_font();

    if (bold)
        return Platform::FontPlugin::the().default_font(point_size)->bold_variant();

    return *Platform::FontPlugin::the().default_font(point_size);
}

CSSPixels ComputedProperties::normal_line_height(Gfx::FontPixelMetrics const& font_metrics)
{
    return CSSPixels { round_to<i32>(font_metrics.ascent) + round_to<i32>(font_metrics.descent) };
}

CSSPixels ComputedProperties::line_height(FontComputer const& font_computer) const
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

LineHeightData ComputedProperties::line_height_data() const
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

Optional<int> ComputedProperties::z_index() const
{
    auto const& value = property(PropertyID::ZIndex);
    if (value.has_auto())
        return {};

    return int_from_style_value(value);
}

float ComputedProperties::opacity() const
{
    return property(PropertyID::Opacity).as_opacity_value().resolved();
}

float ComputedProperties::stop_opacity() const
{
    return property(PropertyID::StopOpacity).as_opacity_value().resolved();
}

float ComputedProperties::flood_opacity() const
{
    return property(PropertyID::FloodOpacity).as_opacity_value().resolved();
}

double ComputedProperties::flex_grow() const
{
    auto const& value = property(PropertyID::FlexGrow);
    return number_from_style_value(NonnullRefPtr<StyleValue const> { value }, {});
}

double ComputedProperties::flex_shrink() const
{
    auto const& value = property(PropertyID::FlexShrink);
    return number_from_style_value(NonnullRefPtr<StyleValue const> { value }, {});
}

i32 ComputedProperties::order() const
{
    auto const& value = property(PropertyID::Order);
    return int_from_style_value(NonnullRefPtr<StyleValue const> { value });
}

ImageRendering ComputedProperties::image_rendering() const
{
    auto const& value = property(PropertyID::ImageRendering);
    return keyword_to_image_rendering(value.to_keyword()).release_value();
}

CSSPixels ComputedProperties::border_spacing_horizontal() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 0);
        return Length::from_style_value(list.value_at(0, false), {}).absolute_length_to_px();
    }

    return Length::from_style_value(style_value, {}).absolute_length_to_px();
}

CSSPixels ComputedProperties::border_spacing_vertical() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 1);
        return Length::from_style_value(list.value_at(1, false), {}).absolute_length_to_px();
    }

    return Length::from_style_value(style_value, {}).absolute_length_to_px();
}

CaptionSide ComputedProperties::caption_side() const
{
    auto const& value = property(PropertyID::CaptionSide);
    return keyword_to_caption_side(value.to_keyword()).release_value();
}

Vector<NonnullRefPtr<TransformationStyleValue const>> ComputedProperties::transformations() const
{
    return transformations_for_style_value(property(PropertyID::Transform));
}

RefPtr<TransformationStyleValue const> ComputedProperties::rotate() const
{
    auto const& value = property(PropertyID::Rotate);
    if (!value.is_transformation())
        return {};
    return value.as_transformation();
}

RefPtr<TransformationStyleValue const> ComputedProperties::translate() const
{
    auto const& value = property(PropertyID::Translate);
    if (!value.is_transformation())
        return {};
    return value.as_transformation();
}

RefPtr<TransformationStyleValue const> ComputedProperties::scale() const
{
    auto const& value = property(PropertyID::Scale);
    if (!value.is_transformation())
        return {};
    return value.as_transformation();
}

TransformBox ComputedProperties::transform_box() const
{
    auto const& value = property(PropertyID::TransformBox);
    return keyword_to_transform_box(value.to_keyword()).release_value();
}

Optional<CSSPixels> ComputedProperties::perspective() const
{
    auto const& value = property(PropertyID::Perspective);
    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return {};

    return Length::from_style_value(value, {}).absolute_length_to_px();
}

Position ComputedProperties::perspective_origin() const
{
    return position_value(PropertyID::PerspectiveOrigin);
}

TransformOrigin ComputedProperties::transform_origin() const
{
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

    auto const& value = property(PropertyID::TransformOrigin);
    if (!value.is_value_list() || value.as_value_list().size() != 3)
        return {};
    auto const& list = value.as_value_list();

    auto x_value = length_percentage_with_keywords_resolved(list.values()[0]);
    auto y_value = length_percentage_with_keywords_resolved(list.values()[1]);
    auto z_value = LengthPercentage::from_style_value(list.values()[2]);
    return { x_value, y_value, z_value };
}

TransformStyle ComputedProperties::transform_style() const
{
    auto const& value = property(PropertyID::TransformStyle);
    return keyword_to_transform_style(value.to_keyword()).release_value();
}

BackfaceVisibility ComputedProperties::backface_visibility() const
{
    auto const& value = property(PropertyID::BackfaceVisibility);
    return keyword_to_backface_visibility(value.to_keyword()).release_value();
}

Color ComputedProperties::accent_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::AccentColor);

    if (value.to_keyword() == Keyword::Auto)
        return CSS::SystemColor::accent_color(color_resolution_context.color_scheme.value());

    return value.to_color(color_resolution_context).value();
}

Filter ComputedProperties::backdrop_filter() const
{
    auto const& value = property(PropertyID::BackdropFilter);
    if (is_filter_style_value_list(value))
        return Filter(value.as_value_list());
    return Filter::make_none();
}

Filter ComputedProperties::filter() const
{
    auto const& value = property(PropertyID::Filter);
    if (is_filter_style_value_list(value))
        return Filter(value.as_value_list());
    return Filter::make_none();
}

Positioning ComputedProperties::position() const
{
    auto const& value = property(PropertyID::Position);
    return keyword_to_positioning(value.to_keyword()).release_value();
}

TextOverflow ComputedProperties::text_overflow() const
{
    auto const& value = property(PropertyID::TextOverflow);
    return keyword_to_text_overflow(value.to_keyword()).release_value();
}

TextRendering ComputedProperties::text_rendering() const
{
    auto const& value = property(PropertyID::TextRendering);
    return keyword_to_text_rendering(value.to_keyword()).release_value();
}

CSSPixels ComputedProperties::text_underline_offset() const
{
    auto const& computed_text_underline_offset = property(PropertyID::TextUnderlineOffset);

    // auto
    if (computed_text_underline_offset.to_keyword() == Keyword::Auto)
        return InitialValues::text_underline_offset();

    // <length>
    // <percentage>
    return Length::from_style_value(computed_text_underline_offset, Length::make_px(font_size())).absolute_length_to_px();
}

CSSPixels ComputedProperties::word_spacing() const
{
    auto const& value = property(PropertyID::WordSpacing);
    if (value.is_keyword() && value.to_keyword() == Keyword::Normal)
        return 0;

    return Length::from_style_value(value, Length::make_px(font_size())).absolute_length_to_px();
}

WhiteSpaceTrimData ComputedProperties::white_space_trim() const
{
    auto const& value = property(PropertyID::WhiteSpaceTrim);

    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return WhiteSpaceTrimData {};

    if (value.is_value_list()) {
        auto white_space_trim_data = WhiteSpaceTrimData {};

        for (auto const& value : value.as_value_list().values()) {
            switch (value->as_keyword().keyword()) {
            case Keyword::DiscardBefore:
                white_space_trim_data.discard_before = true;
                break;
            case Keyword::DiscardAfter:
                white_space_trim_data.discard_after = true;
                break;
            case Keyword::DiscardInner:
                white_space_trim_data.discard_inner = true;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        return white_space_trim_data;
    }

    VERIFY_NOT_REACHED();
}

CSSPixels ComputedProperties::letter_spacing() const
{
    auto const& value = property(PropertyID::LetterSpacing);
    if (value.is_keyword() && value.to_keyword() == Keyword::Normal)
        return 0;

    return Length::from_style_value(value, Length::make_px(font_size())).absolute_length_to_px();
}

Float ComputedProperties::float_() const
{
    auto const& value = property(PropertyID::Float);
    return keyword_to_float(value.to_keyword()).release_value();
}

Color ComputedProperties::caret_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::CaretColor);
    if (value.is_keyword() && value.to_keyword() == Keyword::Auto)
        return color_resolution_context.current_color.value_or(InitialValues::color());

    if (value.has_color())
        return value.to_color(color_resolution_context).value();

    return InitialValues::caret_color();
}

Clear ComputedProperties::clear() const
{
    auto const& value = property(PropertyID::Clear);
    return keyword_to_clear(value.to_keyword()).release_value();
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

ContentVisibility ComputedProperties::content_visibility() const
{
    auto const& value = property(PropertyID::ContentVisibility);
    return keyword_to_content_visibility(value.to_keyword()).release_value();
}

Visibility ComputedProperties::visibility() const
{
    auto const& value = property(PropertyID::Visibility);
    if (!value.is_keyword())
        return {};
    return keyword_to_visibility(value.to_keyword()).release_value();
}

Display ComputedProperties::display() const
{
    return property(PropertyID::Display).as_display().display();
}

Vector<TextDecorationLine> ComputedProperties::text_decoration_line() const
{
    auto const& value = property(PropertyID::TextDecorationLine);

    if (value.to_keyword() == Keyword::None)
        return {};

    if (value.is_value_list()) {
        Vector<TextDecorationLine> lines;
        auto values = value.as_value_list().values();
        for (auto const& item : values) {
            lines.append(keyword_to_text_decoration_line(item->to_keyword()).value());
        }
        return lines;
    }

    VERIFY_NOT_REACHED();
}

TextDecorationStyle ComputedProperties::text_decoration_style() const
{
    auto const& value = property(PropertyID::TextDecorationStyle);
    return keyword_to_text_decoration_style(value.to_keyword()).release_value();
}

TextDecorationThickness ComputedProperties::text_decoration_thickness() const
{
    auto const& value = property(PropertyID::TextDecorationThickness);
    if (value.is_keyword()) {
        switch (value.to_keyword()) {
        case Keyword::Auto:
            return { TextDecorationThickness::Auto {} };
        case Keyword::FromFont:
            return { TextDecorationThickness::FromFont {} };
        default:
            VERIFY_NOT_REACHED();
        }
    }

    return TextDecorationThickness { LengthPercentage::from_style_value(value) };
}

ListStyleType ComputedProperties::list_style_type(StyleScope const& style_scope) const
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
    return value.as_counter_style().value().get<Utf16FlyString>();
}

Overflow ComputedProperties::overflow_x() const
{
    return overflow(PropertyID::OverflowX);
}

Overflow ComputedProperties::overflow_y() const
{
    return overflow(PropertyID::OverflowY);
}

Overflow ComputedProperties::overflow(PropertyID property_id) const
{
    auto const& value = property(property_id);
    return keyword_to_overflow(value.to_keyword()).release_value();
}

BoxSizing ComputedProperties::box_sizing() const
{
    auto const& value = property(PropertyID::BoxSizing);
    return keyword_to_box_sizing(value.to_keyword()).release_value();
}

Variant<VerticalAlign, LengthPercentage> ComputedProperties::vertical_align() const
{
    auto const& value = property(PropertyID::VerticalAlign);

    if (value.is_keyword())
        return keyword_to_vertical_align(value.to_keyword()).release_value();

    return LengthPercentage::from_style_value(value);
}

FontKerning ComputedProperties::font_kerning() const
{
    auto const& value = property(PropertyID::FontKerning);
    return keyword_to_font_kerning(value.to_keyword()).release_value();
}

Optional<Utf16FlyString> ComputedProperties::font_language_override() const
{
    auto const& value = property(PropertyID::FontLanguageOverride);
    if (value.is_string())
        return value.as_string().string_value();
    return {};
}

FontFeatureData ComputedProperties::font_feature_data() const
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

Optional<FontVariantAlternates> ComputedProperties::font_variant_alternates() const
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

FontVariantCaps ComputedProperties::font_variant_caps() const
{
    auto const& value = property(PropertyID::FontVariantCaps);
    return keyword_to_font_variant_caps(value.to_keyword()).release_value();
}

Optional<FontVariantEastAsian> ComputedProperties::font_variant_east_asian() const
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

FontVariantEmoji ComputedProperties::font_variant_emoji() const
{
    auto const& value = property(PropertyID::FontVariantEmoji);
    return keyword_to_font_variant_emoji(value.to_keyword()).release_value();
}

Optional<FontVariantLigatures> ComputedProperties::font_variant_ligatures() const
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

Optional<FontVariantNumeric> ComputedProperties::font_variant_numeric() const
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

FontVariantPosition ComputedProperties::font_variant_position() const
{
    auto const& value = property(PropertyID::FontVariantPosition);
    return keyword_to_font_variant_position(value.to_keyword()).release_value();
}

HashMap<Utf16FlyString, u8> ComputedProperties::font_feature_settings() const
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

HashMap<Utf16FlyString, double> ComputedProperties::font_variation_settings() const
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

GridAutoFlow ComputedProperties::grid_auto_flow() const
{
    auto const& value = property(PropertyID::GridAutoFlow);
    if (!value.is_grid_auto_flow())
        return GridAutoFlow {};
    auto& grid_auto_flow_value = value.as_grid_auto_flow();
    return GridAutoFlow { .row = grid_auto_flow_value.is_row(), .dense = grid_auto_flow_value.is_dense() };
}

BorderCollapse ComputedProperties::border_collapse() const
{
    auto const& value = property(PropertyID::BorderCollapse);
    return keyword_to_border_collapse(value.to_keyword()).release_value();
}

EmptyCells ComputedProperties::empty_cells() const
{
    auto const& value = property(PropertyID::EmptyCells);
    return keyword_to_empty_cells(value.to_keyword()).release_value();
}

TableLayout ComputedProperties::table_layout() const
{
    auto const& value = property(PropertyID::TableLayout);
    return keyword_to_table_layout(value.to_keyword()).release_value();
}

Direction ComputedProperties::direction() const
{
    auto const& value = property(PropertyID::Direction);
    return keyword_to_direction(value.to_keyword()).release_value();
}

UnicodeBidi ComputedProperties::unicode_bidi() const
{
    auto const& value = property(PropertyID::UnicodeBidi);
    return keyword_to_unicode_bidi(value.to_keyword()).release_value();
}

WritingMode ComputedProperties::writing_mode() const
{
    auto const& value = property(PropertyID::WritingMode);
    return keyword_to_writing_mode(value.to_keyword()).release_value();
}

Isolation ComputedProperties::isolation() const
{
    auto const& value = property(PropertyID::Isolation);
    return keyword_to_isolation(value.to_keyword()).release_value();
}

AspectRatio ComputedProperties::aspect_ratio() const
{
    auto const& value = property(PropertyID::AspectRatio);

    if (value.is_value_list()) {
        auto const& values = value.as_value_list().values();
        if (values.size() == 2
            && values[0]->is_keyword() && values[0]->as_keyword().keyword() == Keyword::Auto
            && values[1]->is_ratio()) {
            auto ratio = values[1]->as_ratio().resolved();
            if (ratio.is_degenerate())
                return { true, {}, true, ratio };
            return { true, ratio, true, ratio };
        }
        return InitialValues::aspect_ratio();
    }

    if (value.is_ratio()) {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio
        // If the <ratio> is degenerate, the property instead behaves as auto.
        auto ratio = value.as_ratio().resolved();
        if (ratio.is_degenerate())
            return { true, {}, false, ratio };
        return { false, ratio, false, ratio };
    }

    return InitialValues::aspect_ratio();
}

Containment ComputedProperties::contain() const
{
    Containment containment = {};
    auto const& value = property(PropertyID::Contain);

    switch (value.to_keyword()) {
    case Keyword::None:
        // This value indicates that the property has no effect. The element renders as normal, with no containment effects applied.
        return {};
    case Keyword::Strict:
        // This value computes to 'size layout paint style', and thus turns on all forms of containment for the element.
        containment.size_containment = true;
        containment.layout_containment = true;
        containment.paint_containment = true;
        containment.style_containment = true;
        break;
    case Keyword::Content:
        //  This value computes to 'layout paint style', and thus turns on all forms of containment except size containment for the element.
        containment.layout_containment = true;
        containment.paint_containment = true;
        containment.style_containment = true;
        break;
    case Keyword::Size:
        containment.size_containment = true;
        break;
    case Keyword::InlineSize:
        containment.inline_size_containment = true;
        break;
    case Keyword::Layout:
        containment.layout_containment = true;
        break;
    case Keyword::Style:
        containment.style_containment = true;
        break;
    case Keyword::Paint:
        containment.paint_containment = true;
        break;
    default:
        if (value.is_value_list()) {
            auto values = value.as_value_list().values();
            for (auto const& item : values) {
                switch (item->to_keyword()) {
                case Keyword::Size:
                    containment.size_containment = true;
                    break;
                case Keyword::InlineSize:
                    containment.inline_size_containment = true;
                    break;
                case Keyword::Layout:
                    containment.layout_containment = true;
                    break;
                case Keyword::Style:
                    containment.style_containment = true;
                    break;
                case Keyword::Paint:
                    containment.paint_containment = true;
                    break;
                default:
                    dbgln("`{}` is not supported in `contain` (yet?)", item->to_string(SerializationMode::Normal));
                    break;
                }
            }
        }
    }

    return containment;
}

Vector<Utf16FlyString> ComputedProperties::container_name() const
{
    auto const& value = property(PropertyID::ContainerName);
    if (value.to_keyword() == Keyword::None)
        return {};

    Vector<Utf16FlyString> names;

    if (value.is_value_list()) {
        auto values = value.as_value_list().values();
        for (auto const& item : values)
            names.append(item->as_custom_ident().custom_ident());
    } else {
        names.append(value.as_custom_ident().custom_ident());
    }

    return names;
}

ContainerType ComputedProperties::container_type() const
{
    ContainerType container_type {};

    auto const& value = property(PropertyID::ContainerType);

    if (value.to_keyword() == Keyword::Normal)
        return container_type;

    if (value.is_value_list()) {
        auto values = value.as_value_list().values();
        for (auto const& item : values) {
            switch (item->to_keyword()) {
            case Keyword::Size:
                container_type.is_size_container = true;
                break;
            case Keyword::InlineSize:
                container_type.is_inline_size_container = true;
                break;
            case Keyword::ScrollState:
                container_type.is_scroll_state_container = true;
                break;
            default:
                dbgln("`{}` is not supported in `container-type` (yet?)", item->to_string(SerializationMode::Normal));
                break;
            }
        }
    }

    return container_type;
}

MixBlendMode ComputedProperties::mix_blend_mode() const
{
    auto const& value = property(PropertyID::MixBlendMode);
    return keyword_to_mix_blend_mode(value.to_keyword()).release_value();
}

Vector<AnimationProperties> ComputedProperties::animations(DOM::AbstractElement const& abstract_element) const
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
            //        referenced timeline as defined in Scroll-driven Animations §  Declaring a Named Timeline’s Scope:
            //        the timeline-scope property. Otherwise the animation is not associated with a timeline.

            // <scroll()>
            // Use the scroll progress timeline indicated by the given scroll() function. See Scroll-driven Animations
            // § 2.2.1 The scroll() notation.
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
            //        Animations § 3.3.1 The view() notation.

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

Vector<TransitionProperties> ComputedProperties::transitions() const
{
    auto const& coordinated_properties = assemble_coordinated_value_list(
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

ScrollbarColorData ComputedProperties::scrollbar_color(ColorResolutionContext const& color_resolution_context) const
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

Resize ComputedProperties::resize() const
{
    auto const& value = property(PropertyID::Resize);
    return keyword_to_resize(value.to_keyword()).release_value();
}

ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> ComputedProperties::computed_font_list(FontComputer const& font_computer) const
{
    if (!m_cached_computed_font_list) {
        m_cached_computed_font_list = font_computer.compute_font_for_style_values(property(PropertyID::FontFamily), font_size(), font_slope(), font_weight(), font_width(), font_optical_sizing(), font_variation_settings(), font_feature_data());
        VERIFY(!m_cached_computed_font_list->is_empty());
    }

    return *m_cached_computed_font_list;
}

ValueComparingNonnullRefPtr<Gfx::Font const> ComputedProperties::first_available_computed_font(FontComputer const& font_computer) const
{
    if (!m_cached_first_available_computed_font) {
        // https://drafts.csswg.org/css-fonts/#first-available-font
        // First font for which the character U+0020 (space) is not excluded by a unicode-range
        m_cached_first_available_computed_font = computed_font_list(font_computer)->font_for_code_point(' ');
    }

    return *m_cached_first_available_computed_font;
}

int ComputedProperties::math_depth() const
{
    return property(PropertyID::MathDepth).as_integer().integer();
}

CSSPixels ComputedProperties::font_size() const
{
    return property(PropertyID::FontSize).as_length().length().absolute_length_to_px();
}

double ComputedProperties::font_weight() const
{
    return property(PropertyID::FontWeight).as_number().number();
}

Percentage ComputedProperties::font_width() const
{
    return property(PropertyID::FontWidth).as_percentage().percentage();
}

int ComputedProperties::font_slope() const
{
    return property(PropertyID::FontStyle).as_font_style().to_font_slope();
}

FontOpticalSizing ComputedProperties::font_optical_sizing() const
{
    auto const& value = property(PropertyID::FontOpticalSizing);
    return keyword_to_font_optical_sizing(value.to_keyword()).release_value();
}

}
