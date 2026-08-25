/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

extern "C" void ladybird_animated_properties_ref(void const*);

static void ladybird_utf16_fly_string_ref_raw(size_t raw)
{
    Utf16FlyString::ref_raw(static_cast<FlatPtr>(raw));
}

static void ladybird_utf16_fly_string_unref_raw(size_t raw)
{
    Utf16FlyString::unref_raw(static_cast<FlatPtr>(raw));
}

static StyleEngineFFI::FfiResolvedFont resolve_font(void* context, StyleEngineFFI::FfiFontResolutionRequest request)
{
    auto& style_computer = *static_cast<StyleComputer*>(context);
    auto& font_computer = style_computer.document().font_computer();
    auto font_family = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
        static_cast<StyleValueFFI::StyleValueData const*>(request.font_family)));
    auto font_list = font_computer.compute_font_for_style_values(
        *font_family,
        CSSPixels::from_raw(request.font_size_raw),
        request.font_slope,
        request.font_weight,
        Percentage(request.font_width),
        static_cast<FontOpticalSizing>(request.font_optical_sizing),
        {},
        {});
    font_computer.pin_font_list_for_style_record(font_list);
    auto const& first_available_font = font_list->font_for_code_point(' ');
    auto const metrics = first_available_font.pixel_metrics();
    auto* handle = &font_list.leak_ref();
    return {
        .handle = handle,
        .first_available_font = &first_available_font,
        .font_cascade_list = handle,
        .ascent = metrics.ascent,
        .descent = metrics.descent,
        .x_height = metrics.x_height,
    };
}

static void retain_resolved_font(void const* handle)
{
    static_cast<Gfx::FontCascadeList const*>(handle)->ref();
}

static void release_resolved_font(void const* handle)
{
    static_cast<Gfx::FontCascadeList const*>(handle)->unref();
}

static_assert(!IsMoveConstructible<StyleEngine>);
static_assert(!IsMoveAssignable<StyleEngine>);

#include <LibWeb/StyleEngineBridgeGenerated.inc>

bool StyleEngine::published_style_delta_can_absorb_reaction(PublishedStyleDelta const& delta, u8 reaction, u8 inherited_style_groups)
{
    if (delta.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize)
        return true;
    bool const has_additional_reaction = (reaction & ~delta.reaction) != 0;
    bool const has_additional_inherited_style_groups = (inherited_style_groups & ~delta.inherited_style_groups) != 0;
    return !has_additional_reaction && !has_additional_inherited_style_groups;
}

StyleEngine::StyleEngine(DeviceClass device_class, StyleComputer* style_computer)
    : m_impl(StyleEngineFFI::style_engine_create(device_class))
    , m_style_computer(style_computer)
{
    StyleEngineFFI::style_engine_install_raw_atom_callbacks(ladybird_utf16_fly_string_ref_raw, ladybird_utf16_fly_string_unref_raw);
    if (m_style_computer) {
        StyleEngineFFI::style_engine_install_font_resolver(
            m_impl,
            m_style_computer.ptr(),
            resolve_font,
            retain_resolved_font,
            release_resolved_font);
    }
}

StyleEngine::~StyleEngine()
{
    if (m_impl)
        StyleEngineFFI::style_engine_destroy(m_impl);
    for (auto const& atom : m_atoms)
        Utf16FlyString::unref_raw(atom.key);
}

void StyleEngine::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_style_computer);
}

StyleNodeID StyleEngine::allocate_style_node()
{
    StyleNodeID node;
    allocate_style_nodes({ &node, 1 });
    return node;
}

void StyleEngine::allocate_style_nodes(Span<StyleNodeID> nodes)
{
    if (nodes.is_empty())
        return;
    StyleEngineFFI::style_engine_allocate_style_nodes(m_impl, reinterpret_cast<u32*>(nodes.data()), nodes.size());
}

HashTable<StyleNodeID> StyleEngine::take_deferred_element_initial_features()
{
    return move(m_nodes_with_pending_initial_features);
}

HashTable<StyleNodeID> StyleEngine::take_elements_awaiting_first_style_computation()
{
    return move(m_nodes_awaiting_first_style_computation);
}

bool StyleEngine::resize_parsed_substitution_cache(u64 bytes)
{
    return StyleEngineFFI::style_engine_resize_parsed_substitution_cache(m_impl, bytes);
}

void StyleEngine::set_element_parts(StyleNodeID node, ReadonlySpan<StyleAtomID> names, ReadonlySpan<StyleNodeID> hosts)
{
    VERIFY(names.size() == hosts.size());
    StyleEngineFFI::style_engine_set_element_parts(m_impl, node.value(), reinterpret_cast<u32 const*>(names.data()), reinterpret_cast<u32 const*>(hosts.data()), names.size());
}

