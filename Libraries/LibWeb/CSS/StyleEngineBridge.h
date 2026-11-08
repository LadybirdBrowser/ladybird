/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/ParkedRandomBaseValues.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::CSS::StyleEngineFFI {

using Layout::RustFFI::BegunRead;
using Layout::RustFFI::DocumentHost;

// The entries of the table in Rust/src/css/style/boundary.rs, which cbindgen cannot see: a macro expands to them. A
// test there checks each declaration against its row. An AK Span crosses by value as the table's FfiSpan.
extern "C" {
void style_engine_set_highlight_observing_elements(DocumentHost const*, u8 kind, ReadonlySpan<StyleNodeID> nodes);
void style_engine_set_custom_highlight_observing_elements(DocumentHost const*, ReadonlySpan<StyleNodeID> nodes, ReadonlySpan<StyleAtomID> names);
void style_engine_set_fold_id_and_class_name_case(DocumentHost const*, bool fold);
void style_engine_set_html_element_namespace(DocumentHost const*, StyleAtomID namespace_atom);
void style_engine_mark_relation_only_style_node(DocumentHost const*, StyleNodeID node);
void style_engine_link_style_nodes_in_dom_order(DocumentHost const*, ReadonlySpan<u32> links);
void style_engine_unlink_style_node_from_dom_order(DocumentHost const*, StyleNodeID node, StyleNodeID parent);
void style_engine_retire_text_style_nodes(DocumentHost const*, ReadonlySpan<StyleNodeID> nodes);
void style_engine_set_slot_assigned_nodes(DocumentHost const*, StyleNodeID slot, ReadonlySpan<StyleNodeID> assigned);
void style_engine_set_top_layer_elements(DocumentHost const*, ReadonlySpan<StyleNodeID> members);
void style_engine_set_text_is_ascii_whitespace(DocumentHost const*, StyleNodeID node, bool value);
void style_engine_set_node_dom_paint_facts(DocumentHost const*, StyleNodeID node, u8 facts);
void style_engine_set_element_unique_node_id(DocumentHost const*, StyleNodeID node, u64 unique_node_id);
void style_engine_set_element_table_spans(DocumentHost const*, StyleNodeID node, u32 column_span, u32 row_span);
void style_engine_set_element_frameset_layout(DocumentHost const*, StyleNodeID node, ReadonlySpan<FfiFrameSetDimension> columns, ReadonlySpan<FfiFrameSetDimension> rows, ReadonlySpan<FfiFrameSetBorder> borders, u32 border_thickness);
void style_engine_set_element_id_name(DocumentHost const*, StyleNodeID node, StyleAtomID name);
void style_engine_set_shadow_root(DocumentHost const*, StyleNodeID shadow_host, StyleNodeID shadow_root);
void style_engine_record_environment_change(DocumentHost const*, u8 changes);
void style_engine_record_custom_property_registration_change(DocumentHost const*);
void style_engine_flush(DocumentHost const*);
void style_engine_finish_sheet_rules_replacement(DocumentHost const*, SheetID sheet, u32 declaration_block);
void style_engine_attach_sheet(DocumentHost const*, SheetID sheet, TreeScopeID tree_scope, SheetID before_sheet);
void style_engine_detach_sheet(DocumentHost const*, SheetID sheet, TreeScopeID tree_scope);
void style_engine_attach_sheet_occurrence(DocumentHost const*, SheetID sheet, TreeScopeID tree_scope, u64 identity, u64 before, bool conditions_hold);
void style_engine_detach_sheet_occurrence(DocumentHost const*, TreeScopeID tree_scope, u64 identity);
void style_engine_set_sheet_occurrence_conditions(DocumentHost const*, TreeScopeID tree_scope, u64 identity, bool conditions_hold);
void style_engine_set_element_part_exposure(DocumentHost const*, StyleNodeID node, StyleAtomID exposure);
void style_engine_set_element_directionality(DocumentHost const*, StyleNodeID node, StyleAtomID directionality);
void style_engine_set_element_custom_states(DocumentHost const*, StyleNodeID node, ReadonlySpan<StyleAtomID> states);
void style_engine_end_deferred_geometry_transaction_flush(DocumentHost const*);
void style_engine_record_container_query_input(DocumentHost const*, StyleNodeID node);
void style_engine_record_derived_element_style_input(DocumentHost const*, StyleNodeID node, u8 reaction, u8 inherited_style_groups);
void style_engine_record_font_input_changes(DocumentHost const*, ReadonlySpan<u32> family_name_lengths, ReadonlySpan<u16> family_name_units, ReadonlySpan<u64> font_lists);
void style_engine_fill_loaded_font_faces(DocumentHost const*, ReadonlySpan<u64> fills);
void style_engine_consume_element_style_input(DocumentHost const*, StyleNodeID node);
void style_engine_set_element_adjustment_facts(DocumentHost const*, StyleNodeID node, u32 facts);
void style_engine_set_element_construction_facts(DocumentHost const*, StyleNodeID node, u32 facts);
void style_engine_set_element_box_kind(DocumentHost const*, StyleNodeID node, u8 box_kind);
void style_engine_set_element_replaced_content_input(DocumentHost const*, StyleNodeID node, u8 kind, u8 present, u32 const* values);
void style_engine_set_element_heading_level(DocumentHost const*, StyleNodeID node, u8 level);
void style_engine_record_transition_baseline(DocumentHost const*, StyleNodeID node, u8 pseudo_kind, void const* style_record);
void style_engine_release_transition_baselines(DocumentHost const*);
void style_engine_begin_style_record_view_epoch(DocumentHost const*);
void style_engine_end_style_record_view_epoch(DocumentHost const*);
void style_engine_set_tree_scope_uses_document_sheets(DocumentHost const*, TreeScopeID tree_scope);
void style_engine_set_element_scroll_state(DocumentHost const*, StyleNodeID node, u8 stuck, u8 snapped, u8 scrollable, u8 scrolled);
void style_engine_set_element_size_container_query_facts(DocumentHost const*, StyleNodeID node, bool is_queried_container, bool depends_on_size_container_query);
void style_engine_note_size_container_needs_evaluation_after_layout(DocumentHost const*, StyleNodeID node);
void style_engine_record_size_container_query_dependents(DocumentHost const*, StyleNodeID container);
void style_engine_evaluate_size_containers_needing_evaluation_after_layout(DocumentHost const*);
void style_engine_set_element_backing_pseudo_kind(DocumentHost const*, StyleNodeID node, u8 pseudo_kind_plus_one);
void style_engine_set_counter_style_environment_identity(DocumentHost const*, TreeScopeID tree_scope, u64 identity);
void style_engine_set_held_style_record(DocumentHost const*, StyleNodeID node, void const* record);
void style_engine_set_held_pseudo_style_record(DocumentHost const*, StyleNodeID node, u8 pseudo_kind, StyleAtomID highlight_name, u64 style_record, void const* released);
void style_engine_set_element_css_defined_animations(DocumentHost const*, StyleNodeID node, u8 slot, ReadonlySpan<FlatPtr> names, ReadonlySpan<FfiAppliedAnimationDefinition> definitions);
void style_engine_set_tree_scope_root(DocumentHost const*, TreeScopeID tree_scope, StyleNodeID root);
void style_engine_set_sheet_conditions_hold(DocumentHost const*, SheetID sheet, bool conditions_hold);
u32 style_engine_connected_element_count(DocumentHost const*, BegunRead const*);
bool style_engine_self_check_is_enabled(DocumentHost const*);
bool style_engine_flown_update_awaits_self_check(DocumentHost const*);
bool style_engine_custom_property_environment_is_engine_made(DocumentHost const*, BegunRead const*, u64 identity);
bool style_engine_pending_transaction_may_affect_layout_geometry(DocumentHost const*, BegunRead const*);
void style_engine_defer_pending_transaction_for_geometry_read(DocumentHost const*, BegunRead const*);
bool style_engine_begin_deferred_geometry_transaction_flush(DocumentHost const*, BegunRead const*);
bool style_engine_has_deferred_geometry_transaction(DocumentHost const*, BegunRead const*);
u64 style_engine_cold_pass(DocumentHost const*, BegunRead const*, StyleNodeID root, bool verify, u32 threads);
u64 style_engine_cold_pass_report(DocumentHost const*, BegunRead const*, u32 index);
u64 style_engine_live_report(DocumentHost const*, BegunRead const*, StyleNodeID root, bool verify, u32 index);
u64 style_engine_style_counter(DocumentHost const*, BegunRead const*, u32 index);
bool style_engine_reset_style_counters(DocumentHost const*, BegunRead const*);
bool style_engine_last_pass_is_full(DocumentHost const*, BegunRead const*);
u32 style_engine_last_pass_size(DocumentHost const*, BegunRead const*, StyleNodeID root, bool changed);
u32 style_engine_last_pass_element(DocumentHost const*, BegunRead const*, StyleNodeID root, bool changed, u32 index);
u64 style_engine_animated_style_samples(DocumentHost const*, BegunRead const*, bool rebuilt_every_group, bool reset);
u32 style_engine_layer_index(DocumentHost const*, BegunRead const*, TreeScopeID tree_scope, u32 layer);
u64 style_engine_size_query_container_scan_visits(DocumentHost const*, BegunRead const*, bool reset);
u64 style_engine_ensure_random_base_value(DocumentHost const*, BegunRead const*, StyleNodeID node, ReadonlySpan<u16> name, bool element_shared);
}

// A view of the code units of a string, Latin-1 or UTF-16 as it stores them, which the engine copies if it keeps them.
inline FfiUtf16View ffi_utf16_view(Utf16View view)
{
    if (view.has_latin1_storage())
        return { .latin1 = view.latin1_span().data(), .utf16 = nullptr, .length = view.length_in_code_units() };
    return { .latin1 = nullptr, .utf16 = reinterpret_cast<u16 const*>(view.utf16_span().data()), .length = view.length_in_code_units() };
}

}

