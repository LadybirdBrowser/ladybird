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
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/ParkedRandomBaseValues.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::CSS::StyleValueFFI {

struct FfiTransitionAction;
struct FfiTransitionInput;

}

namespace Web::CSS {

enum class PseudoElement : u8;
enum class StyleRecordDependencyFlag : u8;

class CustomPropertyData;
class FontComputer;
class StyleComputer;
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
    u64 style_sheet_set_generation { 0 };
    String document_api_base_url;
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
    using DeviceClass = StyleEngineFFI::FfiDeviceClass;
    explicit StyleEngine(DeviceClass, StyleComputer* = nullptr);
    ~StyleEngine();

    void visit_edges(GC::Cell::Visitor&);

#include <LibWeb/StyleEngineBridgeGenerated.h>

    // https://drafts.csswg.org/css-values-5/#random-caching
    // The random base value of a random caching key: the name, and the element unless the sharing is element-shared.
    [[nodiscard]] double ensure_random_base_value(Layout::BegunRead const& read, StyleNodeID, Utf16View name, bool element_shared);
    // The random base values of the keys that name an element whose style node this was, which the element keeps while
    // it has none, and gives to the style node it gets next.
    [[nodiscard]] ParkedRandomBaseValues park_element_random_base_values(StyleNodeID);
    void unpark_element_random_base_values(StyleNodeID, ParkedRandomBaseValues);

    // The document's style node identities are minted here, without asking the engine, and the engine is told of each
    // mint ahead of anything recorded about its node. Identity 0 is never minted; it means "no node".
    StyleNodeID mint_style_node();
    void mint_style_nodes(Span<StyleNodeID> nodes);
    void mint_text_style_nodes(Span<StyleNodeID> nodes);
    void defer_element_initial_features(StyleNodeID style_node)
    {
        m_nodes_with_pending_initial_features.set(style_node);
        m_nodes_awaiting_first_style_computation.set(style_node);
    }
    void cancel_deferred_element_initial_features(StyleNodeID style_node)
    {
        m_nodes_with_pending_initial_features.remove(style_node);
        m_nodes_awaiting_first_style_computation.remove(style_node);
    }
    [[nodiscard]] bool has_deferred_element_initial_features(StyleNodeID style_node) const { return m_nodes_with_pending_initial_features.contains(style_node); }
    HashTable<StyleNodeID> take_deferred_element_initial_features();
    HashTable<StyleNodeID> take_elements_awaiting_first_style_computation();

