/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontResolution.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/SharedCompiledStyleSheet.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

static_assert(StyleEngineFFI::LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND == to_underlying(last_synthetic_pseudo_element));
static_assert(StyleEngineFFI::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND == to_underlying(first_element_reference_pseudo_element));
static_assert(StyleEngineFFI::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND == to_underlying(last_element_reference_pseudo_element));
static_assert(!IsMoveConstructible<StyleEngine>);
static_assert(!IsMoveAssignable<StyleEngine>);

#include <LibWeb/StyleEngineBridgeGenerated.inc>

static NonnullRefPtr<Layout::RenderDocument> create_render_document(StyleEngine::DeviceClass device_class)
{
    // The engine the render state creates holds the style groups each longhand reaches from its creation on.
    register_style_groups();
    return Layout::RenderDocument::create(to_underlying(device_class));
}

StyleEngine::StyleEngine(DeviceClass device_class, StyleComputer* style_computer)
    : m_render_document(create_render_document(device_class))
    , m_style_node_ids(StyleEngineFFI::style_node_id_allocator_create())
    , m_style_computer(style_computer)
{
    if (m_style_computer) {
        set_pseudo_element_style_deferred(to_underlying(PseudoElement::Selection), true);
        set_pseudo_element_style_deferred(to_underlying(PseudoElement::SearchText), true);
    }
}

StyleEngine::~StyleEngine()
{
    StyleEngineFFI::style_node_id_allocator_destroy(m_style_node_ids);
    for (auto const& atom : m_atoms)
        Utf16FlyString::unref_raw(atom.key);
}

void StyleEngine::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_style_computer);
}

StyleNodeID StyleEngine::mint_style_node()
{
    StyleNodeID node;
    mint_style_nodes({ &node, 1 });
    return node;
}

void StyleEngine::mint_style_nodes(Span<StyleNodeID> nodes)
{
    if (nodes.is_empty())
        return;
    auto* raw_nodes = reinterpret_cast<u32*>(nodes.data());
    StyleEngineFFI::style_node_id_allocator_mint(m_style_node_ids, false, raw_nodes, nodes.size());
    StyleEngineFFI::style_engine_mint_style_nodes(m_render_document->host(), raw_nodes, nodes.size());
    // An element minted beside a style transaction that flew is unknown to it, whenever its arrival is recorded.
    if (has_flown_style_transaction()) {
        for (auto node : nodes)
            m_style_nodes_beside_flown_transaction.set(node);
    }
}

void StyleEngine::mint_text_style_nodes(Span<StyleNodeID> nodes)
{
    if (nodes.is_empty())
        return;
    auto* raw_nodes = reinterpret_cast<u32*>(nodes.data());
    StyleEngineFFI::style_node_id_allocator_mint(m_style_node_ids, true, raw_nodes, nodes.size());
    StyleEngineFFI::style_engine_mint_style_nodes(m_render_document->host(), raw_nodes, nodes.size());
}

HashTable<StyleNodeID> StyleEngine::take_deferred_element_initial_features()
{
    return move(m_nodes_with_pending_initial_features);
}

HashTable<StyleNodeID> StyleEngine::take_elements_awaiting_first_style_computation()
{
    return move(m_nodes_awaiting_first_style_computation);
}

void StyleEngine::set_element_parts(StyleNodeID node, ReadonlySpan<StyleAtomID> names, ReadonlySpan<StyleNodeID> hosts)
{
    VERIFY(names.size() == hosts.size());
    StyleEngineFFI::style_engine_set_element_parts(m_render_document->host(), node.value(), reinterpret_cast<u32 const*>(names.data()), reinterpret_cast<u32 const*>(hosts.data()), names.size());
}

SheetID StyleEngine::add_sheet(u32 object, StyleEngineFFI::FfiCascadeOrigin origin)
{
    return SheetID { StyleEngineFFI::style_engine_add_sheet(m_render_document->host(), object, origin) };
}

void StyleEngine::begin_sheet_rules_replacement(SheetID sheet)
{
    StyleEngineFFI::style_engine_begin_sheet_rules_replacement(m_render_document->host(), sheet.value());
}

void StyleEngine::finish_sheet_rules_replacement(SheetID sheet)
{
    StyleEngineFFI::style_engine_finish_sheet_rules_replacement(m_render_document->host(), sheet.value(), next_declaration_block_version());
}

void StyleEngine::set_element_inline_style_properties(StyleNodeID node, RustDeclarationBlock const* declarations)
{
    if (StyleEngineFFI::style_engine_set_element_inline_style_properties(host(), node.value(), declarations ? declarations->handle() : nullptr))
        note_css_transitions_may_observe_style_changes();
}

void StyleEngine::set_element_presentational_hint_properties(StyleNodeID node, StyleEngineFFI::FfiElementDeclarationKind kind, ReadonlySpan<StyleProperty> properties)
{
    Vector<Parser::ValueParserFFI::FfiDeclaredProperty> declarations;
    declarations.ensure_capacity(properties.size());
    for (auto const& property : properties) {
        declarations.unchecked_append({
            .property_id = to_underlying(property.property_id),
            .important = property.important == Important::Yes,
            .value = property.value->rust_style_value_data(),
            .name = {},
        });
    }
    if (StyleEngineFFI::style_engine_set_element_presentational_hint_properties(host(), node.value(), kind, declarations.data(), declarations.size()))
        note_css_transitions_may_observe_style_changes();
}