namespace Web::CSS {

enum class PseudoElement : u8;
enum class StyleRecordDependencyFlag : u8;

class CustomPropertyData;
class FontComputer;
class StyleComputer;
class StyleUpdateInputs;
class RustDeclarationBlock;
struct StyleProperty;

// A style sheet's resource context as a rule's cascaded values read it, keyed by its native
// sheet: an imported sheet has its own.
struct CollectedStyleSheetResourceContext {
    u64 source_identity { 0 };
    String base_url;
    bool has_base_url { false };
    bool origin_clean { false };
};

// The resource contexts of the document's style sheets as the last transaction was lent them,
// and what they were collected against. A document can have a shadow root, and a sheet, per
// element, so they are collected again only once either has moved on.
struct StyleSheetResourceContexts {
    Vector<CollectedStyleSheetResourceContext> contexts;
    // What the engine is lent of the contexts, which borrows from them.
    Vector<StyleEngineFFI::FfiStyleSheetResourceContextEntry> entries;
    u64 style_sheet_set_generation { 0 };
    ::URL::URL document_api_base_url;
    u64 version { 0 };
};

// Owns one document's StyleEngine. The engine itself lives entirely on the Rust side: selector
// evaluation, cascade, computed values, and every index and identity they are keyed by. C++ keeps
// what only C++ can own -- DOM and CSSOM object identity, mutation semantics, document lifecycle,
// and the observation barriers -- and holds no second copy of the style state.
//
// Most input crosses in one flat batch per style flush. Neighbour-relation changes are published
// immediately because the DOM mutation path already has the old relations in hand.
class WEB_API StyleEngine {
    AK_MAKE_NONCOPYABLE(StyleEngine);
    AK_MAKE_NONMOVABLE(StyleEngine);

public:
    explicit StyleEngine(StyleComputer* = nullptr);
    ~StyleEngine();