    void set_element_parts(StyleNodeID node, ReadonlySpan<StyleAtomID> names, ReadonlySpan<StyleNodeID> hosts);
    void set_element_language(StyleNodeID node, StyleAtomID language, Utf16View tag);
    // The characters a text node holds. The engine shares the document's string rather than copying it, so this
    // costs one reference.
    void set_text_data(StyleNodeID node, Utf16String const& data);
    // Which longhand properties one of an element's own declarations covers, their canonical
    // specified values and their authored aliases, and whether the inventory has complete
    // continuation semantics.
    void set_element_inline_style_properties(StyleNodeID node, RustDeclarationBlock const*);
    void set_element_presentational_hint_properties(StyleNodeID node, StyleEngineFFI::FfiElementDeclarationKind, ReadonlySpan<StyleProperty>);
    struct StyleRecordDelta {
        StyleRecordID old_style_record;
        StyleRecordID new_style_record;
    };
    using StyleRecordView = StyleEngineFFI::FfiStyleRecordView;
    // Publish the immutable input identities of an element or pseudo-element's base style and
    // return its previous and current StyleRecordID assignments. A zero node interns an unassigned
    // record for a style target which is not registered in the engine.
    [[nodiscard]] StyleRecordDelta publish_computed_groups(Layout::BegunRead const& read, StyleNodeID node, u8 pseudo_kind, ReadonlySpan<void const*> payloads, size_t inherited_group_count, u64 custom_property_environment, bool inherited_group_swap_candidate, u64 counter_style_environment_identity, u64 animation_overlay_identity, void const* animated_overlay, ReadonlySpan<void const*> animation_overlay_payloads, void const* computed_longhand_table, void const* custom_property_store);
    [[nodiscard]] Optional<StyleRecordDelta> publish_animation_overlay(Layout::BegunRead const& read, StyleNodeID node, u8 pseudo_kind, u64 animation_overlay_identity, void const* animated_overlay, ReadonlySpan<void const*> payloads);
    [[nodiscard]] StyleRecordDependencyFlag style_record_dependency_flags(Layout::BegunRead const& read, StyleRecordID style_record) const;
    [[nodiscard]] u64 style_record_custom_property_environment(Layout::BegunRead const& read, StyleRecordID style_record) const;
    // What moving between two records changes, for no element in particular.
    [[nodiscard]] u32 compare_style_records(Layout::BegunRead const& read, StyleRecordID old_style_record, StyleRecordID new_style_record) const;
    // What moving the element from one record to another damages, which the engine reads from the
    // records and its own facts of the element.
    [[nodiscard]] u32 element_record_damage(Layout::BegunRead const& read, StyleNodeID, StyleRecordID old_style_record, StyleRecordID new_style_record) const;
    // The same for one of its pseudo-elements, whose box appears or goes away when either record is
    // none. The host compares the counter styles the box was built with.
    [[nodiscard]] u32 pseudo_element_record_damage(Layout::BegunRead const& read, StyleNodeID, PseudoElement, StyleRecordID old_style_record, StyleRecordID new_style_record, StyleRecordID originating_style_record, bool counter_styles_changed) const;
    [[nodiscard]] bool animation_overlay_changed(Layout::BegunRead const& read, StyleRecordID old_style_record, void const* animated_overlay) const;
    [[nodiscard]] StyleEngineFFI::FfiAnimationInvalidation compare_animation_overlay(Layout::BegunRead const& read, StyleRecordID old_style_record, void const* animated_overlay, ReadonlySpan<void const*> payloads, bool is_document_element) const;
    // The borrowed views are stable while a base record exists or an animation-overlay generation remains assigned or
    // pinned.
    [[nodiscard]] StyleRecordView style_record_view(Layout::BegunRead const& read, StyleRecordID style_record) const;
    void decide_transitions(Layout::BegunRead const& read, StyleRecordID before_style_record, StyleRecordID after_style_record, StyleValueFFI::FfiTransitionInput const&, StyleValueFFI::FfiTransitionAction*) const;
    // Remove the retained input identities for one pseudo-element kind and return its removal.
    [[nodiscard]] StyleRecordDelta remove_computed_pseudo(Layout::BegunRead const& read, StyleNodeID node, u8 pseudo_kind);
    void finish_sheet_rules_replacement(SheetID sheet);
    // A fresh identity for an element-sourced declaration block.
    //
    // A block's contents change while the CSSOM object stays the same, so its address is not what
    // makes one version of it different from the next. A version is: an edit that reported the same
    // identity on both sides would cancel in the journal and invalidate nothing.
    [[nodiscard]] u32 next_declaration_block_version() { return StyleEngineFFI::style_engine_next_declaration_block_version(host()); }