StyleEngine::StyleRecordDelta StyleEngine::publish_computed_groups(Layout::BegunRead const& read, StyleNodeID node, u8 pseudo_kind, ReadonlySpan<void const*> payloads, size_t inherited_group_count, u64 custom_property_environment, bool inherited_group_swap_candidate, u64 counter_style_environment_identity, u64 animation_overlay_identity, void const* animated_overlay, ReadonlySpan<void const*> animation_overlay_payloads, void const* computed_longhand_table, void const* custom_property_store)
{
    VERIFY(inherited_group_count <= payloads.size());
    auto delta = StyleEngineFFI::style_engine_publish_computed_groups(host(), &read, node.value(), pseudo_kind, payloads.data(), payloads.size(), inherited_group_count, custom_property_environment, inherited_group_swap_candidate, counter_style_environment_identity, animation_overlay_identity, animated_overlay, animation_overlay_payloads.data(), animation_overlay_payloads.size(), computed_longhand_table, custom_property_store);
    return { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

Optional<StyleEngine::StyleRecordDelta> StyleEngine::publish_animation_overlay(Layout::BegunRead const& read, StyleNodeID node, u8 pseudo_kind, u64 animation_overlay_identity, void const* animated_overlay, ReadonlySpan<void const*> payloads)
{
    auto delta = StyleEngineFFI::style_engine_publish_animation_overlay(host(), &read, node.value(), pseudo_kind, animation_overlay_identity, animated_overlay, payloads.data(), payloads.size());
    if (delta.new_style_record == 0)
        return {};
    return StyleRecordDelta { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

StyleRecordDependencyFlag StyleEngine::style_record_dependency_flags(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    return static_cast<StyleRecordDependencyFlag>(StyleEngineFFI::style_engine_style_record_dependency_flags(m_render_document->host(), &read, style_record.value()));
}

u64 StyleEngine::style_record_custom_property_environment(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_style_record_custom_property_environment(m_render_document->host(), &read, style_record.value());
}

u32 StyleEngine::compare_style_records(Layout::BegunRead const& read, StyleRecordID old_style_record, StyleRecordID new_style_record) const
{
    return StyleEngineFFI::style_engine_compare_style_records(host(), &read, old_style_record.value(), new_style_record.value());
}

u32 StyleEngine::element_record_damage(Layout::BegunRead const& read, StyleNodeID node, StyleRecordID old_style_record, StyleRecordID new_style_record) const
{
    return StyleEngineFFI::style_engine_element_record_damage(host(), &read, node.value(), old_style_record.value(), new_style_record.value());
}

u32 StyleEngine::pseudo_element_record_damage(Layout::BegunRead const& read, StyleNodeID node, PseudoElement pseudo_element, StyleRecordID old_style_record, StyleRecordID new_style_record, StyleRecordID originating_style_record, bool counter_styles_changed) const
{
    return StyleEngineFFI::style_engine_pseudo_element_record_damage(host(), &read, node.value(), to_underlying(pseudo_element), old_style_record.value(), new_style_record.value(), originating_style_record.value(), counter_styles_changed);
}

bool StyleEngine::animation_overlay_changed(Layout::BegunRead const& read, StyleRecordID old_style_record, void const* animated_overlay) const
{
    return StyleEngineFFI::style_engine_animation_overlay_changed(host(), &read, old_style_record.value(), animated_overlay);
}

StyleEngineFFI::FfiAnimationInvalidation StyleEngine::compare_animation_overlay(Layout::BegunRead const& read, StyleRecordID old_style_record, void const* animated_overlay, ReadonlySpan<void const*> payloads, bool is_document_element) const
{
    return StyleEngineFFI::style_engine_compare_animation_overlay(host(), &read, old_style_record.value(), animated_overlay, payloads.data(), payloads.size(), is_document_element);
}

StyleEngine::StyleRecordView StyleEngine::style_record_view(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_style_record_view(host(), &read, style_record.value());
}

double StyleEngine::ensure_random_base_value(Layout::BegunRead const& read, StyleNodeID node, Utf16View name, bool element_shared)
{
    Vector<u16, 32> code_units;
    code_units.ensure_capacity(name.length_in_code_units());
    for (size_t i = 0; i < name.length_in_code_units(); ++i)
        code_units.unchecked_append(name.code_unit_at(i));
    return bit_cast<double>(ensure_random_base_value_bits(read, node, code_units, element_shared));
}

ParkedRandomBaseValues StyleEngine::park_element_random_base_values(StyleNodeID node)
{
    return ParkedRandomBaseValues { StyleEngineFFI::style_engine_park_element_random_base_values(host(), node.value()) };
}

void StyleEngine::unpark_element_random_base_values(StyleNodeID node, ParkedRandomBaseValues values)
{
    StyleEngineFFI::style_engine_unpark_element_random_base_values(host(), node.value(), values.leak_slot());
}

ParkedRandomBaseValues::~ParkedRandomBaseValues()
{
    if (m_slot)
        StyleEngineFFI::style_engine_release_random_base_values(m_slot);
}

void StyleEngine::decide_transitions(Layout::BegunRead const& read, StyleRecordID before_style_record, StyleRecordID after_style_record, StyleValueFFI::FfiTransitionInput const& input, StyleValueFFI::FfiTransitionAction* actions) const
{
    StyleEngineFFI::style_engine_decide_transitions(m_render_document->host(), &read, before_style_record.value(), after_style_record.value(), &input, actions);
}

StyleEngine::StyleRecordDelta StyleEngine::remove_computed_pseudo(Layout::BegunRead const& read, StyleNodeID node, u8 pseudo_kind)
{
    auto delta = StyleEngineFFI::style_engine_remove_computed_pseudo(host(), &read, node.value(), pseudo_kind);
    return { StyleRecordID { delta.old_style_record }, StyleRecordID { delta.new_style_record } };
}

StyleAtomID StyleEngine::intern_atom(Utf16FlyString const& name)
{
    // Utf16FlyString is already interned, so its one-word raw form is the name's identity. The atom
    // is the process-global one, which is also what selector names intern as: two tables keyed by
    // the same word but each assigning its own sequence would compare unequal for the same name,
    // which fails to match silently rather than loudly. The host takes a reference to it without
    // the engine, which adopts the name later.
    // First time seen, the leaked reference is kept so the identity cannot be reused while the
    // atom is live. Duplicates release their new reference and return without crossing the FFI.
    auto raw = name.to_raw_leaked();
    if (auto atom = m_atoms.get(raw); atom.has_value()) {
        Utf16FlyString::unref_raw(raw);
        return atom.release_value();
    }
    auto atom = StyleAtomID { StyleEngineFFI::document_host_intern_atom(host(), raw) };
    m_atoms.set(raw, atom);
    return atom;
}

StyleAtomID StyleEngine::intern_qualified_atom(StyleAtomID namespace_atom, StyleAtomID name)
{
    return StyleAtomID { StyleEngineFFI::document_host_intern_qualified_atom(host(), namespace_atom.value(), name.value()) };
}

void StyleEngine::note_custom_property_name(StyleAtomID atom, Utf16FlyString const& name)
{
    if (m_published_custom_property_names.contains(atom))
        return;
    m_published_custom_property_names.set(atom);
    auto const view = name.view();
    Vector<u16> code_units;
    code_units.ensure_capacity(view.length_in_code_units());
    for (size_t i = 0; i < view.length_in_code_units(); ++i)
        code_units.unchecked_append(view.code_unit_at(i));
    // The write carries this reference across to the engine, which retains the fly string itself.
    StyleEngineFFI::style_engine_note_custom_property_name(host(), atom.value(), name.to_raw_leaked(), code_units.data(), code_units.size());
}

StyleRecordID StyleEngine::republish_record_environment(Layout::BegunRead const& read, StyleNodeID node, u64 environment, void const* store)
{
    return StyleRecordID { StyleEngineFFI::style_engine_republish_record_environment(host(), &read, node.value(), environment, store) };
}

#define ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(name) \
    static_assert(to_underlying(StyleEngine::DemandedPseudoElement::name) == to_underlying(PseudoElement::name));
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(After)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(SearchText)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(ViewTransition)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(DetailsContent)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(SliderTrack)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(ViewTransitionGroup)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(ViewTransitionOld)
#undef ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND

Optional<StyleEngine::DemandedPseudoElement> StyleEngine::demanded_pseudo_element(PseudoElement pseudo_element)
{
    // The engine numbers each pseudo-element it answers as its kind: the synthetic ones, the ones an element in the
    // shadow tree backs, and the named view transition ones. ::part() and ::slotted() name other elements.
    switch (pseudo_element) {
    case PseudoElement::Part:
    case PseudoElement::Slotted:
    case PseudoElement::KnownPseudoElementCount:
    case PseudoElement::UnknownWebKit:
        return {};
    default:
        return static_cast<DemandedPseudoElement>(to_underlying(pseudo_element));
    }
}

StyleEngineFFI::FfiRecordDemandAnswer StyleEngine::answer_record_demand(Layout::BegunRead const& read, StyleNodeID node, RecordDemand demand)
{
    return StyleEngineFFI::style_engine_answer_record_demand(m_render_document->host(), &read, node.value(), demand);
}

StyleEngineFFI::FfiRecordDemandAnswer StyleEngine::answer_pseudo_element_record_demand(Layout::BegunRead const& read, StyleNodeID node, PseudoElementRecordDemand demand, DemandedPseudoElement pseudo_element)
{
    return StyleEngineFFI::style_engine_answer_pseudo_element_record_demand(m_render_document->host(), &read, node.value(), demand, pseudo_element);
}

StyleEngineFFI::FfiSettledPseudoRecords StyleEngine::settle_pseudo_records_after_host_record(Layout::BegunRead const& read, StyleNodeID node, bool old_is_list_item)
{
    return StyleEngineFFI::style_engine_settle_pseudo_records_after_host_record(host(), &read, node.value(), old_is_list_item);
}

u64 StyleEngine::inheritable_custom_property_environment(Layout::BegunRead const& read, u64 identity) const
{
    return StyleEngineFFI::style_engine_inheritable_custom_property_environment(host(), &read, identity);
}

void const* StyleEngine::borrow_engine_custom_property_environment(Layout::BegunRead const& read, u64 identity, u64& parent_identity) const
{
    return StyleEngineFFI::style_engine_borrow_engine_custom_property_environment(host(), &read, identity, &parent_identity);
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
    StyleEngineFFI::style_engine_set_element_language(m_render_document->host(), 0, atom.value(), code_units.data(), code_units.size());
    return atom;
}

StyleAtomID StyleEngine::intern_case_sensitive_text_atom(Utf16View text)
{
    return intern_atom(Utf16FlyString::from_utf16(text));
}

void StyleEngine::publish_html_element_namespace(StyleAtomID namespace_atom)
{
    // NB: The engine keeps the atom it was told alive, so an equal one names the same namespace.
    if (exchange(m_html_element_namespace, namespace_atom) != namespace_atom)
        set_html_element_namespace(namespace_atom);
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
    // An attr() reads an attribute in no namespace by its local name.
    if (namespace_atom == 0) {
        auto local_name_view = local_name.view();
        Vector<u16> local_name_code_units;
        local_name_code_units.ensure_capacity(local_name_view.length_in_code_units());
        for (size_t i = 0; i < local_name_view.length_in_code_units(); ++i)
            local_name_code_units.unchecked_append(local_name_view.code_unit_at(i));
        note_attribute_substitution_name(name, local_name_code_units);
    }
    names_by_namespace.set(namespace_atom, name);
    return name;
}

StyleAtomID StyleEngine::intern_attribute_value(StyleAtomID name, Utf16String const& value)
{
    auto atom = intern_atom(Utf16FlyString { value });
    if (!attribute_name_requires_value_text(name))
        return atom;

    publish_attribute_value_text(atom, value);
    return atom;
}

void StyleEngine::backfill_attribute_value_text_if_required(StyleAtomID name, Utf16String const& value)
{
    if (!attribute_name_requires_value_text(name))
        return;

    auto atom = intern_atom(Utf16FlyString { value });
    publish_attribute_value_text(atom, value);
}

void StyleEngine::publish_attribute_value_text(StyleAtomID atom, Utf16View value)
{
    // The engine holds one copy of the text per currently used value. Ask whether it survived
    // reclamation before copying it out of the attribute's representation again, as the host's own read of the render
    // state.
    Layout::ForcedReadScope read { render_document(), false };
    if (StyleEngineFFI::style_engine_has_attribute_value_text(m_render_document->host(), read, atom.value()))
        return;

    Vector<u16> code_units;
    code_units.ensure_capacity(value.length_in_code_units());
    for (size_t i = 0; i < value.length_in_code_units(); ++i)
        code_units.unchecked_append(value.code_unit_at(i));
    StyleEngineFFI::style_engine_set_attribute_value_text(m_render_document->host(), atom.value(), code_units.data(), code_units.size());
}

bool StyleEngine::refresh_attribute_value_text_requirements(Layout::BegunRead const& read)
{
    auto version = StyleEngineFFI::style_engine_attribute_value_text_requirements_version(m_render_document->host(), &read);
    if (version == m_attribute_value_text_requirements_version)
        return false;
    m_attribute_value_text_requirements_version = version;
    m_attribute_names_requiring_value_text.clear();
    return true;
}

bool StyleEngine::attribute_name_requires_value_text(StyleAtomID name)
{
    return m_attribute_names_requiring_value_text.ensure(name, [&] {
        // The engine works out which names its selectors read the value text of, so a name not seen
        // since they changed is the host's own read of the render state.
        Layout::ForcedReadScope read { render_document(), false };
        return StyleEngineFFI::style_engine_attribute_name_requires_value_text(m_render_document->host(), read, name.value());
    });
}

void StyleEngine::set_text_data(StyleNodeID node, Utf16String const& data)
{
    StyleEngineFFI::style_engine_set_text_data(m_render_document->host(), node.value(), data.to_raw_leaked());
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
    StyleEngineFFI::style_engine_set_element_language(m_render_document->host(), node.value(), language.value(), code_units.data(), code_units.size());
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
    // The flush asks the engine whether the transaction a geometry read deferred is still deferred, as its own read of
    // the render state, only where one may be.
    if (style_computer && style_engine.may_have_deferred_geometry_transaction())
        style_computer->document().flush_deferred_style_change_event();
}

void StyleEngine::note_pending_arrivals(size_t count)
{
    // An arrival is recorded as the input is next submitted, where the transaction a geometry read deferred must not
    // be waiting: it is flushed here, where the insertion would have recorded the arrival.
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    m_pending_arrival_count += count;
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
    note_style_node_arrived_or_retired(StyleNodeID { arrival.node });
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

void StyleEngine::record_derived_element_style_input_change(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node != 0 && reaction != 0) {
        flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
        request_frame_for_first_recorded_input(*this, m_style_computer);
        record_derived_element_style_input(style_node, reaction, inherited_style_groups);
    }
}

void StyleEngine::record_container_query_input_change(StyleNodeID style_node)
{
    if (style_node == 0)
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    record_container_query_input(style_node);
}

void StyleEngine::record_flat_tree_descendant_style_input_changes(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node == 0 || reaction == 0)
        return;

    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    // The relation columns must include every tree delta recorded before this derived action.
    submit_recorded_input();
    request_frame_for_first_recorded_input(*this, m_style_computer);
    record_flat_tree_descendant_style_inputs(style_node, reaction, inherited_style_groups);
}

void StyleEngine::record_size_container_query_dependents(StyleNodeID container)
{
    if (container == 0)
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    record_size_container_query_dependent_inputs(container);
}

void StyleEngine::evaluate_size_containers_needing_evaluation_after_layout(Layout::BegunRead const& read)
{
    if (!has_size_containers_needing_evaluation_after_layout(read))
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    record_dependent_inputs_of_size_containers_needing_evaluation_after_layout();
}

Vector<StyleNodeID> StyleEngine::viewport_dependent_style_nodes(Layout::BegunRead const& read)
{
    Vector<StyleNodeID> nodes;
    StyleEngineFFI::style_engine_viewport_dependent_nodes(m_render_document->host(), &read, &nodes, [](void* context, u32 node) {
        static_cast<Vector<StyleNodeID>*>(context)->append(StyleNodeID { node });
    });
    return nodes;
}

void StyleEngine::record_benchmark_marker(Utf16View name)
{
    auto const* data = name.has_ascii_storage()
        ? static_cast<void const*>(name.bytes().data())
        : static_cast<void const*>(name.utf16_span().data());
    StyleEngineFFI::style_engine_record_benchmark_marker(host(), data, name.length_in_code_units(), name.has_ascii_storage());
}

bool StyleEngine::has_recorded_input() const
{
    return m_pending_arrival_count > 0
        || !m_tree_deltas.is_empty()
        || !m_element_arrivals.is_empty()
        || !m_local_feature_deltas.is_empty()
        || !m_state_deltas.is_empty()
        || !m_element_declaration_deltas.is_empty();
}

void StyleEngine::submit_recorded_input()
{
    // Submitting what was recorded is the host's own read of the render state.
    Layout::ForcedReadScope read { render_document(), false };
    if (m_style_computer) {
        take_in_pending_style_arrivals(m_style_computer->document());
        publish_pending_element_features(*this, *m_style_computer);
    }
    if (!has_recorded_input()) {
        if (refresh_attribute_value_text_requirements(read) && m_style_computer)
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
        .element_style_inputs = nullptr,
        .element_style_input_count = 0,
    };
    apply_transaction(transaction);

    m_tree_deltas.clear_with_capacity();
    m_element_arrivals.clear_with_capacity();
    m_arrival_custom_state_atoms.clear_with_capacity();
    m_local_feature_deltas.clear_with_capacity();
    m_state_deltas.clear_with_capacity();
    m_element_declaration_deltas.clear_with_capacity();

    // Selector demand can arrive while the program change and element facts are still staged.
    // Refresh after applying the fact batch, then backfill values before matching observes it.
    if (refresh_attribute_value_text_requirements(read) && m_style_computer)
        publish_required_attribute_value_texts(*this, *m_style_computer);
}

void StyleEngine::apply_transaction(InputTransaction const& transaction)
{
    StyleEngineFFI::style_engine_apply_transaction(m_render_document->host(), &transaction);
}

void StyleEngine::flush()
{
    submit_recorded_input();
    StyleEngineFFI::style_engine_flush(m_render_document->host());
}

bool StyleEngine::take_diagnostic_style_transaction(Layout::BegunRead const& read, StyleNodeID root, Function<void(ReadonlySpan<StyleNodeID>)>&& consume)
{
    Vector<StyleNodeID> reaction_nodes;
    auto take_reaction_nodes = [&](auto const& reactions) {
        for (auto const& reaction : reactions) {
            // A pseudo-element record is part of its element's reaction.
            if (reaction.pseudo_kind != NumericLimits<u8>::max())
                continue;
            reaction_nodes.append(StyleNodeID { reaction.style_node });
        }
    };
    auto transaction = take_style_transaction(read, root);
    take_reaction_nodes(transaction.reactions);
    // A pass the host would install in waves reports every wave.
    while (StyleEngineFFI::style_engine_has_suspended_style_pass(m_render_document->host(), &read))
        take_reaction_nodes(take_style_transaction(read, root).reactions);
    discard_style_transaction_outputs(read);
    if (!transaction.is_scoped)
        return false;
    consume(reaction_nodes.span());
    return true;
}

void StyleEngine::discard_style_transaction_outputs(Layout::BegunRead const& read)
{
    // The end of the transaction releases the identities no reader can name any more, which are minted again first.
    StyleEngineFFI::style_engine_end_style_transaction(m_render_document->host(), &read, m_style_node_ids);
}

namespace {

struct StyleSheetResourceContextCollection {
    GC::Ref<DOM::Document> document;
    // The base URL of the document's sheets that have neither a base URL nor a location of their
    // own, which StyleSheetState::style_resource_base_url() resolves against the document.
    String const& document_api_base_url;
    HashTable<StyleSheetState const*> collected {};
    Vector<CollectedStyleSheetResourceContext> contexts {};
};

Optional<String> style_resource_base_url(StyleSheetState const& sheet, StyleSheetResourceContextCollection const& collection)
{
    if (!sheet.base_url().has_value() && !sheet.location().has_value() && sheet.owning_document() == collection.document)
        return collection.document_api_base_url;
    if (auto base_url = sheet.style_resource_base_url(); base_url.has_value())
        return base_url->to_string();
    return {};
}

void collect_style_sheet_resource_context(StyleSheetState& sheet, StyleSheetResourceContextCollection& collection)
{
    // A sheet's context is its own, whichever scope reaches it: a constructed sheet adopted by many
    // shadow roots is collected once.
    if (collection.collected.set(&sheet) != AK::HashSetResult::InsertedNewEntry)
        return;
    auto base_url = style_resource_base_url(sheet, collection);
    collection.contexts.append({
        .source_identity = sheet.native_sheet().identity(),
        .base_url = base_url.value_or(String {}),
        .has_base_url = base_url.has_value(),
        .origin_clean = sheet.is_origin_clean(),
    });
    for (auto const& import : sheet.import_rules()) {
        if (auto* imported = import->loaded_style_sheet())
            collect_style_sheet_resource_context(*imported, collection);
    }
    // A sheet whose rules are compiled from a shared snapshot has those rules name the snapshot's
    // native sheet, which shares the sheet's base URL.
    if (auto* shared = sheet.shared_compiled_style_sheet(); shared && &shared->contents() != &sheet)
        collect_style_sheet_resource_context(shared->contents(), collection);
}

Vector<CollectedStyleSheetResourceContext> collect_style_sheet_resource_contexts(DOM::Document& document, String const& document_api_base_url)
{
    StyleSheetResourceContextCollection collection { .document = document, .document_api_base_url = document_api_base_url };
    Function<void(StyleSheetState&)> collect = [&](StyleSheetState& sheet) { collect_style_sheet_resource_context(sheet, collection); };
    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User, CascadeOrigin::Author })
        document.style_scope().for_each_stylesheet(origin, collect);
    document.for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        shadow_root.style_scope().for_each_stylesheet(CascadeOrigin::Author, collect);
    });
    return move(collection.contexts);
}

}