    void visit_edges(GC::Cell::Visitor&);

    // The random base values of the keys that name an element whose style node this was, which the element keeps while
    // it has none, and gives to the style node it gets next.
    [[nodiscard]] ParkedRandomBaseValues park_element_random_base_values(StyleNodeID);
    void unpark_element_random_base_values(StyleNodeID, ParkedRandomBaseValues);

    // The document's style node identities are minted here, without asking the engine, and the engine is told of each
    // mint ahead of anything recorded about its node. Identity 0 is never minted; it means "no node".
    StyleNodeID mint_style_node();
    void mint_style_nodes(Span<StyleNodeID> nodes);
    void mint_text_style_nodes(Span<StyleNodeID> nodes);
    bool is_taking_in_pending_arrivals() const { return m_taking_in_pending_arrivals; }
    [[nodiscard]] TemporaryChange<bool> begin_taking_in_pending_arrivals() { return TemporaryChange<bool>(m_taking_in_pending_arrivals, true); }
    void defer_element_initial_features(DOM::Element&);
    void note_element_awaiting_preparation_for_style_computation(DOM::Element&);
    void cancel_deferred_element_initial_features(DOM::Element&);
    // Whether any element waits to have its initial features published, or to be prepared for style computation.
    [[nodiscard]] bool has_elements_awaiting_initial_style_work() const { return !m_nodes_with_pending_initial_features.is_empty() || !m_nodes_awaiting_preparation_for_style_computation.is_empty(); }
    Vector<StyleNodeID> take_deferred_element_initial_features();
    HashTable<StyleNodeID> take_elements_awaiting_preparation_for_style_computation();