    // Interns one selector-mentioned name and returns its process-global atom, retained by this
    // document.
    //
    // Utf16FlyString is already interned, so its one-word raw form is the identity: this is a hash
    // lookup on that word plus one reference to keep the name alive. No string is copied, and
    // neither side pays an ASCII or UTF-16 conversion for a fact a u32 comparison answers.
    StyleAtomID intern_atom(Utf16FlyString const&);
    // The process-global atom of `name` qualified by `namespace_atom`, retained by this document.
    StyleAtomID intern_qualified_atom(StyleAtomID namespace_atom, StyleAtomID name);
    // The engine keeps what a custom property's name spells, once per name, for the environments
    // it computes.
    void note_custom_property_name(StyleAtomID, Utf16FlyString const&);
    // The store of an environment the engine resolved, with one strong reference transferred, and
    // the environment it was resolved over; null for one C++ published.
    [[nodiscard]] void const* borrow_engine_custom_property_environment(Layout::BegunRead const& read, u64 identity, u64& parent_identity) const;
    // The environment a child inherits from one the engine resolved.
    [[nodiscard]] u64 inheritable_custom_property_environment(Layout::BegunRead const& read, u64 identity) const;
    // Moves a node's record to the environment its inherited custom-property data was refreshed
    // to; the new record's identity, or zero when nothing moved.
    [[nodiscard]] StyleRecordID republish_record_environment(Layout::BegunRead const&, StyleNodeID, u64 environment, void const* store);
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
    // Answers a record demand of one of an element's pseudo-elements: its record, that it generates no box, or zero
    // where the read is C++'s.
    [[nodiscard]] StyleEngineFFI::FfiRecordDemandAnswer answer_pseudo_element_record_demand(Layout::BegunRead const& read, StyleNodeID, PseudoElementRecordDemand, DemandedPseudoElement);
    // Settles the synthetic pseudo-elements of an element whose record the host just installed, or refuses them all.
    [[nodiscard]] StyleEngineFFI::FfiSettledPseudoRecords settle_pseudo_records_after_host_record(Layout::BegunRead const& read, StyleNodeID, bool old_is_list_item);
    // Whether an environment identity is one the engine minted for an environment it resolved.
    [[nodiscard]] static bool is_engine_custom_property_environment(u64 identity) { return (identity & (1ull << 62)) != 0; }
    [[nodiscard]] u64 atom_generation() const { return m_atom_generation; }
    // The namespace `[*|x]` names, which is any of them. No interned namespace is zero, so this
    // keys a form of its own in the same table.
    static constexpr StyleAtomID any_namespace { 0 };

    // A name both a selector and the DOM produce as text, with no interned identity on either side:
    // a language subtag and a `:dir()` keyword. Matched ASCII case-insensitively.
    StyleAtomID intern_text_atom(Utf16View);
    StyleAtomID intern_language_atom(Utf16View);
    // The same, without the ASCII folding, for names compared literally such as namespace URIs.
    StyleAtomID intern_case_sensitive_text_atom(Utf16View);

    // The namespace an element of an HTML document is an HTML element in, or none in any other document. It changes
    // only with the document's kind, so only a change goes to the engine.
    void publish_html_element_namespace(StyleAtomID);

