/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
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
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

static_assert(StyleEngineFFI::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND == to_underlying(first_element_reference_pseudo_element));
static_assert(StyleEngineFFI::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND == to_underlying(last_element_reference_pseudo_element));
static_assert(!IsMoveConstructible<StyleEngine>);
static_assert(!IsMoveAssignable<StyleEngine>);

StyleEngine::StyleEngine(StyleComputer* style_computer)
    : m_render_document(Layout::RenderDocument::create())
    , m_style_computer(style_computer)
{
}

StyleEngine::~StyleEngine() = default;

void StyleEngine::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_style_computer);
}

// More nodes than this waiting to arrive at once are found along the marks instead.
static constexpr size_t max_listed_arrivals = 4096;

void StyleEngine::list_arrival(DOM::Node& node)
{
    if (m_arrivals_unlisted)
        return;
    if (m_listed_arrivals.size() == max_listed_arrivals) {
        m_arrivals_unlisted = true;
        m_listed_arrivals.clear();
        return;
    }
    m_listed_arrivals.append(GC::Weak<DOM::Node> { node });
}

Optional<GC::RootVector<GC::Ref<DOM::Node>>> StyleEngine::take_listed_arrivals()
{
    auto listed = move(m_listed_arrivals);
    if (exchange(m_arrivals_unlisted, false))
        return {};
    GC::RootVector<GC::Ref<DOM::Node>> nodes;
    nodes.ensure_capacity(listed.size());
    for (auto const& node : listed) {
        if (auto pointer = node.ptr())
            nodes.unchecked_append(*pointer);
    }
    return nodes;
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
    Span<u32> raw_nodes { reinterpret_cast<u32*>(nodes.data()), nodes.size() };
    StyleEngineFFI::style_engine_mint_style_nodes(m_render_document->host(), false, raw_nodes);
}