    void set_element_parts(StyleNodeID node, ReadonlySpan<StyleAtomID> names, ReadonlySpan<StyleNodeID> hosts);
    // Which longhand properties one of an element's own declarations covers, their canonical
    // specified values and their authored aliases, and whether the inventory has complete
    // continuation semantics.
    void set_element_inline_style_properties(StyleNodeID node, Parser::ValueParserFFI::DeclarationBlock const*);
    void set_element_presentational_hint_properties(StyleNodeID node, StyleEngineFFI::FfiElementDeclarationKind, ReadonlySpan<StyleProperty>);
    // Install the record of a style the host made outside the engine, such as a layout node's, which holds every
    // computed group. Nothing holds the record until the caller pins it.
    [[nodiscard]] StyleRecordID install_host_style_record(Layout::BegunRead const& read, ReadonlySpan<void const*> payloads, ComputedValuesFFI::InheritedSetValues inherited, ComputedValuesFFI::ResetBits reset, u64 custom_property_environment, u64 counter_style_environment_identity, void const* source_record, void const* custom_property_store);
    // The record the engine holds for the host for a record identity, or null for one it does not hold. The engine
    // holds it while it holds the identity, which a base record or an assigned or pinned animation-overlay generation
    // keeps; an InstalledStyle holds it longer.
    [[nodiscard]] void const* host_record(Layout::BegunRead const& read, StyleRecordID style_record) const;
    // A sheet the host numbers itself, which the engine adds as it applies the host's writes.
    [[nodiscard]] SheetID add_sheet(StyleEngineFFI::FfiCascadeOrigin origin);
    // Ends a replacement of the sheet's rules that style_engine_begin_sheet_rules_replacement() began.
    void finish_sheet_rules_replacement(SheetID sheet);

    // Interns one selector-mentioned name and returns its process-global atom, retained by this
    // document.
    //
    // Utf16FlyString is already interned, so its one-word raw form is the identity: the document host answers a name
    // it interned before with one hash lookup on that word. No string is copied, and neither side pays an ASCII or
    // UTF-16 conversion for a fact a u32 comparison answers.
    StyleAtomID intern_atom(Utf16FlyString const&);
    // Counts the reclaims of this document's atoms, so a cache keyed by atoms can tell when one may have been reused.
    [[nodiscard]] u64 atom_generation() const;
    // What a read of an element's style, or one of its pseudo-elements', made before the next style update asks of
    // the style engine, and the pseudo-element a demand may read.
    using RecordDemand = StyleEngineFFI::FfiRecordDemand;
    using PseudoElementRecordDemand = StyleEngineFFI::FfiPseudoElementRecordDemand;
    using DemandedPseudoElement = StyleEngineFFI::FfiDemandedPseudoElement;
    // The pseudo-element a record demand reads for a pseudo-element, if it is a synthetic one.
    [[nodiscard]] static Optional<DemandedPseudoElement> demanded_pseudo_element(PseudoElement);
    // Answers a record demand of an element: the record the engine derived from the document as it is now, or zero
    // where the read is C++'s.
    [[nodiscard]] StyleEngineFFI::FfiRecordDemandAnswer answer_record_demand(Layout::BegunRead const& read, StyleNodeID, RecordDemand);
    // Whether the environment `identity` names is one the engine resolved.
    [[nodiscard]] bool is_engine_custom_property_environment(Layout::BegunRead const& read, u64 identity) const { return identity && StyleEngineFFI::style_engine_custom_property_environment_is_engine_made(host(), &read, identity); }