// What a style transaction's document computation inputs lend the engine for the call that takes them, which copies
// what it keeps.
struct StyleEngine::LentComputationInputs {
    StyleEngineFFI::FfiDocumentStyleComputationInputs inputs {};
    String document_base_url;
    Vector<StyleEngineFFI::FfiStyleSheetResourceContextEntry> resource_contexts;
    Vector<StyleEngineFFI::FfiCustomFunctionEntry> custom_functions;
};

void StyleEngine::gather_computation_inputs(Layout::BegunRead const& read, LentComputationInputs& lent)
{
    if (!m_style_computer)
        return;
    auto& document = m_style_computer->document();
    document.publish_animation_keyframes_for_style_update(read);
    lent.document_base_url = document.serialized_base_url();
    auto document_api_base_url = HTML::relevant_settings_object(document).api_base_url().to_string();
    if (!m_style_sheet_resource_contexts.has_value()
        || m_style_sheet_resource_contexts->style_sheet_set_generation != document.style_sheet_set_generation()
        || m_style_sheet_resource_contexts->document_api_base_url != document_api_base_url) {
        m_style_sheet_resource_contexts = StyleSheetResourceContexts {
            .contexts = collect_style_sheet_resource_contexts(document, document_api_base_url),
            .style_sheet_set_generation = document.style_sheet_set_generation(),
            .document_api_base_url = move(document_api_base_url),
        };
    }
    auto const& collected_resource_contexts = m_style_sheet_resource_contexts->contexts;
    lent.resource_contexts.ensure_capacity(collected_resource_contexts.size());
    for (auto const& context : collected_resource_contexts) {
        lent.resource_contexts.unchecked_append({
            .source_identity = context.source_identity,
            .base_url = context.base_url.bytes().data(),
            .base_url_length = context.base_url.bytes().size(),
            .has_base_url = context.has_base_url,
            .origin_clean = context.origin_clean,
        });
    }
    auto const viewport_rect = m_style_computer->viewport_rect_for_style_environment();
    auto const& media_environment = *m_style_computer->ensure_media_environment_for_style_update();
    // What each scope's custom function calls name, published after the media environment is
    // settled: a media change rebuilds the definitions. A definition is seen only below a
    // scope holding @function rules, which most documents have none of.
    auto has_function_rules = [](StyleScope const& scope) { return !scope.rule_cache().function_rules_by_name.is_empty(); };
    bool document_has_function_rules = has_function_rules(document.style_scope());
    document.for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        document_has_function_rules = document_has_function_rules || has_function_rules(shadow_root.style_scope());
    });
    if (document_has_function_rules) {
        // A call in a function's body names what the function's own scope sees, so the scopes
        // that define what another sees publish what they see too.
        HashTable<StyleScope const*> visited_scopes;
        Vector<StyleScope const*> scopes;
        auto append_scope = [&](StyleScope const& scope) {
            if (visited_scopes.set(&scope) == AK::HashSetResult::InsertedNewEntry)
                scopes.append(&scope);
        };
        append_scope(document.style_scope());
        document.for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) { append_scope(shadow_root.style_scope()); });
        for (size_t index = 0; index < scopes.size(); ++index) {
            auto const& scope = *scopes[index];
            scope.for_each_visible_function_definition(read, [&](StyleScope::FunctionDefinitionAndScope const& definition) {
                lent.custom_functions.append({
                    .function = definition.function.handle(),
                    .caller_scope = bit_cast<FlatPtr>(&scope),
                    .definition_scope = bit_cast<FlatPtr>(&definition.scope),
                    .tree_scope = scope.style_engine_tree_scope().value(),
                });
                append_scope(definition.scope);
            });
        }
    }
    auto const& root_font_metrics = m_style_computer->root_element_font_metrics();
    auto const& initial_font = m_style_computer->document().font_computer().initial_font();
    Length::FontMetrics const initial_font_metrics { CSSPixels { initial_font.pixel_size() }, initial_font.pixel_metrics(), InitialValues::line_height() };
    lent.inputs = {
        .in_quirks_mode = m_style_computer->document().in_quirks_mode(),
        .viewport_width = viewport_rect.width().to_double(),
        .viewport_height = viewport_rect.height().to_double(),
        .root_font_size = root_font_metrics.font_size.to_double(),
        .root_font_x_height = root_font_metrics.x_height.to_double(),
        .root_font_cap_height = root_font_metrics.cap_height.to_double(),
        .root_font_zero_advance = root_font_metrics.zero_advance.to_double(),
        .root_line_height = root_font_metrics.line_height.to_double(),
        .root_font_metrics_depend_on_viewport_metrics = m_style_computer->root_element_font_metrics_depend_on_viewport_metrics(),
        .initial_font_size = initial_font_metrics.font_size.to_double(),
        .initial_font_x_height = initial_font_metrics.x_height.to_double(),
        .initial_font_cap_height = initial_font_metrics.cap_height.to_double(),
        .initial_font_zero_advance = initial_font_metrics.zero_advance.to_double(),
        .initial_font_size_raw = InitialValues::font_size().raw_value(),
        .default_font_size_raw = StyleComputer::default_user_font_size().raw_value(),
        .device_pixels_per_css_pixel = m_style_computer->document().page().client().device_pixels_per_css_pixel(),
        .font_environment_generation = m_style_computer->document().font_computer().environment_generation(),
        .preferred_color_scheme = static_cast<u8>(to_underlying(m_style_computer->document().page().preferred_color_scheme())),
        .has_document_supported_schemes = false,
        .document_supported_scheme_count = 0,
        .document_supported_scheme_codes = {},
        .custom_property_registry = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(m_style_computer->document().rust_custom_property_registry()),
        .custom_property_registration_generation = m_style_computer->document().custom_property_registration_generation(),
        .document_base_url = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(lent.document_base_url.bytes().data()),
        .document_base_url_length = lent.document_base_url.bytes().size(),
        .style_sheet_resource_contexts = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(lent.resource_contexts.data()),
        .style_sheet_resource_context_count = lent.resource_contexts.size(),
        .media_feature_values = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(media_environment.values),
        .media_feature_value_count = media_environment.value_count,
        .media_length_resolution_context = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(media_environment.length_resolution_context),
        .custom_functions = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(lent.custom_functions.data()),
        .custom_function_count = lent.custom_functions.size(),
    };
    if (auto supported = m_style_computer->document().supported_color_schemes(); supported.has_value()) {
        lent.inputs.has_document_supported_schemes = true;
        for (auto const& scheme : *supported) {
            auto preferred_scheme = preferred_color_scheme_from_string(scheme);
            if (preferred_scheme == PreferredColorScheme::Auto)
                continue;
            auto code = static_cast<u8>(to_underlying(preferred_scheme));
            auto supported_codes = Span<u8> { lent.inputs.document_supported_scheme_codes };
            if (supported_codes.trim(lent.inputs.document_supported_scheme_count).contains_slow(code))
                continue;
            VERIFY(lent.inputs.document_supported_scheme_count < supported_codes.size());
            supported_codes[lent.inputs.document_supported_scheme_count++] = code;
        }
    }
}