void StyleEngine::mint_text_style_nodes(Span<StyleNodeID> nodes)
{
    if (nodes.is_empty())
        return;
    Span<u32> raw_nodes { reinterpret_cast<u32*>(nodes.data()), nodes.size() };
    StyleEngineFFI::style_engine_mint_style_nodes(m_render_document->host(), true, raw_nodes);
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

SheetID StyleEngine::add_sheet(StyleEngineFFI::FfiCascadeOrigin origin)
{
    return SheetID { StyleEngineFFI::style_engine_add_sheet(m_render_document->host(), origin) };
}

void StyleEngine::finish_sheet_rules_replacement(SheetID sheet)
{
    StyleEngineFFI::style_engine_finish_sheet_rules_replacement(host(), sheet, StyleEngineFFI::style_engine_next_declaration_block_version(host()));
}

void StyleEngine::set_element_inline_style_properties(StyleNodeID node, Parser::ValueParserFFI::DeclarationBlock const* declarations)
{
    if (StyleEngineFFI::style_engine_set_element_inline_style_properties(host(), node.value(), declarations))
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

StyleRecordID StyleEngine::install_host_style_record(Layout::BegunRead const& read, ReadonlySpan<void const*> payloads, ComputedValuesFFI::InheritedSetValues inherited, ComputedValuesFFI::ResetBits reset, u64 custom_property_environment, u64 counter_style_environment_identity, void const* source_record, void const* custom_property_store)
{
    return StyleRecordID { StyleEngineFFI::style_engine_install_host_style_record(host(), &read, payloads.data(), payloads.size(), inherited.bits, inherited.effective_zoom_bits, reset, custom_property_environment, counter_style_environment_identity, source_record, custom_property_store) };
}

void const* StyleEngine::host_record(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    return StyleEngineFFI::style_engine_host_record(host(), &read, style_record.value());
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

#define ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(name) \
    static_assert(to_underlying(StyleEngine::DemandedPseudoElement::name) == to_underlying(PseudoElement::name));
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(After)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(SearchText)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(SearchTextCurrent)
ASSERT_DEMANDED_PSEUDO_ELEMENT_KIND(SpellingError)
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
    // FIXME: Compute the styles of ::highlight() pseudo-elements for each custom highlight name.
    case PseudoElement::Highlight:
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

StyleAtomID StyleEngine::intern_atom(Utf16FlyString const& name)
{
    return StyleAtomID { StyleEngineFFI::document_host_intern_atom(host(), name.raw_identity()) };
}

u64 StyleEngine::atom_generation() const
{
    return StyleEngineFFI::document_host_atom_generation(host());
}

StyleAtomID StyleEngine::intern_text_atom(Utf16View text)
{
    return intern_atom(Utf16FlyString::from_utf16(text).to_ascii_lowercase());
}

StyleAtomID StyleEngine::intern_language_atom(Utf16View text)
{
    // OPTIMIZATION: Elements arrive in tree order, and most of them have the language of the one before, which the
    //               engine was given the tag of already, unless the atom was reclaimed since.
    auto generation = atom_generation();
    if (m_last_language_atom.value() != 0 && m_last_language_atom_generation == generation && m_last_language_text.utf16_view() == text)
        return m_last_language_atom;
    auto atom = intern_text_atom(text);
    // The engine is given the language's tag the first time the host names it.
    StyleEngineFFI::style_engine_set_element_language(host(), 0, atom.value(), StyleEngineFFI::ffi_utf16_view(text));
    m_last_language_text = Utf16String::from_utf16(text);
    m_last_language_atom = atom;
    m_last_language_atom_generation = generation;
    return atom;
}

void StyleEngine::publish_html_element_namespace(StyleAtomID namespace_atom)
{
    // NB: The engine keeps the atom it was told alive, so an equal one names the same namespace.
    if (exchange(m_html_element_namespace, namespace_atom) != namespace_atom)
        StyleEngineFFI::style_engine_set_html_element_namespace(host(), namespace_atom);
}

StyleEngine::WithheldFeaturesNowRead StyleEngine::take_withheld_features_now_read(Layout::BegunRead const& read)
{
    WithheldFeaturesNowRead now_read;
    if (m_withheld_local_link_states.is_empty() && m_withheld_attribute_carriers.is_empty())
        return now_read;
    StyleEngineFFI::document_host_learn_selector_requirements(host(), &read);
    if (!m_withheld_local_link_states.is_empty() && !local_link_is_known_untested())
        now_read.local_link_states = move(m_withheld_local_link_states);
    Vector<WithheldAttributeName> names_now_read;
    for (auto const& [name, carriers] : m_withheld_attribute_carriers) {
        if (!attribute_is_known_unread(intern_attribute_name(name.local_name, name.namespace_uri)))
            names_now_read.append(name);
    }
    for (auto const& name : names_now_read)
        now_read.attributes.set(name, m_withheld_attribute_carriers.take(name).release_value());
    return now_read;
}

void StyleEngine::publish_attribute_value_texts_if_requirements_moved(Layout::BegunRead const& read)
{
    StyleEngineFFI::document_host_refresh_attribute_value_text_requirements(host(), &read);
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
    set_rendering_pending(style_computer->document(), RenderingPendingReason::NeedsRenderingUpdate, 1);
    style_computer->document().page().client().request_frame();
}

static void flush_deferred_geometry_transaction_before_non_replayable_input(StyleEngine const& style_engine, GC::Ptr<StyleComputer> style_computer)
{
    // The flush asks the engine whether the transaction a geometry read deferred is still deferred, only where one may
    // be.
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

void StyleEngine::note_waiting_arrivals_may_arrive()
{
    request_frame_for_first_recorded_input(*this, m_style_computer);
    ++m_pending_arrival_count;
}

void StyleEngine::note_waiting_arrivals_if_keys_moved(Layout::BegunRead const& read)
{
    if (m_may_have_waiting_children && m_waiting_children_checked_against_keys_version != relational_arrival_keys(read).version)
        note_waiting_arrivals_may_arrive();
}

namespace {

#include <LibWeb/SelectorStateFactsGenerated.inc>

}

StyleEngine::RelationalArrivalKeys const& StyleEngine::relational_arrival_keys(Layout::BegunRead const& read)
{
    auto keys = StyleEngineFFI::style_engine_relational_arrival_keys(host(), &read);
    if (keys.version == m_relational_arrival_keys.version)
        return m_relational_arrival_keys;
    m_relational_arrival_keys.version = keys.version;
    m_relational_arrival_keys.reached_by_any_arrival = keys.reached_by_any_arrival;
    m_relational_arrival_keys.names.clear();
    for (size_t i = 0; i < keys.name_count; ++i)
        m_relational_arrival_keys.names.set(keys.names[i]);
    m_relational_arrival_keys.state_pseudo_classes.clear();
    for (size_t i = 0; i < keys.state_count; ++i)
        m_relational_arrival_keys.state_pseudo_classes.append(pseudo_classes_by_state_fact[keys.states[i]]);
    return m_relational_arrival_keys;
}

void StyleEngine::record_tree_delta(StyleEngineFFI::FfiTreeDelta const& delta)
{
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_stage_tree_delta(host(), &delta);
}

void StyleEngine::record_element_arrivals(ReadonlySpan<StyleEngineFFI::FfiArrivingElement> elements)
{
    if (elements.is_empty())
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_stage_element_arrivals(host(), elements);
}

void StyleEngine::record_element_arrival(StyleEngineFFI::FfiElementArrival const& arrival, ReadonlySpan<StyleAtomID> custom_states, ReadonlySpan<StyleAtomID> classes, ReadonlySpan<StyleAtomID> attributes)
{
    VERIFY(custom_states.size() == arrival.custom_state_count && classes.size() == arrival.class_count && attributes.size() == 2 * static_cast<size_t>(arrival.attribute_count));
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    static_assert(sizeof(StyleAtomID) == sizeof(u32));
    StyleEngineFFI::style_engine_stage_element_arrival(host(), &arrival, reinterpret_cast<u32 const*>(custom_states.data()), reinterpret_cast<u32 const*>(classes.data()), reinterpret_cast<u32 const*>(attributes.data()));
}

void StyleEngine::record_local_feature_delta(StyleEngineFFI::FfiLocalFeatureDelta const& delta)
{
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_stage_local_feature_delta(host(), &delta);
}

void StyleEngine::record_state_delta(StyleEngineFFI::FfiStateDelta const& delta)
{
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_stage_state_delta(host(), &delta);
}

void StyleEngine::record_hover(Optional<StyleNodeID> target)
{
    if (!StyleEngineFFI::style_engine_request_hover(host(), target.value_or(StyleNodeID {}).value()))
        return;
    request_frame_for_first_recorded_input(*this, m_style_computer);
}

void StyleEngine::record_element_declaration_delta(StyleEngineFFI::FfiElementDeclarationDelta const& delta)
{
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_stage_element_declaration_delta(host(), &delta);
}

void StyleEngine::record_derived_element_style_input_change(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups)
{
    if (style_node != 0 && reaction != 0) {
        flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
        request_frame_for_first_recorded_input(*this, m_style_computer);
        StyleEngineFFI::style_engine_record_derived_element_style_input(host(), style_node, reaction, inherited_style_groups);
    }
}

void StyleEngine::set_highlight_observing_elements(PseudoElement pseudo_element, ReadonlySpan<StyleNodeID> style_nodes)
{
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_set_highlight_observing_elements(host(), to_underlying(pseudo_element), style_nodes);
}

void StyleEngine::record_container_query_input_change(StyleNodeID style_node)
{
    if (style_node == 0)
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_record_container_query_input(host(), style_node);
}

void StyleEngine::record_size_container_query_dependents(StyleNodeID container)
{
    if (container == 0)
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_record_size_container_query_dependents(host(), container);
}

void StyleEngine::evaluate_size_containers_needing_evaluation_after_layout(Layout::BegunRead const& read)
{
    if (!StyleEngineFFI::style_engine_has_size_containers_needing_evaluation_after_layout(host(), &read))
        return;
    flush_deferred_geometry_transaction_before_non_replayable_input(*this, m_style_computer);
    request_frame_for_first_recorded_input(*this, m_style_computer);
    StyleEngineFFI::style_engine_evaluate_size_containers_needing_evaluation_after_layout(host());
}

Vector<StyleNodeID> StyleEngine::viewport_dependent_style_nodes(Layout::BegunRead const& read, u8 moved_axes)
{
    Vector<StyleNodeID> nodes;
    StyleEngineFFI::style_engine_viewport_dependent_nodes(m_render_document->host(), &read, moved_axes, &nodes, [](void* context, u32 node) {
        static_cast<Vector<StyleNodeID>*>(context)->append(StyleNodeID { node });
    });
    return nodes;
}

bool StyleEngine::has_recorded_input() const
{
    return m_pending_arrival_count > 0 || StyleEngineFFI::style_engine_has_staged_input(host());
}

void StyleEngine::submit_recorded_input()
{
    if (m_style_computer)
        m_style_computer->document().assign_slottables_if_needed();
    Layout::ForcedReadScope read { render_document() };
    if (m_style_computer) {
        take_in_pending_style_arrivals(m_style_computer->document(), HiddenChildArrivals::MayWait);
        publish_pending_element_features(*this, *m_style_computer, read);
        publish_withheld_element_features(*this, *m_style_computer, read);
    }
    StyleEngineFFI::style_engine_submit_staged_input(host());

    // Selector demand can arrive while the program change and element facts are still staged.
    // Refresh after applying the fact batch, then backfill values before matching observes it.
    publish_attribute_value_texts_if_requirements_moved(read);
}

void StyleEngine::flush()
{
    submit_recorded_input();
    StyleEngineFFI::style_engine_flush(m_render_document->host());
}

void StyleEngine::discard_style_update_outputs()
{
    // The end of the transaction releases the identities no reader can name any more, which are minted again first.
    StyleEngineFFI::style_engine_end_style_update(m_render_document->host());
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

// What a style update's document computation inputs lend the engine for the call that takes them, which copies
// what it keeps.
struct StyleEngine::LentComputationInputs {
    StyleEngineFFI::FfiDocumentStyleComputationInputs inputs {};
    String document_base_url;
    Vector<StyleEngineFFI::FfiCustomFunctionEntry> custom_functions;
};

void StyleEngine::gather_computation_inputs(LentComputationInputs& lent)
{
    if (!m_style_computer)
        return;
    auto& document = m_style_computer->document();
    document.publish_animation_keyframes_for_style_update();
    lent.document_base_url = document.serialized_base_url();
    auto document_api_base_url = HTML::relevant_settings_object(document).api_base_url();
    if (!m_style_sheet_resource_contexts.has_value()
        || m_style_sheet_resource_contexts->style_sheet_set_generation != document.style_sheet_set_generation()
        || m_style_sheet_resource_contexts->document_api_base_url != document_api_base_url) {
        auto contexts = collect_style_sheet_resource_contexts(document, document_api_base_url.to_string());
        Vector<StyleEngineFFI::FfiStyleSheetResourceContextEntry> entries;
        entries.ensure_capacity(contexts.size());
        for (auto const& context : contexts) {
            entries.unchecked_append({
                .source_identity = context.source_identity,
                .base_url = context.base_url.bytes().data(),
                .base_url_length = context.base_url.bytes().size(),
                .has_base_url = context.has_base_url,
                .origin_clean = context.origin_clean,
            });
        }
        m_style_sheet_resource_contexts = StyleSheetResourceContexts {
            .contexts = move(contexts),
            .entries = move(entries),
            .style_sheet_set_generation = document.style_sheet_set_generation(),
            .document_api_base_url = move(document_api_base_url),
            .version = ++m_style_sheet_resource_contexts_version,
        };
    }
    auto const viewport_rect = m_style_computer->viewport_rect_for_style_environment();
    auto const& media_environment = *m_style_computer->ensure_media_environment_for_style_update();
    // Each scope's @function rules, published after the media environment is settled: a media
    // change rebuilds the definitions. The style engine decides which of them a call names. Most
    // documents hold none, and then no scope is published.
    auto has_function_rules = [](StyleScope const& scope) { return !scope.rule_cache().function_rules.is_empty(); };
    bool document_has_function_rules = has_function_rules(document.style_scope());
    document.for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        document_has_function_rules = document_has_function_rules || has_function_rules(shadow_root.style_scope());
    });
    if (document_has_function_rules) {
        auto& style_engine = m_style_computer->style_engine();
        auto publish_scope = [&](StyleScope const& scope) {
            StyleEngineFFI::FfiCustomFunctionEntry entry {
                .function = nullptr,
                .scope = bit_cast<FlatPtr>(&scope),
                .parent_scope = bit_cast<FlatPtr>(scope.parent_style_scope()),
                .tree_scope = scope.style_engine_tree_scope().value(),
                .layer = 0,
                .origin = 0,
            };
            auto const& rules = scope.rule_cache().function_rules;
            if (rules.is_empty())
                lent.custom_functions.append(entry);
            for (auto const& rule : rules) {
                entry.function = rule.rule.handle();
                entry.layer = rule.qualified_layer_name.is_empty() ? 0 : style_engine.intern_atom(rule.qualified_layer_name).value();
                entry.origin = cascade_origin_precedence(rule.cascade_origin);
                lent.custom_functions.append(entry);
            }
        };
        publish_scope(document.style_scope());
        document.for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) { publish_scope(shadow_root.style_scope()); });
    }
    auto const& root_font_metrics = m_style_computer->root_element_font_metrics();
    auto const& initial_font = m_style_computer->document().font_computer().initial_font();
    Length::FontMetrics const initial_font_metrics { CSSPixels { initial_font.pixel_size() }, initial_font.pixel_metrics(), normal_line_height(initial_font.pixel_metrics()) };
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
        .root_effective_zoom = m_style_computer->root_element_effective_zoom(),
        .initial_font_size = initial_font_metrics.font_size.to_double(),
        .initial_font_x_height = initial_font_metrics.x_height.to_double(),
        .initial_font_cap_height = initial_font_metrics.cap_height.to_double(),
        .initial_font_zero_advance = initial_font_metrics.zero_advance.to_double(),
        .initial_line_height = initial_font_metrics.line_height.to_double(),
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
        .style_sheet_resource_contexts = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(m_style_sheet_resource_contexts->entries.data()),
        .style_sheet_resource_context_count = m_style_sheet_resource_contexts->entries.size(),
        .style_sheet_resource_contexts_version = m_style_sheet_resource_contexts->version,
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

static StyleEngine::StyleUpdateTimings style_update_timings(MonotonicTime submission_started_at, MonotonicTime bridge_started_at)
{
    return {
        .submission_microseconds = static_cast<u64>((bridge_started_at - submission_started_at).to_microseconds()),
        .bridge_microseconds = static_cast<u64>((MonotonicTime::now() - bridge_started_at).to_microseconds()),
    };
}

StyleEngine::StyleUpdateTimings StyleEngine::run_style_update(Layout::BegunRead const& read, StyleNodeID root)
{
    auto submission_started_at = MonotonicTime::now();
    LentComputationInputs lent;
    submit_recorded_input();
    gather_computation_inputs(lent);
    auto bridge_started_at = MonotonicTime::now();
    if (m_style_computer)
        publish_font_faces(m_style_computer->document().font_computer());
    StyleEngineFFI::style_engine_run_style_update(m_render_document->host(), &read, root.value(), lent.inputs);
    return style_update_timings(submission_started_at, bridge_started_at);
}

StyleEngine::StyleUpdateTimings StyleEngine::drain_flown_style_update(Layout::BegunRead const& read)
{
    auto submission_started_at = MonotonicTime::now();
    // What was recorded beside the transaction is queued behind its drain, as the next transaction's input.
    submit_recorded_input();
    auto bridge_started_at = MonotonicTime::now();
    StyleEngineFFI::style_engine_drain_flown_style_update(m_render_document->host(), &read);
    // The transaction's reactions were read inside the style record view epoch its seal opened.
    if (m_style_computer)
        m_style_computer->end_style_record_view_epoch();
    return style_update_timings(submission_started_at, bridge_started_at);
}

void StyleEngine::end_flown_style_drain()
{
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

void StyleEngine::take_in_fonts_resolved_against_older_tables()
{
    if (!m_style_computer)
        return;
    // NB: A pass resolves against an older table than the memo's only while the one the engine holds is behind.
    auto& font_computer = m_style_computer->document().font_computer();
    if (m_published_font_environment_generation != font_computer.environment_generation())
        font_computer.take_in_resolutions_against_older_tables();
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

bool StyleEngine::has_flown_style_update() const
{
    return m_style_computer && m_style_computer->document().has_flown_style_update();
}

bool StyleEngine::let_style_update_fly(StyleUpdateInputs const&, StyleNodeID root, Layout::RustFFI::FfiFlightBlocker blocker)
{
    if (!m_style_computer)
        return false;
    submit_recorded_input();
    LentComputationInputs lent;
    gather_computation_inputs(lent);
    publish_font_faces(m_style_computer->document().font_computer());
    // The reactions are read inside the style record view epoch the transaction is taken in, which stays open until
    // run_style_update() takes them.
    m_style_computer->begin_style_record_view_epoch();
    if (StyleEngineFFI::style_engine_let_style_update_fly(m_render_document->host(), root.value(), lent.inputs, blocker))
        return true;
    m_style_computer->end_style_record_view_epoch();
    return false;
}

bool StyleEngine::has_pending_transaction(Layout::BegunRead const& read) const
{
    return has_recorded_input() || has_flown_style_update() || StyleEngineFFI::style_engine_has_pending_transaction(m_render_document->host(), &read);
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

void StyleEngine::defer_pending_transaction_for_geometry_read(Layout::BegunRead const& read)
{
    submit_recorded_input();
    m_geometry_read_deferred_transaction = true;
    StyleEngineFFI::style_engine_defer_pending_transaction_for_geometry_read(m_render_document->host(), &read);
}

bool StyleEngine::begin_deferred_geometry_transaction_flush(Layout::BegunRead const& read)
{
    submit_recorded_input();
    return StyleEngineFFI::style_engine_begin_deferred_geometry_transaction_flush(m_render_document->host(), &read);
}

bool StyleEngine::matched_rules(Layout::BegunRead const& read, StyleNodeID node, Vector<MatchedRule>& matches)
{
    // A synchronous match is an observation boundary. Most matching follows a published style
    // transaction, but detached-document style reads can arrive directly while mutation facts are
    // still staged. Settle those facts before asking the committed arrangement.
    if (has_pending_transaction(read))
        flush();
    auto raw_node = node.value();
    matches.resize(64);
    auto match = [&] {
        return StyleEngineFFI::style_engine_matched_rules(host(), &read, &raw_node, 1, matches.data(), matches.size(), true);
    };
    auto count = match();
    if (count > matches.size()) {
        // Nothing was written, so grow and ask again rather than reporting a truncated answer.
        matches.resize(count);
        count = match();
        VERIFY(count <= matches.size());
    }
    matches.shrink(count);
    // An element that could not be matched is reported with a rule of zero.
    return !(count == 1 && matches[0].rule == 0);
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

// Resolves the children's environments over what the element hands down, and substitutes the element's own values under
// what its animations sampled, laid over what its style resolves to: name the environment the element holds, and the
// one its animation overlay lies over, with what each hands down under the registrations as they are now.
void StyleEngine::note_element_custom_property_environments(Layout::BegunRead const& read, DOM::Element const& element, CustomPropertyData const& data, IsAnimationOverlay is_animation_overlay)
{
    auto note = [&](CustomPropertyData const& environment) {
        // NB: Many elements of an installation take one environment, which the render side keeps by identity.
        if (!element.document().style_computer().custom_property_environment_needs_note(environment))
            return;
        auto inheritable = environment.inheritable(read, element.document());
        StyleEngineFFI::style_engine_note_custom_property_environment(host(), environment.identity(), environment.rust_store(),
            inheritable ? inheritable->identity() : 0, inheritable ? inheritable->rust_store() : nullptr);
    };
    if (is_animation_overlay == IsAnimationOverlay::Yes) {
        if (auto base = data.parent())
            note(*base);
    }
    note(data);
}

void StyleEngine::set_element_custom_property_data(Layout::BegunRead const& read, DOM::Element const& element, CustomPropertyData const* data, IsAnimationOverlay is_animation_overlay)
{
    auto const* base = is_animation_overlay == IsAnimationOverlay::Yes ? data->parent().ptr() : data;
    if (data)
        note_element_custom_property_environments(read, element, *data, is_animation_overlay);
    StyleEngineFFI::style_engine_set_element_custom_property_data(m_render_document->host(), element.style_node_id().value(), data, data ? data->identity() : 0, is_animation_overlay == IsAnimationOverlay::Yes,
        base ? base->identity() : 0);
}

// The registrations decide what an environment hands down, and the engine resolves a child's environment over what
// its parent's hands down as the host last named it. Once the registrations change, name it again for every
// environment an element holds, so a child the engine styles from here on gets an environment the host can install.
void StyleEngine::republish_element_custom_property_environments(Layout::BegunRead const& read, DOM::Document& document)
{
    struct Context {
        StyleEngine& style_engine;
        Layout::BegunRead const& read;
        GC::Ref<DOM::Document> document;
    } context { *this, read, document };
    StyleEngineFFI::style_engine_for_each_element_custom_property_environment(host(), &read, &context, [](void* context_pointer, u32 style_node, void const* data) {
        auto& context = *static_cast<Context*>(context_pointer);
        auto element = context.document->style_computer().element_for_style_node(StyleNodeID { style_node });
        if (!element)
            return;
        context.style_engine.note_element_custom_property_environments(context.read, *element, *static_cast<CustomPropertyData const*>(data), element->custom_property_data_is_animation_overlay({}) ? IsAnimationOverlay::Yes : IsAnimationOverlay::No);
    });
}

static_assert(to_underlying(PseudoElement::KnownPseudoElementCount) <= 64);

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