    // A name both a selector and the DOM produce as text, with no interned identity on either side:
    // a language subtag and a `:dir()` keyword. Matched ASCII case-insensitively.
    StyleAtomID intern_text_atom(Utf16View);
    StyleAtomID intern_language_atom(Utf16View);

    // The namespace an element of an HTML document is an HTML element in, or none in any other document. It changes
    // only with the document's kind, so only a change goes to the engine.
    void publish_html_element_namespace(StyleAtomID);

    // Interns the name an attribute fact is published under. The document host works out the other names it answers to
    // once per name, as demand expansion revisits every live attribute.
    StyleAtomID intern_attribute_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_uri)
    {
        auto namespace_identity = namespace_uri.has_value() && !namespace_uri->is_empty() ? namespace_uri->raw_identity() : 0;
        return StyleAtomID { StyleEngineFFI::document_host_intern_attribute_name(host(), local_name.raw_identity(), namespace_identity) };
    }

    // Interns an attribute value and hands the engine what it spells unless the host knows that no selector and no
    // attr() reads this name. The engine keeps the text only where something reads it.
    StyleAtomID intern_attribute_value(StyleAtomID name, Utf16String const& value)
    {
        return StyleAtomID { StyleEngineFFI::document_host_intern_attribute_value(host(), name.value(), Utf16FlyString { value }.raw_identity()) };
    }
    // Whether the document host knows that no selector and no attr() reads the attribute name `name`.
    bool attribute_is_known_unread(StyleAtomID name) const
    {
        return StyleEngineFFI::document_host_attribute_is_known_unread(host(), name.value());
    }

    // Whether the document host knows that no selector tests `:local-link`.
    bool local_link_is_known_untested() const
    {
        return StyleEngineFFI::document_host_local_link_is_known_untested(host());
    }
    bool validity_is_known_untested() const
    {
        return StyleEngineFFI::document_host_validity_is_known_untested(host());
    }
    // What an element arrives with that the host knows nothing reads is withheld from the engine: whether a link is a
    // local link where no selector tests `:local-link`, a control's validity where none tests that, and each attribute
    // no selector and no attr() reads. Each
    // withheld fact notes the element it was withheld from, so that once something may read it, it is published for
    // those elements alone.
    struct WithheldAttributeName {
        Utf16FlyString local_name;
        Optional<Utf16FlyString> namespace_uri;
        bool operator==(WithheldAttributeName const&) const = default;
    };
    struct WithheldAttributeNameTraits : public DefaultTraits<WithheldAttributeName> {
        static unsigned hash(WithheldAttributeName const& name) { return pair_int_hash(name.local_name.hash(), name.namespace_uri.has_value() ? name.namespace_uri->hash() : 0); }
    };
    // NB: An element's identity may have been retired, or taken by another element, by the time a fact withheld from
    //     it is published: what is published is read from whatever element holds the identity then.
    void note_local_link_state_withheld() { m_local_link_state_is_withheld = true; }
    void note_validity_state_withheld() { m_validity_state_is_withheld = true; }
    void note_attribute_withheld(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_uri)
    {
        m_withheld_attribute_names.set({ local_name, namespace_uri });
    }
    // What was withheld that something may read now, which is withheld no longer, with the elements it was withheld
    // from.
    struct WithheldFeaturesNowRead {
        bool local_link_state { false };
        bool validity_state { false };
        Vector<WithheldAttributeName> attributes;
    };
    WithheldFeaturesNowRead take_withheld_features_now_read(Layout::BegunRead const&);