StyleEngine::PublishedStyleTransaction StyleEngine::take_style_transaction(Layout::BegunRead const& read, StyleNodeID root)
{
    auto submission_started_at = MonotonicTime::now();
    submit_recorded_input();
    LentComputationInputs lent;
    gather_computation_inputs(read, lent);
    auto bridge_started_at = MonotonicTime::now();
    if (m_style_computer)
        publish_font_faces(m_style_computer->document().font_computer());
    auto view = StyleEngineFFI::style_engine_take_style_transaction(m_render_document->host(), &read, root.value(), lent.inputs);
    return publish_style_transaction_view(view, submission_started_at, bridge_started_at);
}

StyleEngine::PublishedStyleTransaction StyleEngine::take_flown_style_transaction(Layout::BegunRead const& read)
{
    auto submission_started_at = MonotonicTime::now();
    // What was recorded beside the transaction is queued behind its drain, as the next transaction's input.
    submit_recorded_input();
    auto bridge_started_at = MonotonicTime::now();
    auto view = StyleEngineFFI::style_engine_take_flown_style_transaction(m_render_document->host(), &read);
    // The transaction's reactions were read inside the style record view epoch its seal opened.
    if (m_style_computer)
        m_style_computer->end_style_record_view_epoch();
    return publish_style_transaction_view(view, submission_started_at, bridge_started_at);
}

