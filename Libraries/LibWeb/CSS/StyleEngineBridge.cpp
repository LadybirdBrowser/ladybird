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
}

StyleEngine::~StyleEngine()
{
    if (m_impl)
        StyleEngineFFI::style_engine_destroy(m_impl);
    for (auto raw : m_atoms)
        Utf16FlyString::unref_raw(raw);
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

Vector<StyleNodeID> StyleEngine::take_deferred_element_initial_features()
{
    Vector<StyleNodeID> nodes;
    nodes.ensure_capacity(m_nodes_with_pending_initial_features.size());
    for (auto node : m_nodes_with_pending_initial_features)
        nodes.unchecked_append(node);
    m_nodes_with_pending_initial_features.clear_with_capacity();
    return nodes;
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

void StyleEngine::materialize_retained_cascade_state(StyleNodeID node, u8 pseudo_kind, ComputedValuesFFI::CascadedPropertyStore* store, ReadonlySpan<ComputedValuesFFI::FfiCascadeBlock> blocks, ComputedValuesFFI::FfiBulkCascadeCallbacks const* callbacks)
{
    StyleEngineFFI::style_engine_materialize_retained_cascade_state(m_impl, node.value(), pseudo_kind, store, blocks.data(), blocks.size(), callbacks);
}

StyleEngine::StyleRecordDelta StyleEngine::publish_computed_groups(StyleNodeID node, u8 pseudo_kind, ReadonlySpan<void const*> payloads, size_t inherited_group_count, u64 custom_property_environment, u64 pseudo_element_styles, u8 dependency_flags, u64 counter_style_environment_identity, u64 animation_overlay_identity, void const* animated_properties, ReadonlySpan<void const*> animation_overlay_payloads, ReadonlyBytes property_importance, ReadonlyBytes property_inheritance, ReadonlySpan<u16> inheritance_dependent_properties, ReadonlySpan<void const*> inheritance_dependent_values, void const* raw_cascaded_font_size)
{
    VERIFY(inherited_group_count <= payloads.size());
    VERIFY(inheritance_dependent_properties.size() == inheritance_dependent_values.size());
    auto delta = StyleEngineFFI::style_engine_publish_computed_groups(m_impl, node.value(), pseudo_kind, payloads.data(), payloads.size(), inherited_group_count, custom_property_environment, pseudo_element_styles, dependency_flags, counter_style_environment_identity, animation_overlay_identity, animated_properties, animation_overlay_payloads.data(), animation_overlay_payloads.size(), property_importance.data(), property_importance.size(), property_inheritance.data(), property_inheritance.size(), inheritance_dependent_properties.data(), inheritance_dependent_values.data(), inheritance_dependent_properties.size(), raw_cascaded_font_size);
    return { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

void const* StyleEngine::style_record_payloads(StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_style_record_payloads(m_impl, style_record.value());
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
    // atom is live; a duplicate releases it again.
    auto raw = name.to_raw_leaked();
    if (m_atoms.set(raw) != HashSetResult::InsertedNewEntry)
        Utf16FlyString::unref_raw(raw);
    return StyleAtomID { StyleEngineFFI::style_engine_intern_atom(m_impl, raw) };
}

StyleAtomID StyleEngine::intern_text_atom(Utf16View text)
{
    return intern_atom(Utf16FlyString::from_utf16(text).to_ascii_lowercase());
}

StyleAtomID StyleEngine::intern_case_sensitive_text_atom(Utf16View text)
{
    return intern_atom(Utf16FlyString::from_utf16(text));
}

StyleAtomID StyleEngine::intern_attribute_value(Utf16View value)
{
    auto atom = intern_case_sensitive_text_atom(value);
    // The engine holds one copy of the text per currently used value. Ask whether it survived
    // reclamation before copying it out of the attribute's representation again.
    if (StyleEngineFFI::style_engine_has_attribute_value_text(m_impl, atom.value()))
        return atom;

    Vector<u16> code_units;
    code_units.ensure_capacity(value.length_in_code_units());
    for (size_t i = 0; i < value.length_in_code_units(); ++i)
        code_units.unchecked_append(value.code_unit_at(i));
    StyleEngineFFI::style_engine_set_attribute_value_text(m_impl, atom.value(), code_units.data(), code_units.size());
    return atom;
}

void StyleEngine::set_element_language(StyleNodeID node, StyleAtomID language, Utf16View tag)
{
    // A language range is not a name, so `:lang()` compares against the tag itself rather than
    // against the atom. The text is recorded once per language, not once per element.
    Vector<u16> code_units;
    code_units.ensure_capacity(tag.length_in_code_units());
    for (size_t i = 0; i < tag.length_in_code_units(); ++i)
        code_units.unchecked_append(tag.code_unit_at(i));
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

void StyleEngine::record_tree_delta(StyleEngineFFI::FfiTreeDelta const& delta)
{
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_tree_deltas.append(delta);
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
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_element_declaration_deltas.append(delta);
}

void StyleEngine::record_element_style_input_change(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node != 0 && reaction != 0) {
        request_frame_for_first_recorded_input(*this, m_style_computer);
        m_element_style_inputs.append({ style_node.value(), reaction, inherited_style_groups });
    }
}

void StyleEngine::record_flat_tree_descendant_style_input_changes(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node == 0 || reaction == 0)
        return;

    // The relation columns must include every tree delta recorded before this derived action. The
    // descendants themselves stay in the C++ batch so the next transaction normalizes them with
    // every other feedback action from this stabilization pass.
    submit_recorded_input();
    request_frame_for_first_recorded_input(*this, m_style_computer);
    struct Context {
        Vector<StyleEngineFFI::FfiElementStyleInput>& inputs;
        u8 reaction;
        u8 inherited_style_groups;
    } context { m_element_style_inputs, reaction, inherited_style_groups };
    StyleEngineFFI::style_engine_for_each_flat_tree_descendant(
        m_impl,
        style_node.value(),
        &context,
        [](void* context, u32 descendant) {
            auto& data = *static_cast<Context*>(context);
            data.inputs.append({ descendant, data.reaction, data.inherited_style_groups });
        });
}

void StyleEngine::consume_recorded_element_style_input_change(StyleNodeID style_node)
{
    m_element_style_inputs.remove_all_matching([&](auto const& input) {
        return input.style_node == style_node.value();
    });
}

bool StyleEngine::has_recorded_element_style_input_change(StyleNodeID style_node) const
{
    for (auto const& input : m_element_style_inputs) {
        if (input.style_node == style_node.value())
            return true;
    }
    return false;
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
        || !m_local_feature_deltas.is_empty()
        || !m_state_deltas.is_empty()
        || !m_element_declaration_deltas.is_empty()
        || !m_element_style_inputs.is_empty();
}

void StyleEngine::submit_recorded_input()
{
    if (m_style_computer)
        publish_pending_element_features(*this, *m_style_computer);
    if (!has_recorded_input())
        return;

    InputTransaction transaction {
        .tree_deltas = m_tree_deltas.data(),
        .tree_delta_count = m_tree_deltas.size(),
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
    m_local_feature_deltas.clear_with_capacity();
    m_state_deltas.clear_with_capacity();
    m_element_declaration_deltas.clear_with_capacity();
    m_element_style_inputs.clear_with_capacity();
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
    auto transaction_is_scoped = take_style_transaction(
        root,
        [&](PublishedTransactionVersion, ReadonlySpan<PublishedStyleDelta> reactions) {
            for (auto const& reaction : reactions)
                reaction_nodes.append(StyleNodeID { reaction.style_node });
        });
    StyleEngineFFI::style_engine_discard_style_transaction_outputs(m_impl);
    if (!transaction_is_scoped)
        return false;
    consume(reaction_nodes.span());
    return true;
}

bool StyleEngine::take_style_transaction(
    StyleNodeID root,
    Function<void(PublishedTransactionVersion, ReadonlySpan<PublishedStyleDelta>)>&& consume)
{
    struct Callbacks {
        Function<void(PublishedTransactionVersion, ReadonlySpan<PublishedStyleDelta>)>& consume;
    } callbacks { consume };

    submit_recorded_input();
    return StyleEngineFFI::style_engine_take_style_transaction(
        m_impl,
        root.value(),
        &callbacks,
        [](void* context, u64 transaction_version, u64 program_version, PublishedStyleDelta const* answers, size_t count) {
            static_cast<Callbacks*>(context)->consume(
                PublishedTransactionVersion { transaction_version, program_version },
                ReadonlySpan<PublishedStyleDelta> { answers, count });
        });
}

bool StyleEngine::has_pending_transaction() const
{
    return has_recorded_input() || StyleEngineFFI::style_engine_has_pending_transaction(m_impl);
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
    return StyleEngineFFI::style_engine_compile_selector_query(m_impl, selectors.data(), selectors.size());
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