    // The document host stages deltas, which cross to the engine in one batch per style flush, never one write per
    // element.
    void record_tree_delta(StyleEngineFFI::FfiTreeDelta const&);
    // Everything an element's selectors test arrives with it, as one row, and the rows of many elements in one write.
    // The atoms hold each row's custom states, classes, and attributes as name and value atom pairs in turn.
    void record_element_arrivals(ReadonlySpan<StyleEngineFFI::FfiArrivingElement>);
    void record_element_arrival_rows(ReadonlySpan<StyleEngineFFI::FfiElementArrivalRow>, ReadonlySpan<u32> atoms);
    void record_local_feature_delta(StyleEngineFFI::FfiLocalFeatureDelta const&);
    void record_state_delta(StyleEngineFFI::FfiStateDelta const&);
    // The hover moves to `target`, or off the document for none, as the events of a mouse move hover it, once the rest
    // of the recorded input is in place.
    void record_hover(Optional<StyleNodeID> target);
    void record_element_declaration_delta(StyleEngineFFI::FfiElementDeclarationDelta const&);
    enum StyleReaction : u8 {
        PublishedStyle = 1 << 0,
        RecomputeStyle = 1 << 1,
    };
    // A style reaction of one element, for the engine to settle where it can. It names what moved,
    // not who computes the element's style again.
    void record_derived_element_style_input_change(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups = 0);
    // The elements the host shows the highlight pseudo-element of the kind in, which style updates
    // style that pseudo-element of. Those that start or stop showing it are restyled.
    void set_highlight_observing_elements(PseudoElement, ReadonlySpan<StyleNodeID>);
    // The elements the host shows custom highlights in, with the name of each at the same index in `names`.
    void set_custom_highlight_observing_elements(ReadonlySpan<StyleNodeID> style_nodes, ReadonlySpan<StyleAtomID> names);
    // What a container query or container-relative length read of the element's containers moved.
    void record_container_query_input_change(StyleNodeID style_node);
    // Records every element whose style a size query or container-relative unit decided against the container.
    void record_size_container_query_dependents(StyleNodeID container);
    // Records the dependents of every container a style computation asked about before it had a box.
    void evaluate_size_containers_needing_evaluation_after_layout(Layout::BegunRead const& read);
    // The style nodes whose style depends on the axes of the viewport moved_axes names, as DOM::StyleEnvironmentChange bits.
    [[nodiscard]] Vector<StyleNodeID> viewport_dependent_style_nodes(Layout::BegunRead const& read, u8 moved_axes);
    [[nodiscard]] bool has_recorded_input() const;
    // Nodes that connected without taking an identity yet count as recorded input: they arrive when the input is
    // next submitted.
    void note_pending_arrivals(size_t count);
    void forget_pending_arrivals() { m_pending_arrival_count = 0; }
    // The nodes marked as waiting to arrive since the last take-in, which it takes in without walking the marks down
    // from the document where the list holds every node marked since.
    void list_arrival(DOM::Node&);
    void note_unlisted_arrivals() { m_arrivals_unlisted = true; }
    // The nodes listed, or none where a node was marked that the list does not hold.
    Optional<GC::RootVector<GC::Ref<DOM::Node>>> take_listed_arrivals();
    [[nodiscard]] bool has_pending_arrivals() const { return m_pending_arrival_count > 0; }
    // Children that wait to arrive in a display:none subtree may have to arrive now. Nothing is recorded until the
    // input is next submitted, so this can be noted while a style update applies its reactions.
    void note_waiting_arrivals_may_arrive();