void StyleEngine::end_flown_style_drain()
{
    m_style_nodes_beside_flown_transaction.clear();
    StyleEngineFFI::style_engine_end_flown_style_drain(m_render_document->host());
    // Behind the writes made beside the transaction, the children it counted whose siblings changed beside it are
    // counted again.
    for (auto parent : exchange(m_parents_whose_children_changed_beside_flown_transaction, {})) {
        if (auto element = m_style_computer->element_for_style_node(parent); element && element->child_style_uses_tree_counting_function())
            restyle_children_reading_sibling_position(*element);
    }
    // So are the elements whose styles use a font that resolves differently since the transaction flew.
    m_style_computer->document().font_computer().did_end_flown_style_drain();
}

void StyleEngine::restyle_children_reading_sibling_position(DOM::Element& parent)
{
    parent.for_each_child_of_type<DOM::Element>([&](DOM::Element& element) {
        // The engine recomputes the element's record against its new place among its siblings.
        if (element.style_uses_tree_counting_function())
            record_derived_element_style_input_change(element.style_node_id(), PublishedStyle | RecomputeStyle);
        return IterationDecision::Continue;
    });
}

bool StyleEngine::has_flown_style_transaction() const
{
    return m_style_computer && m_style_computer->document().has_flown_style_transaction();
}