    // Interns the exact identity an attribute fact uses and memoizes its namespace and folded
    // forms. Demand expansion revisits every live attribute, so these forms must not cross the
    // boundary again merely to recover an already published name.
    StyleAtomID intern_attribute_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_uri);

    // Interns an attribute value and records what it spells when a selector or an attr() can read
    // this name. Values repeat heavily, so demanded text crosses once per distinct value.
    StyleAtomID intern_attribute_value(StyleAtomID name, Utf16String const& value);
    // Demand expansion already has every value identity. Check the name before interning the text
    // so attributes nothing reads as text do not pay another string hash.
    void backfill_attribute_value_text_if_required(StyleAtomID name, Utf16String const& value);

    // Deltas accumulate here and cross in one flat batch per style flush, never one call per
    // element.
    void record_tree_delta(StyleEngineFFI::FfiTreeDelta const&);
    void record_element_arrival(StyleEngineFFI::FfiElementArrival, ReadonlySpan<StyleAtomID> custom_states);
    void record_local_feature_delta(StyleEngineFFI::FfiLocalFeatureDelta const&);
    void record_state_delta(StyleEngineFFI::FfiStateDelta const&);
    void record_element_declaration_delta(StyleEngineFFI::FfiElementDeclarationDelta const&);
    enum StyleReaction : u8 {
        PublishedStyle = 1 << 0,
        RecomputeStyle = 1 << 1,
        InheritedStyle = 1 << 2,
        InheritedCustomProperties = 1 << 3,
        RecomputeDescendantStyles = 1 << 4,
        AncestorBecameVisible = 1 << 5,
        PseudoInputsMayHaveChanged = 1 << 6,
        FontInputsChanged = 1 << 7,
    };
    // What applying a style reaction found, reported so the engine derives the children's reactions.
    enum StyleReactionAppliedFact : u32 {
        DidChangeCustomProperties = 1 << 0,
        InvalidationIsNone = 1 << 1,
        NeedsLayoutTreeRebuild = 1 << 2,
        RecomputeDescendants = 1 << 3,
        ChildrenExplicitlyInherit = 1 << 4,
        ShadowChildrenExplicitlyInherit = 1 << 5,
        WasUnstyled = 1 << 6,
        WasDisplayNone = 1 << 7,
        DisplayChanged = 1 << 11,
    };
    // A style reaction of one element, for the engine to settle where it can. It names what moved,
    // not who computes the element's style again.
    void record_derived_element_style_input_change(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups = 0);
    void record_flat_tree_descendant_style_input_changes(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups = 0);
    // What a container query or container-relative length read of the element's containers moved.
    void record_container_query_input_change(StyleNodeID style_node);
    // Records every element whose style a size query or container-relative unit decided against the container.
    void record_size_container_query_dependents(StyleNodeID container);
    // Records the dependents of every container a style computation asked about before it had a box.
    void evaluate_size_containers_needing_evaluation_after_layout(Layout::BegunRead const& read);
    [[nodiscard]] Vector<StyleNodeID> viewport_dependent_style_nodes(Layout::BegunRead const& read);
    void record_benchmark_marker(Utf16View);
    [[nodiscard]] bool has_recorded_input() const;
    // Nodes that connected without taking an identity yet count as recorded input: they arrive when the input is
    // next submitted.
    void note_pending_arrivals(size_t count);
    void forget_pending_arrivals() { m_pending_arrival_count = 0; }
    [[nodiscard]] bool has_pending_transaction(Layout::BegunRead const& read) const;
    [[nodiscard]] bool has_deferred_geometry_transaction(Layout::BegunRead const& read) const;
    // Whether a geometry read deferred a transaction that has_deferred_geometry_transaction() may still find, which the
    // host knows without reading the render state.
    [[nodiscard]] bool may_have_deferred_geometry_transaction() const { return m_geometry_read_deferred_transaction; }
    [[nodiscard]] bool has_deferred_element_style_inputs(Layout::BegunRead const& read) const;
    [[nodiscard]] bool has_deferred_element_style_input(Layout::BegunRead const& read, StyleNodeID style_node) const;
    [[nodiscard]] bool pending_transaction_may_affect_layout_geometry(Layout::BegunRead const& read);
    [[nodiscard]] bool defer_pending_transaction_for_geometry_read(Layout::BegunRead const& read);
    [[nodiscard]] bool begin_deferred_geometry_transaction_flush(Layout::BegunRead const& read);
    void end_deferred_geometry_transaction_flush();
    // Geometry reads establish the before-change style used by CSS transitions. Keep this
    // monotonic because an inactive rule or a later inline edit can expose the transition only
    // after that boundary.
    void note_css_transitions_may_observe_style_changes() { m_css_transitions_may_observe_style_changes = true; }
    [[nodiscard]] bool css_transitions_may_observe_style_changes() const { return m_css_transitions_may_observe_style_changes; }

    // Submits everything recorded since the last flush as one transaction and normalizes it.
    void flush();

    using PublishedStyleDelta = StyleEngineFFI::FfiStyleDelta;
    struct PublishedTransactionVersion {
        u64 transaction;
        u64 program;
    };
    struct PublishedStyleTransaction {
        PublishedTransactionVersion version;
        ReadonlySpan<PublishedStyleDelta> reactions;
        bool is_scoped;
        bool only_derived_child_reactions;
        u32 connected_element_count;
        // Returned to the caller so diagnostic transactions do not charge style-update clocks.
        u64 submission_microseconds;
        u64 bridge_microseconds;
    };

    // Takes pending inputs. The diagnostic transaction reports reaction nodes and then discards
    // its matching outputs. The style transaction publishes versioned match-answer records. False
    // means the result is broad enough to prefer complete matching scratch.
    // NB: The returned reactions borrow Rust storage until the next mutable engine call or an
    //     explicit discard. Consume them synchronously before asking the engine anything else.
    bool take_diagnostic_style_transaction(Layout::BegunRead const& read, StyleNodeID root, Function<void(ReadonlySpan<StyleNodeID>)>&&);
    PublishedStyleTransaction take_style_transaction(Layout::BegunRead const& read, StyleNodeID root);
    // Lets the pending style transaction under root fly beside the event loop, where `blocker` is none, and answers
    // whether it flies. The next style update drains it first, with take_flown_style_transaction().
    [[nodiscard]] bool let_style_transaction_fly(Layout::BegunRead const& read, StyleNodeID root, Layout::RustFFI::FfiFlightBlocker blocker);
    // Whether the style transaction that flew still flies. One that has landed is taken in, which only the event loop
    // does, between two tasks.
    [[nodiscard]] bool style_transaction_flies();
    // Whether a style transaction flew whose reactions no style update has drained yet, which its document knows.
    [[nodiscard]] bool has_flown_style_transaction() const;
    // Takes the style transaction that flew in, waiting for it to land, to drain its reactions against the inputs it
    // was sealed with. What was written beside it waits for end_flown_style_drain(): it is the next transaction's.
    PublishedStyleTransaction take_flown_style_transaction(Layout::BegunRead const& read);
    void end_flown_style_drain();
    // Whether the frame whose style transaction is drained applied the element `style_node` names the record
    // `style_record` ahead of the host, and marked the relayout the move asks for.
    [[nodiscard]] bool frame_marked_relayout(StyleNodeID style_node, StyleRecordID style_record) const;
    // The transaction that flew knows an element that arrived or was removed beside it as it was sealed: the drain
    // leaves its change, and what inherits from it, to the next transaction.
    void note_style_node_arrived_or_retired(StyleNodeID);
    [[nodiscard]] bool style_node_arrived_or_retired_beside_flown_transaction(StyleNodeID style_node) const { return m_style_nodes_beside_flown_transaction.contains(style_node); }
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
    void sort_style_deltas_for_direct_application(Layout::BegunRead const& read, Span<PublishedStyleDelta>) const;
    void discard_style_transaction_outputs(Layout::BegunRead const& read);

    // While a batch's reactions are applied, a host's own style application may rewrite the declarations of an element
    // in its shadow tree, after the engine computed that element's record from the ones it had. These name the
    // elements whose declarations changed that way.
    void begin_noting_declaration_changes_during_apply() { ++m_declaration_change_noting_depth; }
    void end_noting_declaration_changes_during_apply()
    {
        VERIFY(m_declaration_change_noting_depth > 0);
        if (--m_declaration_change_noting_depth == 0)
            m_declaration_changes_during_apply.clear_with_capacity();
    }
    void note_element_declarations_changed(StyleNodeID node)
    {
        if (m_declaration_change_noting_depth > 0)
            m_declaration_changes_during_apply.set(node);
    }
    [[nodiscard]] bool declarations_changed_during_apply(StyleNodeID node) const { return m_declaration_changes_during_apply.contains(node); }

    using RuleMatch = StyleEngineFFI::FfiRuleMatch;

    enum class MatchPurpose {
        Exact,
        Cascade,
    };

    // Every rule that decides for one element, in the order the cascade applies them. Cascade
    // callers may omit rules whose declarations cannot win; exact callers receive the same answer
    // as the document pass. Returns false when matching could not complete.
    bool match_element(Layout::BegunRead const& read, StyleNodeID node, Vector<RuleMatch>&, MatchPurpose);

    // Give the engine the document's @font-face table and cascade memo, when they moved since it was last given them.
    void publish_font_faces(FontComputer const&);

    // The custom-property environment each element holds is kept here; the element keeps none of its own.
    void set_element_custom_property_data(Layout::BegunRead const& read, DOM::Element const&, CustomPropertyData const*);
    [[nodiscard]] CustomPropertyData const* element_custom_property_data(Layout::BegunRead const& read, StyleNodeID) const;
    void set_pseudo_element_custom_property_data(StyleNodeID, PseudoElement, CustomPropertyData const*);
    [[nodiscard]] CustomPropertyData const* pseudo_element_custom_property_data(Layout::BegunRead const& read, StyleNodeID, PseudoElement) const;
    // One bit per kind of the element's synthetic pseudo-elements that hold an environment.
    [[nodiscard]] u64 pseudo_elements_with_custom_property_data(Layout::BegunRead const& read, StyleNodeID) const;

    // Enumerates the engine's counters. Returns false once index is past the last counter.
    bool counter(Layout::BegunRead const& read, size_t index, StringView& out_name, u64& out_value) const;

    // The render state that owns the engine, which the document's layout node arena shares.
    [[nodiscard]] Layout::RenderDocument& render_document() { return *m_render_document; }
    [[nodiscard]] Layout::RenderDocument const& render_document() const { return *m_render_document; }

    // The host of the document's render state, which every entry into the document's style engine goes through.
    [[nodiscard]] Layout::RustFFI::DocumentHost* host() const { return m_render_document->host(); }