    // What an element's arrival has to carry to reach the anchor of a `:has()` argument: one of `names` as the raw
    // identity of its tag, ID, class or attribute name, or the state of one of `state_pseudo_classes`.
    struct RelationalArrivalKeys {
        u64 version { 0 };
        bool reached_by_any_arrival { false };
        HashTable<FlatPtr> names;
        Vector<PseudoClass> state_pseudo_classes;
    };
    RelationalArrivalKeys const& relational_arrival_keys(Layout::BegunRead const&);
    // The keys as the engine last said, which may have moved since.
    RelationalArrivalKeys const& last_relational_arrival_keys() const { return m_relational_arrival_keys; }
    // The keys the children waiting in display:none subtrees were last found to carry none of.
    Optional<u64> waiting_children_checked_against_keys_version() const { return m_waiting_children_checked_against_keys_version; }
    void set_waiting_children_checked_against_keys_version(u64 version) { m_waiting_children_checked_against_keys_version = version; }
    // Whether children waited in a display:none subtree as of the last take-in that looked at them, or were released
    // since.
    bool may_have_waiting_children() const { return m_may_have_waiting_children; }
    void set_may_have_waiting_children(bool value) { m_may_have_waiting_children = value; }
    // A rule a style update compiled can move the keys the waiting children were looked at against, and the
    // children have to be looked at again before the update ends.
    void note_waiting_arrivals_if_keys_moved(Layout::BegunRead const&);
    [[nodiscard]] bool has_pending_transaction(Layout::BegunRead const& read) const;
    [[nodiscard]] bool has_deferred_geometry_transaction(Layout::BegunRead const& read) const;
    // Whether a geometry read deferred a transaction that has_deferred_geometry_transaction() may still find, which the
    // host knows without reading the render state.
    [[nodiscard]] bool may_have_deferred_geometry_transaction() const { return m_geometry_read_deferred_transaction; }
    [[nodiscard]] bool pending_transaction_may_affect_layout_geometry(Layout::BegunRead const& read);
    void defer_pending_transaction_for_geometry_read(Layout::BegunRead const& read);
    [[nodiscard]] bool begin_deferred_geometry_transaction_flush(Layout::BegunRead const& read);
    // Geometry reads establish the before-change style used by CSS transitions. Keep this
    // monotonic because an inactive rule or a later inline edit can expose the transition only
    // after that boundary.
    void note_css_transitions_may_observe_style_changes() { m_css_transitions_may_observe_style_changes = true; }
    [[nodiscard]] bool css_transitions_may_observe_style_changes() const { return m_css_transitions_may_observe_style_changes; }

    // Submits everything recorded since the last flush as one transaction and normalizes it.
    void flush();

    struct StyleUpdateTimings {
        u64 submission_microseconds { 0 };
        u64 bridge_microseconds { 0 };
    };

    // Takes pending inputs, and answers how long that took.
    StyleUpdateTimings run_style_update(Layout::BegunRead const& read, StyleNodeID root);
    // Lets the pending style update under root, taken against `inputs`, fly beside the event loop where `blocker` is
    // none, and answers whether it flies. The next style update drains it first, with drain_flown_style_update().
    [[nodiscard]] bool let_style_update_fly(StyleUpdateInputs const& inputs, StyleNodeID root, Layout::RustFFI::FfiFlightBlocker blocker);
    // Whether a style update flew whose reactions have not been drained yet, which its document knows.
    [[nodiscard]] bool has_flown_style_update() const;
    // Takes the style update that flew in, waiting for it to land, to drain its reactions against the inputs it
    // was sealed with. What was written beside it waits for end_flown_style_drain(): it is the next transaction's.
    StyleUpdateTimings drain_flown_style_update(Layout::BegunRead const& read);
    void end_flown_style_drain();
    // Takes in the fonts passes resolved against a table older than the document's. Those of a transaction that flew wait
    // for the end of its drain, as what was written beside it does; a drain's own later passes publish the newer table
    // ahead of that.
    void take_in_fonts_resolved_against_older_tables();
    // Has the engine recompute the children of `parent` whose style reads their place among their siblings, where some
    // child's does.
    void restyle_children_reading_sibling_position(DOM::Element& parent);
    // The transaction that flew counted the children of `parent` as they were when it was sealed. A child whose style
    // it found to read their count is known to only once the drain installs that style, so the drain's end recounts
    // them.
    void note_children_changed_beside_flown_transaction(StyleNodeID parent)
    {
        if (parent != 0)
            m_parents_whose_children_changed_beside_flown_transaction.set(parent);
    }
    void discard_style_update_outputs();