StyleEngineRuleID StyleEngine::add_style_rule(SheetID sheet, StyleEngineRuleID before_rule, ReadonlySpan<void const*> selectors, NamespaceScope const& namespaces, ReadonlySpan<void const*> scope_roots, ReadonlySpan<void const*> scope_limits, ScopeLevels const& scope_levels)
{
    VERIFY(namespaces.prefixes.size() == namespaces.uris.size());
    VERIFY(scope_levels.root_counts.size() == scope_levels.limit_counts.size());
    VERIFY(scope_levels.root_counts.size() == scope_levels.implicit_roots.size());
    return StyleEngineRuleID { StyleEngineFFI::style_engine_add_style_rule(
        m_impl,
        sheet.value(),
        before_rule.value(),
        selectors.data(),
        selectors.size(),
        namespaces.default_namespace.value(),
        reinterpret_cast<u32 const*>(namespaces.prefixes.data()),
        reinterpret_cast<u32 const*>(namespaces.uris.data()),
        namespaces.prefixes.size(),
        scope_roots.data(),
        scope_roots.size(),
        scope_limits.data(),
        scope_limits.size(),
        scope_levels.root_counts.data(),
        scope_levels.limit_counts.data(),
        reinterpret_cast<u32 const*>(scope_levels.implicit_roots.data()),
        scope_levels.root_counts.size()) };
}

void StyleEngine::replace_style_rule_selectors(StyleEngineRuleID rule, ReadonlySpan<void const*> selectors, NamespaceScope const& namespaces, ReadonlySpan<void const*> scope_roots, ReadonlySpan<void const*> scope_limits, ScopeLevels const& scope_levels)
{
    VERIFY(namespaces.prefixes.size() == namespaces.uris.size());
    VERIFY(scope_levels.root_counts.size() == scope_levels.limit_counts.size());
    VERIFY(scope_levels.root_counts.size() == scope_levels.implicit_roots.size());
    StyleEngineFFI::style_engine_replace_style_rule_selectors(
        m_impl,
        rule.value(),
        selectors.data(),
        selectors.size(),
        namespaces.default_namespace.value(),
        reinterpret_cast<u32 const*>(namespaces.prefixes.data()),
        reinterpret_cast<u32 const*>(namespaces.uris.data()),
        namespaces.prefixes.size(),
        scope_roots.data(),
        scope_roots.size(),
        scope_limits.data(),
        scope_limits.size(),
        scope_levels.root_counts.data(),
        scope_levels.limit_counts.data(),
        reinterpret_cast<u32 const*>(scope_levels.implicit_roots.data()),
        scope_levels.root_counts.size());
}

void StyleEngine::finish_sheet_rules_replacement(SheetID sheet)
{
    StyleEngineFFI::style_engine_finish_sheet_rules_replacement(m_impl, sheet.value(), next_declaration_block_version());
}

void StyleEngine::set_rule_declared_properties(StyleEngineRuleID rule, ReadonlySpan<u16> properties, ReadonlySpan<bool> important, ReadonlySpan<StyleEngineFFI::FfiCascadeOperator> operators, ReadonlySpan<void const*> values, ReadonlySpan<void const*> original_values, bool declarations_are_complete)
{
    VERIFY(properties.size() == important.size());
    VERIFY(properties.size() == operators.size());
    VERIFY(properties.size() == values.size());
    VERIFY(properties.size() == original_values.size());
    StyleEngineFFI::style_engine_set_rule_declared_properties(m_impl, rule.value(), properties.data(), important.data(), operators.data(), values.data(), original_values.data(), properties.size(), declarations_are_complete);
}

void StyleEngine::set_element_declared_properties(StyleNodeID node, StyleEngineFFI::FfiElementDeclarationKind kind, ReadonlySpan<u16> properties, ReadonlySpan<bool> important, ReadonlySpan<StyleEngineFFI::FfiCascadeOperator> operators, ReadonlySpan<void const*> values, ReadonlySpan<void const*> original_values, bool declarations_are_complete)
{
    VERIFY(properties.size() == important.size());
    VERIFY(properties.size() == operators.size());
    VERIFY(properties.size() == values.size());
    VERIFY(properties.size() == original_values.size());
    StyleEngineFFI::style_engine_set_element_declared_properties(m_impl, node.value(), kind, properties.data(), important.data(), operators.data(), values.data(), original_values.data(), properties.size(), declarations_are_complete);
}

StyleEngine::ExactCascadePublication StyleEngine::publish_exact_cascade_state(StyleNodeID node, u8 pseudo_kind, ComputedValuesFFI::CascadedPropertyStore const* store, u8 inherited_style_groups)
{
    return StyleEngineFFI::style_engine_publish_exact_cascade_state(m_impl, node.value(), pseudo_kind, store, inherited_style_groups);
}