private:
    using InputTransaction = StyleEngineFFI::FfiStyleInputTransaction;

    struct LentComputationInputs;
    void gather_computation_inputs(Layout::BegunRead const& read, LentComputationInputs&);
    PublishedStyleTransaction publish_style_transaction_view(StyleEngineFFI::FfiStyleTransactionView const&, MonotonicTime submission_started_at, MonotonicTime bridge_started_at);

    void apply_transaction(InputTransaction const&);
    void submit_recorded_input();
    bool refresh_attribute_value_text_requirements(Layout::BegunRead const& read);
    [[nodiscard]] bool attribute_name_requires_value_text(StyleAtomID);
    void publish_attribute_value_text(StyleAtomID, Utf16View);

    Optional<StyleSheetResourceContexts> m_style_sheet_resource_contexts;

    NonnullRefPtr<Layout::RenderDocument> m_render_document;
    StyleEngineFFI::StyleNodeIdAllocator* m_style_node_ids { nullptr };
    u64 m_published_font_environment_generation { 0 };
    GC::Ptr<StyleComputer> m_style_computer;

    HashMap<FlatPtr, StyleAtomID> m_atoms;
    HashTable<StyleAtomID> m_published_language_atoms;
    HashTable<StyleAtomID> m_published_custom_property_names;
    HashMap<StyleAtomID, HashMap<StyleAtomID, StyleAtomID>> m_attribute_name_atoms;
    HashMap<StyleAtomID, bool> m_attribute_names_requiring_value_text;
    u64 m_atom_generation { 1 };
    u64 m_attribute_value_text_requirements_version { 0 };
    HashTable<StyleNodeID> m_nodes_with_pending_initial_features;
    HashTable<StyleNodeID> m_nodes_awaiting_first_style_computation;
    u32 m_declaration_change_noting_depth { 0 };
    HashTable<StyleNodeID> m_declaration_changes_during_apply;
    size_t m_element_match_capacity { 64 };

    HashTable<StyleNodeID> m_style_nodes_beside_flown_transaction;
    HashTable<StyleNodeID> m_parents_whose_children_changed_beside_flown_transaction;
    Vector<StyleEngineFFI::FfiTreeDelta> m_tree_deltas;
    Vector<StyleEngineFFI::FfiElementArrival> m_element_arrivals;
    Vector<u32> m_arrival_custom_state_atoms;
    Vector<StyleEngineFFI::FfiLocalFeatureDelta> m_local_feature_deltas;
    Vector<StyleEngineFFI::FfiStateDelta> m_state_deltas;
    Vector<StyleEngineFFI::FfiElementDeclarationDelta> m_element_declaration_deltas;
    size_t m_pending_arrival_count { 0 };
    bool m_css_transitions_may_observe_style_changes { false };
    mutable bool m_geometry_read_deferred_transaction { false };
    StyleAtomID m_html_element_namespace;
};

}