void StyleEngine::note_style_node_arrived_or_retired(StyleNodeID style_node)
{
    if (has_flown_style_transaction())
        m_style_nodes_beside_flown_transaction.set(style_node);
}

StyleEngine::PublishedStyleTransaction StyleEngine::publish_style_transaction_view(StyleEngineFFI::FfiStyleTransactionView const& view, MonotonicTime submission_started_at, MonotonicTime bridge_started_at)
{
    auto bridge_microseconds = (MonotonicTime::now() - bridge_started_at).to_truncated_microseconds();
    if (view.reclaimed_style_atom_count != 0) {
        HashTable<StyleAtomID> reclaimed_atoms;
        reclaimed_atoms.ensure_capacity(view.reclaimed_style_atom_count);
        for (auto const& reclaimed : ReadonlySpan<StyleEngineFFI::FfiReclaimedStyleAtom> { view.reclaimed_style_atoms, view.reclaimed_style_atom_count }) {
            auto atom_id = StyleAtomID { reclaimed.atom };
            reclaimed_atoms.set(atom_id);
            m_published_language_atoms.remove(atom_id);
            m_published_custom_property_names.remove(atom_id);
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
        .only_derived_child_reactions = view.only_derived_child_reactions,
        .connected_element_count = view.connected_element_count,
        .submission_microseconds = static_cast<u64>((bridge_started_at - submission_started_at).to_truncated_microseconds()),
        .bridge_microseconds = static_cast<u64>(bridge_microseconds),
    };
}

bool StyleEngine::let_style_transaction_fly(Layout::BegunRead const& read, StyleNodeID root, Layout::RustFFI::FfiFlightBlocker blocker)
{
    if (!m_style_computer)
        return false;
    submit_recorded_input();
    LentComputationInputs lent;
    gather_computation_inputs(read, lent);
    publish_font_faces(m_style_computer->document().font_computer());
    // The reactions are read inside the style record view epoch the transaction is taken in, which stays open until
    // take_style_transaction() takes them.
    m_style_computer->begin_style_record_view_epoch();
    // The frame the transaction flies in runs the first round of the update's layout after it.
    m_style_computer->document().seal_first_layout_round(read);
    if (StyleEngineFFI::style_engine_let_style_transaction_fly(m_render_document->host(), root.value(), lent.inputs, blocker))
        return true;
    m_style_computer->end_style_record_view_epoch();
    return false;
}

bool StyleEngine::style_transaction_flies()
{
    return StyleEngineFFI::style_engine_style_transaction_flies(m_render_document->host());
}

bool StyleEngine::frame_marked_relayout(StyleNodeID style_node, StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_frame_marked_relayout(m_render_document->host(), style_node.value(), style_record.value());
}

void StyleEngine::sort_style_deltas_for_direct_application(Layout::BegunRead const& read, Span<PublishedStyleDelta> deltas) const
{
    StyleEngineFFI::style_engine_sort_style_deltas_for_direct_application(host(), &read, deltas.data(), deltas.size());
}

bool StyleEngine::has_pending_transaction(Layout::BegunRead const& read) const
{
    return has_recorded_input() || has_flown_style_transaction() || StyleEngineFFI::style_engine_has_pending_transaction(m_render_document->host(), &read);
}

bool StyleEngine::pending_transaction_may_affect_layout_geometry(Layout::BegunRead const& read)
{
    submit_recorded_input();
    return StyleEngineFFI::style_engine_pending_transaction_may_affect_layout_geometry(m_render_document->host(), &read);
}

bool StyleEngine::has_deferred_geometry_transaction(Layout::BegunRead const& read) const
{
    // Only a geometry read defers a transaction, so until one has, every input recorded meanwhile is spared the
    // question.
    if (!m_geometry_read_deferred_transaction)
        return false;
    m_geometry_read_deferred_transaction = StyleEngineFFI::style_engine_has_deferred_geometry_transaction(m_render_document->host(), &read);
    return m_geometry_read_deferred_transaction;
}

bool StyleEngine::has_deferred_element_style_inputs(Layout::BegunRead const& read) const
{
    return StyleEngineFFI::style_engine_has_deferred_element_style_inputs(m_render_document->host(), &read);
}

bool StyleEngine::has_deferred_element_style_input(Layout::BegunRead const& read, StyleNodeID style_node) const
{
    return StyleEngineFFI::style_engine_has_deferred_element_style_input(m_render_document->host(), &read, style_node.value());
}

bool StyleEngine::defer_pending_transaction_for_geometry_read(Layout::BegunRead const& read)
{
    submit_recorded_input();
    m_geometry_read_deferred_transaction = true;
    return StyleEngineFFI::style_engine_defer_pending_transaction_for_geometry_read(m_render_document->host(), &read);
}

bool StyleEngine::begin_deferred_geometry_transaction_flush(Layout::BegunRead const& read)
{
    submit_recorded_input();
    return StyleEngineFFI::style_engine_begin_deferred_geometry_transaction_flush(m_render_document->host(), &read);
}

void StyleEngine::end_deferred_geometry_transaction_flush()
{
    StyleEngineFFI::style_engine_end_deferred_geometry_transaction_flush(m_render_document->host());
}

bool StyleEngine::match_element(Layout::BegunRead const& read, StyleNodeID node, Vector<RuleMatch>& matches, MatchPurpose purpose)
{
    // A synchronous match is an observation boundary. Most matching follows a published style
    // transaction, but detached-document style reads can arrive directly while mutation facts are
    // still staged. Settle those facts before asking the committed arrangement.
    if (has_pending_transaction(read))
        flush();
    matches.resize(max(m_element_match_capacity, 16u));
    auto match = [&] {
        return StyleEngineFFI::style_engine_match_element(host(), &read, node.value(), matches.data(), matches.size(), purpose == MatchPurpose::Cascade);
    };
    auto count = match();
    if (count == NumericLimits<size_t>::max())
        return false;
    if (count > matches.size()) {
        // Nothing was written, so grow and ask again rather than reporting a truncated answer.
        m_element_match_capacity = count * 2;
        matches.resize(m_element_match_capacity);
        count = match();
        if (count == NumericLimits<size_t>::max() || count > matches.size())
            return false;
    }
    matches.shrink(count);
    return true;
}

bool StyleEngine::counter(Layout::BegunRead const& read, size_t index, StringView& out_name, u64& out_value) const
{
    size_t name_length = 0;
    auto const* name = StyleEngineFFI::style_engine_counter(m_render_document->host(), &read, index, &out_value, &name_length);
    if (!name)
        return false;
    out_name = StringView { name, name_length };
    return true;
}

void StyleEngine::set_element_custom_property_data(Layout::BegunRead const& read, DOM::Element const& element, CustomPropertyData const* data)
{
    // A move of the environment the element inherits reads whether this is its animation overlay, and
    // whether what the element's style resolves to declares custom properties of its own.
    bool const is_animation_overlay = data && data->is_animation_overlay_for({ element });
    auto const* base = is_animation_overlay ? data->parent().ptr() : data;
    // The engine resolves the children's environments over what the element hands down, and substitutes the
    // element's own values under what its animations sampled, laid over what its style resolves to.
    auto note = [&](CustomPropertyData const& environment) {
        auto inheritable = environment.inheritable(read, element.document());
        StyleEngineFFI::style_engine_note_custom_property_environment(host(), &read, environment.identity(), environment.rust_store(),
            inheritable ? inheritable->identity() : 0, inheritable ? inheritable->rust_store() : nullptr);
    };
    if (base)
        note(*base);
    if (is_animation_overlay)
        note(*data);
    StyleEngineFFI::style_engine_set_element_custom_property_data(m_render_document->host(), element.style_node_id().value(), data, data ? data->identity() : 0, is_animation_overlay, base && base->declared_count() > 0,
        base ? base->identity() : 0);
}

CustomPropertyData const* StyleEngine::element_custom_property_data(Layout::BegunRead const& read, StyleNodeID node) const
{
    return static_cast<CustomPropertyData const*>(StyleEngineFFI::style_engine_element_custom_property_data(m_render_document->host(), &read, node.value()));
}

static_assert(to_underlying(PseudoElement::KnownPseudoElementCount) <= 64);

void StyleEngine::set_pseudo_element_custom_property_data(StyleNodeID node, PseudoElement pseudo_element, CustomPropertyData const* data)
{
    StyleEngineFFI::style_engine_set_pseudo_element_custom_property_data(m_render_document->host(), node.value(), to_underlying(pseudo_element), data, data ? data->identity() : 0);
}

CustomPropertyData const* StyleEngine::pseudo_element_custom_property_data(Layout::BegunRead const& read, StyleNodeID node, PseudoElement pseudo_element) const
{
    return static_cast<CustomPropertyData const*>(StyleEngineFFI::style_engine_pseudo_element_custom_property_data(m_render_document->host(), &read, node.value(), to_underlying(pseudo_element)));
}

u64 StyleEngine::pseudo_elements_with_custom_property_data(Layout::BegunRead const& read, StyleNodeID node) const
{
    return StyleEngineFFI::style_engine_pseudo_elements_with_custom_property_data(m_render_document->host(), &read, node.value());
}

// The engine resolves fonts against the @font-face table and cascade memo it was given, at the generation of the
// computation inputs it was given with them. It names the @font-feature-values of the nearest of the shadow tree scopes
// declaring some around an element, so it is given those scopes too.
void StyleEngine::publish_font_faces(FontComputer const& font_computer)
{
    if (m_published_font_environment_generation == font_computer.environment_generation())
        return;
    m_published_font_environment_generation = font_computer.environment_generation();
    // The engine resolves against this table from here on, and nothing it named from an older one is read again.
    font_computer.font_cascade_memo().release_retired();
    auto snapshot = font_computer.font_face_snapshot();
    auto shadow_scopes = snapshot->font_feature_values_shadow_scopes();
    StyleEngineFFI::style_engine_publish_font_faces(m_render_document->host(), &snapshot.leak_ref(), &NonnullRefPtr { font_computer.font_cascade_memo() }.leak_ref(), reinterpret_cast<u32 const*>(shadow_scopes.data()), shadow_scopes.size());
}

}