ReadonlySpan<ComputedValuesFFI::FfiSourceSlotAssignment> StyleEngine::materialize_retained_cascade_state(StyleNodeID node, u8 pseudo_kind, ComputedValuesFFI::CascadedPropertyStore* store, ReadonlySpan<ComputedValuesFFI::FfiCascadeBlock> blocks)
{
    auto view = StyleEngineFFI::style_engine_materialize_retained_cascade_state(m_impl, node.value(), pseudo_kind, store, blocks.data(), blocks.size());
    return { static_cast<ComputedValuesFFI::FfiSourceSlotAssignment const*>(view.assignments), view.count };
}

void StyleEngine::discard_retained_cascade_assignments()
{
    StyleEngineFFI::style_engine_discard_retained_cascade_assignments(m_impl);
}

StyleEngine::StyleRecordDelta StyleEngine::publish_computed_groups(StyleNodeID node, u8 pseudo_kind, ReadonlySpan<void const*> payloads, size_t inherited_group_count, u64 custom_property_environment, u64 pseudo_element_styles, u8 dependency_flags, u64 counter_style_environment_identity, u64 animation_overlay_identity, void const* animated_properties, ReadonlySpan<void const*> animation_overlay_payloads, ReadonlyBytes property_importance, ReadonlyBytes property_inheritance, ReadonlySpan<u16> inheritance_dependent_properties, ReadonlySpan<void const*> inheritance_dependent_values, void const* raw_cascaded_font_size, void const* computed_longhand_table)
{
    VERIFY(inherited_group_count <= payloads.size());
    VERIFY(inheritance_dependent_properties.size() == inheritance_dependent_values.size());
    if (animated_properties)
        ladybird_animated_properties_ref(animated_properties);
    auto delta = StyleEngineFFI::style_engine_publish_computed_groups(m_impl, node.value(), pseudo_kind, payloads.data(), payloads.size(), inherited_group_count, custom_property_environment, pseudo_element_styles, dependency_flags, counter_style_environment_identity, animation_overlay_identity, animated_properties, animation_overlay_payloads.data(), animation_overlay_payloads.size(), property_importance.data(), property_importance.size(), property_inheritance.data(), property_inheritance.size(), inheritance_dependent_properties.data(), inheritance_dependent_values.data(), inheritance_dependent_properties.size(), raw_cascaded_font_size, computed_longhand_table);
    return { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

StyleEngine::StyleRecordDelta StyleEngine::assign_shared_style_record(StyleNodeID node, u8 pseudo_kind, StyleRecordID style_record, bool inherited_group_swap_eligible)
{
    auto delta = StyleEngineFFI::style_engine_assign_shared_style_record(m_impl, node.value(), pseudo_kind, style_record.value(), ComputedValues::inherited_style_group_count, inherited_group_swap_eligible);
    return { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

void const* StyleEngine::style_record_payloads(StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_style_record_payloads(m_impl, style_record.value());
}

StyleEngine::StyleRecordState StyleEngine::style_record_state(StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_style_record_state(m_impl, style_record.value());
}

StyleEngine::StyleRecordView StyleEngine::style_record_view(StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_style_record_view(m_impl, style_record.value());
}

StyleEngine::StyleRecordDelta StyleEngine::remove_computed_pseudo(StyleNodeID node, u8 pseudo_kind)
{
    auto delta = StyleEngineFFI::style_engine_remove_computed_pseudo(m_impl, node.value(), pseudo_kind);
    return { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

StyleAtomID StyleEngine::intern_atom(Utf16FlyString const& name)
{
    // Utf16FlyString is already interned, so its one-word raw form is the name's identity. The atom
    // itself is assigned by the engine, which is also where selector names intern: two tables keyed
    // by the same word but each assigning its own sequence would compare unequal for the same name,
    // which fails to match silently rather than loudly.
    // First time seen, the leaked reference is kept so the identity cannot be reused while the
    // atom is live. Duplicates release their new reference and return without crossing the FFI.
    auto raw = name.to_raw_leaked();
    if (auto atom = m_atoms.get(raw); atom.has_value()) {
        Utf16FlyString::unref_raw(raw);
        return atom.release_value();
    }
    auto atom = StyleAtomID { StyleEngineFFI::style_engine_intern_atom(m_impl, raw) };
    m_atoms.set(raw, atom);
    return atom;
}

StyleAtomID StyleEngine::intern_text_atom(Utf16View text)
{
    return intern_atom(Utf16FlyString::from_utf16(text).to_ascii_lowercase());
}

StyleAtomID StyleEngine::intern_language_atom(Utf16View text)
{
    auto atom = intern_text_atom(text);
    if (atom == 0 || text.is_empty() || m_published_language_atoms.set(atom) != AK::HashSetResult::InsertedNewEntry)
        return atom;

    Vector<u16> code_units;
    code_units.ensure_capacity(text.length_in_code_units());
    for (size_t i = 0; i < text.length_in_code_units(); ++i)
        code_units.unchecked_append(text.code_unit_at(i));
    StyleEngineFFI::style_engine_set_element_language(m_impl, 0, atom.value(), code_units.data(), code_units.size());
    return atom;
}

StyleAtomID StyleEngine::intern_case_sensitive_text_atom(Utf16View text)
{
    return intern_atom(Utf16FlyString::from_utf16(text));
}

// The name an attribute is published under, and the any-namespace name it shares.
//
// Three selectors ask three different questions of an attribute called `x`. `[ns|x]` reaches only
// the one in that namespace, `[x]` reaches only the one in no namespace - which is what the bare
// local name is - and `[*|x]` reaches whichever of them the element carries. The first two name
// exactly one of an element's attributes, so they are the key: an element can hold `x` in several
// namespaces at once, and each is a fact with its own value. `[*|x]` asks about all of them
// together, so the shared form is published as an identity of the name rather than as a fact of its
// own, and one entry per attribute answers all three.
StyleAtomID StyleEngine::intern_attribute_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_uri)
{
    auto local = intern_atom(local_name);
    auto namespace_atom = !namespace_uri.has_value() || namespace_uri->is_empty()
        ? StyleAtomID {}
        : intern_case_sensitive_text_atom(namespace_uri->view());
    auto& names_by_namespace = m_attribute_name_atoms.ensure(local, [] { return HashMap<StyleAtomID, StyleAtomID> {}; });
    if (auto name = names_by_namespace.get(namespace_atom); name.has_value())
        return name.release_value();

    auto in_namespace = [&](StyleAtomID name) {
        if (namespace_atom == 0)
            return name;
        return intern_qualified_atom(namespace_atom, name);
    };
    auto any_namespace = intern_qualified_atom(StyleEngine::any_namespace, local);
    auto name = in_namespace(local);

    StyleAtomID folded_name;
    StyleAtomID folded_local;
    if (auto folded = local_name.to_ascii_lowercase(); folded != local_name) {
        auto folded_atom = intern_atom(folded);
        folded_name = in_namespace(folded_atom);
        folded_local = intern_qualified_atom(StyleEngine::any_namespace, folded_atom);
    }

    note_attribute_name_forms(name, any_namespace, folded_name, folded_local);
    names_by_namespace.set(namespace_atom, name);
    return name;
}

StyleAtomID StyleEngine::intern_attribute_value(StyleAtomID name, Utf16View value)
{
    auto atom = intern_case_sensitive_text_atom(value);
    if (!attribute_name_requires_value_text(name))
        return atom;

    publish_attribute_value_text(atom, value);
    return atom;
}

void StyleEngine::backfill_attribute_value_text_if_required(StyleAtomID name, Utf16View value)
{
    if (!attribute_name_requires_value_text(name))
        return;

    auto atom = intern_case_sensitive_text_atom(value);
    publish_attribute_value_text(atom, value);
}

void StyleEngine::publish_attribute_value_text(StyleAtomID atom, Utf16View value)
{
    // The engine holds one copy of the text per currently used value. Ask whether it survived
    // reclamation before copying it out of the attribute's representation again.
    if (StyleEngineFFI::style_engine_has_attribute_value_text(m_impl, atom.value()))
        return;

    Vector<u16> code_units;
    code_units.ensure_capacity(value.length_in_code_units());
    for (size_t i = 0; i < value.length_in_code_units(); ++i)
        code_units.unchecked_append(value.code_unit_at(i));
    StyleEngineFFI::style_engine_set_attribute_value_text(m_impl, atom.value(), code_units.data(), code_units.size());
}

bool StyleEngine::refresh_attribute_value_text_requirements()
{
    auto version = StyleEngineFFI::style_engine_attribute_value_text_requirements_version(m_impl);
    if (version == m_attribute_value_text_requirements_version)
        return false;
    m_attribute_value_text_requirements_version = version;
    m_attribute_names_requiring_value_text.clear();
    return true;
}

bool StyleEngine::attribute_name_requires_value_text(StyleAtomID name)
{
    return m_attribute_names_requiring_value_text.ensure(name, [&] {
        return StyleEngineFFI::style_engine_attribute_name_requires_value_text(m_impl, name.value());
    });
}

void StyleEngine::set_element_language(StyleNodeID node, StyleAtomID language, Utf16View tag)
{
    // A language range is not a name, so `:lang()` compares against the tag itself rather than
    // against the atom. The text is recorded once per language, not once per element.
    Vector<u16> code_units;
    if (language != 0 && !tag.is_empty() && m_published_language_atoms.set(language) == AK::HashSetResult::InsertedNewEntry) {
        code_units.ensure_capacity(tag.length_in_code_units());
        for (size_t i = 0; i < tag.length_in_code_units(); ++i)
            code_units.unchecked_append(tag.code_unit_at(i));
    }
    StyleEngineFFI::style_engine_set_element_language(m_impl, node.value(), language.value(), code_units.data(), code_units.size());
}

// Recording input gives the next rendering update style work to do, but touches no layout tree
// and no paintable, so nothing else asks the page for a frame. On a quiet document a change made
// from a timer would otherwise sit unflushed indefinitely, and a transition it should start would
// not run until something unrelated woke the rendering loop. Only the first input needs the poke:
// the frame it schedules flushes everything recorded before it runs.
static void request_frame_for_first_recorded_input(StyleEngine const& style_engine, GC::Ptr<StyleComputer> style_computer)
{
    if (style_engine.has_recorded_input() || !style_computer)
        return;
    style_computer->document().page().client().request_frame();
}

static void flush_deferred_geometry_transaction_before_non_replayable_input(StyleEngine const& style_engine, GC::Ptr<StyleComputer> style_computer)
{
    if (style_computer && style_engine.has_deferred_geometry_transaction())
        style_computer->document().flush_deferred_style_change_event();
}

void StyleEngine::record_tree_delta(StyleEngineFFI::FfiTreeDelta const& delta)
{
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_tree_deltas.append(delta);
}

void StyleEngine::record_element_arrival(StyleEngineFFI::FfiElementArrival arrival, ReadonlySpan<StyleAtomID> custom_states)
{
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    VERIFY(m_arrival_custom_state_atoms.size() <= NumericLimits<u32>::max());
    VERIFY(custom_states.size() <= NumericLimits<u32>::max());
    VERIFY(m_arrival_custom_state_atoms.size() + custom_states.size() <= NumericLimits<u32>::max());
    arrival.custom_state_offset = static_cast<u32>(m_arrival_custom_state_atoms.size());
    arrival.custom_state_count = static_cast<u32>(custom_states.size());
    for (auto state : custom_states)
        m_arrival_custom_state_atoms.append(state.value());
    m_element_arrivals.append(arrival);
}

void StyleEngine::record_local_feature_delta(StyleEngineFFI::FfiLocalFeatureDelta const& delta)
{
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_local_feature_deltas.append(delta);
}

void StyleEngine::record_state_delta(StyleEngineFFI::FfiStateDelta const& delta)
{
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_state_deltas.append(delta);
}

void StyleEngine::record_element_declaration_delta(StyleEngineFFI::FfiElementDeclarationDelta const& delta)
{
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_element_declaration_deltas.append(delta);
}

void StyleEngine::append_or_merge_element_style_input(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (auto existing = m_element_style_input_indices.find(style_node); existing != m_element_style_input_indices.end()) {
        auto& input = m_element_style_inputs[existing->value];
        input.reaction |= reaction;
        input.inherited_style_groups |= inherited_style_groups;
        return;
    }

    m_element_style_input_indices.set(style_node, m_element_style_inputs.size());
    m_element_style_inputs.append({ style_node.value(), reaction, inherited_style_groups });
}

void StyleEngine::record_element_style_input_change(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node != 0 && reaction != 0) {
        flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
        request_frame_for_first_recorded_input(*this, m_style_computer);
        append_or_merge_element_style_input(style_node, reaction, inherited_style_groups);
    }
}

void StyleEngine::record_flat_tree_descendant_style_input_changes(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node == 0 || reaction == 0)
        return;

    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    // The relation columns must include every tree delta recorded before this derived action. The
    // descendants themselves stay in the C++ batch so the next transaction normalizes them with
    // every other feedback action from this stabilization pass.
    submit_recorded_input();
    request_frame_for_first_recorded_input(*this, m_style_computer);
    auto descendants = StyleEngineFFI::style_engine_flat_tree_descendants(m_impl, style_node.value());
    for (auto descendant : ReadonlySpan<u32> { descendants.nodes, descendants.count })
        append_or_merge_element_style_input(StyleNodeID { descendant }, reaction, inherited_style_groups);
    StyleEngineFFI::style_engine_discard_flat_tree_descendants(m_impl);
}

Vector<StyleNodeID> StyleEngine::viewport_dependent_style_nodes()
{
    auto view = StyleEngineFFI::style_engine_viewport_dependent_nodes(m_impl);
    Vector<StyleNodeID> nodes;
    nodes.ensure_capacity(view.count);
    for (auto node : ReadonlySpan<u32> { view.nodes, view.count })
        nodes.unchecked_append(StyleNodeID { node });
    StyleEngineFFI::style_engine_discard_viewport_dependent_nodes(m_impl);
    return nodes;
}

void StyleEngine::consume_recorded_element_style_input_change(StyleNodeID style_node)
{
    auto existing = m_element_style_input_indices.find(style_node);
    if (existing == m_element_style_input_indices.end())
        return;

    auto index = existing->value;
    VERIFY(index < m_element_style_inputs.size());
    m_element_style_input_indices.remove(style_node);
    auto last_input = m_element_style_inputs.take_last();
    if (index < m_element_style_inputs.size()) {
        m_element_style_inputs[index] = last_input;
        m_element_style_input_indices.set(StyleNodeID { last_input.style_node }, index);
    }
}

bool StyleEngine::has_recorded_element_style_input_change(StyleNodeID style_node) const
{
    return m_element_style_input_indices.contains(style_node);
}
void StyleEngine::record_benchmark_marker(Utf16View name)
{
    auto const* data = name.has_ascii_storage()
        ? static_cast<void const*>(name.bytes().data())
        : static_cast<void const*>(name.utf16_span().data());
    StyleEngineFFI::style_engine_record_benchmark_marker(m_impl, data, name.length_in_code_units(), name.has_ascii_storage());
}

bool StyleEngine::has_recorded_input() const
{
    return !m_tree_deltas.is_empty()
        || !m_element_arrivals.is_empty()
        || !m_local_feature_deltas.is_empty()
        || !m_state_deltas.is_empty()
        || !m_element_declaration_deltas.is_empty()
        || !m_element_style_inputs.is_empty();
}

void StyleEngine::submit_recorded_input()
{
    if (m_style_computer)
        publish_pending_element_features(*this, *m_style_computer);
    if (!has_recorded_input()) {
        if (refresh_attribute_value_text_requirements() && m_style_computer)
            publish_required_attribute_value_texts(*this, *m_style_computer);
        return;
    }

    InputTransaction transaction {
        .tree_deltas = m_tree_deltas.data(),
        .tree_delta_count = m_tree_deltas.size(),
        .element_arrivals = m_element_arrivals.data(),
        .element_arrival_count = m_element_arrivals.size(),
        .arrival_custom_state_atoms = m_arrival_custom_state_atoms.data(),
        .arrival_custom_state_atom_count = m_arrival_custom_state_atoms.size(),
        .local_feature_deltas = m_local_feature_deltas.data(),
        .local_feature_delta_count = m_local_feature_deltas.size(),
        .state_deltas = m_state_deltas.data(),
        .state_delta_count = m_state_deltas.size(),
        .element_declaration_deltas = m_element_declaration_deltas.data(),
        .element_declaration_delta_count = m_element_declaration_deltas.size(),
        .element_style_inputs = m_element_style_inputs.data(),
        .element_style_input_count = m_element_style_inputs.size(),
    };
    apply_transaction(transaction);

    m_tree_deltas.clear_with_capacity();
    m_element_arrivals.clear_with_capacity();
    m_arrival_custom_state_atoms.clear_with_capacity();
    m_local_feature_deltas.clear_with_capacity();
    m_state_deltas.clear_with_capacity();
    m_element_declaration_deltas.clear_with_capacity();
    m_element_style_inputs.clear_with_capacity();
    m_element_style_input_indices.clear_with_capacity();

    // Selector demand can arrive while the program change and element facts are still staged.
    // Refresh after applying the fact batch, then backfill values before matching observes it.
    if (refresh_attribute_value_text_requirements() && m_style_computer)
        publish_required_attribute_value_texts(*this, *m_style_computer);
}

void StyleEngine::apply_transaction(InputTransaction const& transaction)
{
    StyleEngineFFI::style_engine_apply_transaction(m_impl, &transaction);
}

void StyleEngine::flush()
{
    submit_recorded_input();
    StyleEngineFFI::style_engine_flush(m_impl);
}

bool StyleEngine::take_diagnostic_style_transaction(StyleNodeID root, Function<void(ReadonlySpan<StyleNodeID>)>&& consume)
{
    Vector<StyleNodeID> reaction_nodes;
    auto transaction = take_style_transaction(root);
    for (auto const& reaction : transaction.reactions)
        reaction_nodes.append(StyleNodeID { reaction.style_node });
    discard_style_transaction_outputs();
    if (!transaction.is_scoped)
        return false;
    consume(reaction_nodes.span());
    return true;
}

void StyleEngine::discard_style_transaction_outputs()
{
    StyleEngineFFI::style_engine_discard_style_transaction_outputs(m_impl);
}

StyleEngine::PublishedStyleTransaction StyleEngine::take_style_transaction(StyleNodeID root)
{
    submit_recorded_input();
    StyleEngineFFI::FfiDocumentStyleComputationInputs computation_inputs {};
    if (m_style_computer) {
        auto const viewport_rect = m_style_computer->viewport_rect_for_style_environment();
        auto const& root_font_metrics = m_style_computer->root_element_font_metrics();
        computation_inputs = {
            .viewport_width = viewport_rect.width().to_double(),
            .viewport_height = viewport_rect.height().to_double(),
            .root_font_size = root_font_metrics.font_size.to_double(),
            .root_font_x_height = root_font_metrics.x_height.to_double(),
            .root_font_cap_height = root_font_metrics.cap_height.to_double(),
            .root_font_zero_advance = root_font_metrics.zero_advance.to_double(),
            .root_line_height = root_font_metrics.line_height.to_double(),
            .root_font_metrics_depend_on_viewport_metrics = m_style_computer->root_element_font_metrics_depend_on_viewport_metrics(),
            .initial_font_size_raw = InitialValues::font_size().raw_value(),
            .default_font_size_raw = StyleComputer::default_user_font_size().raw_value(),
            .device_pixels_per_css_pixel = m_style_computer->document().page().client().device_pixels_per_css_pixel(),
            .font_environment_generation = m_style_computer->document().font_computer().environment_generation(),
        };
    }
    auto view = StyleEngineFFI::style_engine_take_style_transaction(m_impl, root.value(), computation_inputs);
    if (view.reclaimed_style_atom_count != 0) {
        HashTable<StyleAtomID> reclaimed_atoms;
        reclaimed_atoms.ensure_capacity(view.reclaimed_style_atom_count);
        for (auto const& reclaimed : ReadonlySpan<StyleEngineFFI::FfiReclaimedStyleAtom> { view.reclaimed_style_atoms, view.reclaimed_style_atom_count }) {
            auto atom_id = StyleAtomID { reclaimed.atom };
            reclaimed_atoms.set(atom_id);
            m_published_language_atoms.remove(atom_id);
            m_attribute_names_requiring_value_text.remove(atom_id);
            if (reclaimed.raw == 0)
                continue;
            auto atom = m_atoms.take(reclaimed.raw);
            VERIFY(atom.has_value());
            VERIFY(atom.release_value() == reclaimed.atom);
            Utf16FlyString::unref_raw(reclaimed.raw);
        }
        m_attribute_name_atoms.remove_all_matching([&](StyleAtomID local, auto& names_by_namespace) {
            if (reclaimed_atoms.contains(local))
                return true;
            names_by_namespace.remove_all_matching([&](StyleAtomID namespace_atom, StyleAtomID name) {
                return reclaimed_atoms.contains(namespace_atom) || reclaimed_atoms.contains(name);
            });
            return names_by_namespace.is_empty();
        });
        ++m_atom_generation;
    }
    return {
        .version = { view.transaction_version, view.program_version },
        .reactions = { view.answers, view.count },
        .is_scoped = view.scoped,
    };
}

void StyleEngine::sort_style_deltas_for_direct_application(Span<PublishedStyleDelta> deltas) const
{
    StyleEngineFFI::style_engine_sort_style_deltas_for_direct_application(m_impl, deltas.data(), deltas.size());
}

bool StyleEngine::has_pending_transaction() const
{
    return has_recorded_input() || StyleEngineFFI::style_engine_has_pending_transaction(m_impl);
}

bool StyleEngine::pending_transaction_may_affect_layout_geometry()
{
    submit_recorded_input();
    return StyleEngineFFI::style_engine_pending_transaction_may_affect_layout_geometry(m_impl);
}

bool StyleEngine::has_deferred_geometry_transaction() const
{
    return StyleEngineFFI::style_engine_has_deferred_geometry_transaction(m_impl);
}

bool StyleEngine::has_deferred_element_style_inputs() const
{
    return StyleEngineFFI::style_engine_has_deferred_element_style_inputs(m_impl);
}

bool StyleEngine::has_deferred_element_style_input(StyleNodeID style_node) const
{
    return StyleEngineFFI::style_engine_has_deferred_element_style_input(m_impl, style_node.value());
}

bool StyleEngine::defer_pending_transaction_for_geometry_read()
{
    submit_recorded_input();
    return StyleEngineFFI::style_engine_defer_pending_transaction_for_geometry_read(m_impl);
}

bool StyleEngine::begin_deferred_geometry_transaction_flush()
{
    submit_recorded_input();
    return StyleEngineFFI::style_engine_begin_deferred_geometry_transaction_flush(m_impl);
}

void StyleEngine::end_deferred_geometry_transaction_flush()
{
    StyleEngineFFI::style_engine_end_deferred_geometry_transaction_flush(m_impl);
}

bool StyleEngine::read_matches(StyleNodeID node, Vector<RuleMatch>& matches, Optional<MatchPurpose> purpose)
{
    matches.resize(max(m_element_match_capacity, 16u));
    auto read = [&] {
        if (!purpose.has_value())
            return StyleEngineFFI::style_engine_consume_published_match_answer(m_impl, node.value(), matches.data(), matches.size());
        return StyleEngineFFI::style_engine_match_element(m_impl, node.value(), matches.data(), matches.size(), *purpose == MatchPurpose::Cascade);
    };
    auto count = read();
    if (count == NumericLimits<size_t>::max())
        return false;
    if (count > matches.size()) {
        // Nothing was written, so grow and ask again rather than reporting a truncated answer.
        m_element_match_capacity = count * 2;
        matches.resize(m_element_match_capacity);
        count = read();
        if (count == NumericLimits<size_t>::max() || count > matches.size())
            return false;
    }
    matches.shrink(count);
    return true;
}

bool StyleEngine::match_element(StyleNodeID node, Vector<RuleMatch>& matches, MatchPurpose purpose)
{
    // A synchronous match is an observation boundary. Most matching follows a published style
    // transaction, but detached-document style reads can arrive directly while mutation facts are
    // still staged. Settle those facts before asking the committed arrangement.
    if (has_pending_transaction())
        flush();
    return read_matches(node, matches, purpose);
}

bool StyleEngine::consume_published_match_answer(StyleNodeID node, Vector<RuleMatch>& matches)
{
    return read_matches(node, matches, {});
}

void* StyleEngine::compile_selector_query(ReadonlySpan<void const*> selectors)
{
    auto* query = StyleEngineFFI::style_engine_compile_selector_query(m_impl, selectors.data(), selectors.size());
    if (refresh_attribute_value_text_requirements() && m_style_computer)
        publish_required_attribute_value_texts(*this, *m_style_computer);
    return query;
}

void StyleEngine::destroy_selector_query(void* query)
{
    StyleEngineFFI::style_engine_destroy_selector_query(query);
}

void StyleEngine::prepare_selector_query()
{
    // Exact selector queries read current tree and fact columns. Make those inputs visible while
    // retaining their old rows and the transaction journal for the normal style update to consume.
    submit_recorded_input();
    StyleEngineFFI::style_engine_prepare_selector_query(m_impl);
}

Optional<bool> StyleEngine::selector_query_matches(void const* query, StyleNodeID node, StyleNodeID scope_root, StyleNodeID shadow_root)
{
    auto result = StyleEngineFFI::style_engine_selector_query_matches(m_impl, query, node.value(), scope_root.value(), shadow_root.value());
    if (result == NumericLimits<u8>::max())
        return {};
    return result != 0;
}

Optional<bool> StyleEngine::selector_query_matches_without_document_root(void const* query, StyleNodeID node, StyleNodeID scope_root, StyleNodeID shadow_root)
{
    auto result = StyleEngineFFI::style_engine_selector_query_matches_without_document_root(m_impl, query, node.value(), scope_root.value(), shadow_root.value());
    if (result == NumericLimits<u8>::max())
        return {};
    return result != 0;
}

bool StyleEngine::selector_query_all(void* query, StyleNodeID root, bool include_root, StyleNodeID scope_root, StyleNodeID shadow_root, bool has_document_root, Vector<StyleNodeID>& matches)
{
    matches.resize(16);
    auto read = [&] {
        return StyleEngineFFI::style_engine_selector_query_all(
            m_impl,
            query,
            root.value(),
            include_root,
            scope_root.value(),
            shadow_root.value(),
            has_document_root,
            reinterpret_cast<u32*>(matches.data()),
            matches.size());
    };
    auto count = read();
    if (count == NumericLimits<size_t>::max())
        return false;
    if (count > matches.size()) {
        matches.resize(count);
        count = read();
        if (count == NumericLimits<size_t>::max() || count > matches.size())
            return false;
    }
    matches.shrink(count);
    return true;
}

bool StyleEngine::counter(size_t index, StringView& out_name, u64& out_value) const
{
    size_t name_length = 0;
    auto const* name = StyleEngineFFI::style_engine_counter(m_impl, index, &out_value, &name_length);
    if (!name)
        return false;
    out_name = StringView { name, name_length };
    return true;
}

}