    using MatchedRule = StyleEngineFFI::FfiMatchedRule;

    // Every rule that matches one element, user agent rules included, in the order the cascade
    // applies them. Returns false when matching could not complete.
    bool matched_rules(Layout::BegunRead const& read, StyleNodeID node, Vector<MatchedRule>&);

    // Give the engine the document's @font-face table and cascade memo, when they moved since it was last given them.
    void publish_font_faces(FontComputer const&);

    // The custom-property environment each element holds is kept here; the element keeps none of its own.
    void set_element_custom_property_data(Layout::BegunRead const& read, DOM::Element const&, CustomPropertyData const*, IsAnimationOverlay);
    void note_element_custom_property_environments(Layout::BegunRead const& read, DOM::Element const&, CustomPropertyData const&, IsAnimationOverlay);
    void republish_element_custom_property_environments(Layout::BegunRead const& read, DOM::Document&);

    // Enumerates the engine's counters. Returns false once index is past the last counter.
    bool counter(Layout::BegunRead const& read, size_t index, StringView& out_name, u64& out_value) const;

    // The render state that owns the engine, which the document's layout node arena shares.
    [[nodiscard]] Layout::RenderDocument& render_document() { return *m_render_document; }
    [[nodiscard]] Layout::RenderDocument const& render_document() const { return *m_render_document; }

    // The host of the document's render state, which every entry into the document's style engine goes through.
    [[nodiscard]] Layout::RustFFI::DocumentHost* host() const { return m_render_document->host(); }

private:
    struct LentComputationInputs;
    void gather_computation_inputs(LentComputationInputs&);

    void submit_recorded_input();
    void publish_attribute_value_texts_if_requirements_moved(Layout::BegunRead const& read);

    Optional<StyleSheetResourceContexts> m_style_sheet_resource_contexts;
    u64 m_style_sheet_resource_contexts_version { 0 };
    Optional<u64> m_function_rules_checked_generation;
    bool m_document_has_function_rules { false };

    NonnullRefPtr<Layout::RenderDocument> m_render_document;
    u64 m_published_font_environment_generation { 0 };
    GC::Ptr<StyleComputer> m_style_computer;
    // The language the host interned last, which most elements arriving after one share: its tag, its atom, and the
    // atom generation the atom was interned in.
    Utf16String m_last_language_text;
    StyleAtomID m_last_language_atom;
    u64 m_last_language_atom_generation { 0 };

    // NB: In the order the elements arrived, which is tree order. An element that left since is still listed, and no
    //     longer holds its mark, DOM::Element::initial_style_features_pending().
    Vector<StyleNodeID> m_nodes_with_pending_initial_features;
    bool m_taking_in_pending_arrivals { false };
    HashTable<StyleNodeID> m_nodes_awaiting_preparation_for_style_computation;

    HashTable<StyleNodeID> m_parents_whose_children_changed_beside_flown_transaction;
    size_t m_pending_arrival_count { 0 };
    // NB: The list holds the nodes weakly: a node that left the tree and was collected arrives nowhere.
    Vector<GC::Weak<DOM::Node>> m_listed_arrivals;
    bool m_arrivals_unlisted { false };
    RelationalArrivalKeys m_relational_arrival_keys;
    Optional<u64> m_waiting_children_checked_against_keys_version;
    bool m_may_have_waiting_children { false };
    bool m_local_link_state_is_withheld { false };
    bool m_validity_state_is_withheld { false };
    HashTable<WithheldAttributeName, WithheldAttributeNameTraits> m_withheld_attribute_names;
    bool m_css_transitions_may_observe_style_changes { false };
    mutable bool m_geometry_read_deferred_transaction { false };
    StyleAtomID m_html_element_namespace;
};

}
