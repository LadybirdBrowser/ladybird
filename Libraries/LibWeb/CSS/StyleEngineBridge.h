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
#include <AK/Types.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::CSS::StyleValueFFI {

struct FfiTransitionAction;
struct FfiTransitionInput;

}

namespace Web::CSS {

class StyleComputer;

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

    // Identity 0 is never returned; it means "no node".
    StyleNodeID allocate_style_node();
    void allocate_style_nodes(Span<StyleNodeID> nodes);
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
    void mark_style_node_preallocated(StyleNodeID style_node, TreeScopeID tree_scope) { m_preallocated_style_nodes.set(style_node, tree_scope); }
    Optional<TreeScopeID> consume_preallocated_style_node(StyleNodeID style_node) { return m_preallocated_style_nodes.take(style_node); }
    void cancel_preallocated_style_node(StyleNodeID style_node) { m_preallocated_style_nodes.remove(style_node); }
    [[nodiscard]] bool resize_parsed_substitution_cache(u64 bytes);

    void set_element_parts(StyleNodeID node, ReadonlySpan<StyleAtomID> names, ReadonlySpan<StyleNodeID> hosts);
    void set_element_language(StyleNodeID node, StyleAtomID language, Utf16View tag);
    // Which longhand properties a rule declares, which of them it marks important, their canonical
    // specified values and their authored aliases, and whether that inventory describes everything
    // the block can contribute.
    // Only a property some rule declares can be a candidate for a winner change.
    void set_rule_declared_properties(StyleEngineRuleID rule, ReadonlySpan<u16> properties, ReadonlySpan<bool> important, ReadonlySpan<StyleEngineFFI::FfiCascadeOperator> operators, ReadonlySpan<void const*> values, ReadonlySpan<void const*> original_values, bool declarations_are_complete);
    // Which longhand properties one of an element's own declarations covers, their canonical
    // specified values and their authored aliases, and whether the inventory has complete
    // continuation semantics.
    void set_element_declared_properties(StyleNodeID node, StyleEngineFFI::FfiElementDeclarationKind, ReadonlySpan<u16> properties, ReadonlySpan<bool> important, ReadonlySpan<StyleEngineFFI::FfiCascadeOperator> operators, ReadonlySpan<void const*> values, ReadonlySpan<void const*> original_values, bool declarations_are_complete);
    struct StyleRecordDelta {
        StyleRecordID old_style_record;
        StyleRecordID new_style_record;
    };
    using StyleRecordView = StyleEngineFFI::FfiStyleRecordView;
    using ExactCascadePublication = StyleEngineFFI::FfiExactCascadePublication;
    // The returned assignments borrow Rust storage until the next mutable engine call or an
    // explicit discard. Consume them synchronously before asking the engine anything else.
    [[nodiscard]] ReadonlySpan<ComputedValuesFFI::FfiSourceSlotAssignment> materialize_retained_cascade_state(StyleNodeID node, u8 pseudo_kind, ComputedValuesFFI::CascadedPropertyStore*, ReadonlySpan<ComputedValuesFFI::FfiCascadeBlock>);
    void discard_retained_cascade_assignments();
    [[nodiscard]] ExactCascadePublication publish_exact_cascade_state(StyleNodeID node, u8 pseudo_kind, ComputedValuesFFI::CascadedPropertyStore const*, u8 inherited_style_groups = 0, StyleNodeID donor_node = {}, StyleRecordID donor_style_record = {});
    // Publish the immutable input identities of an element or pseudo-element's base style and
    // return its previous and current StyleRecordID assignments. A zero node interns an unassigned
    // record for a style target which is not registered in the engine.
    [[nodiscard]] StyleRecordDelta publish_computed_groups(StyleNodeID node, u8 pseudo_kind, ReadonlySpan<void const*> payloads, size_t inherited_group_count, u64 custom_property_environment, bool inherited_group_swap_candidate, u64 counter_style_environment_identity, u64 animation_overlay_identity, void const* animated_overlay, ReadonlySpan<void const*> animation_overlay_payloads, void const* computed_longhand_table);
    [[nodiscard]] Optional<StyleRecordDelta> publish_animation_overlay(StyleNodeID node, u8 pseudo_kind, u64 animation_overlay_identity, void const* animated_overlay, ReadonlySpan<void const*> payloads);
    [[nodiscard]] StyleRecordDelta assign_shared_style_record(StyleNodeID node, u8 pseudo_kind, StyleRecordID style_record, bool inherited_group_swap_eligible);
    // The borrowed payload array is stable while a base record exists or an animation-overlay
    // generation remains assigned or pinned.
    [[nodiscard]] void const* style_record_payloads(StyleRecordID style_record) const;
    [[nodiscard]] u8 style_record_dependency_flags(StyleRecordID style_record) const;
    [[nodiscard]] u64 style_record_custom_property_environment(StyleRecordID style_record) const;
    void begin_computed_record_verification();
    void end_computed_record_verification();
    [[nodiscard]] u32 compare_style_records(StyleRecordID old_style_record, StyleRecordID new_style_record, bool font_lists_equal, bool element_folds_transform_into_layout) const;
    [[nodiscard]] bool animation_overlay_changed(StyleRecordID old_style_record, void const* animated_overlay) const;
    [[nodiscard]] StyleEngineFFI::FfiAnimationInvalidation compare_animation_overlay(StyleRecordID old_style_record, void const* animated_overlay, ReadonlySpan<void const*> payloads, bool is_document_element) const;
    [[nodiscard]] StyleRecordView style_record_view(StyleRecordID style_record) const;
    void decide_transitions(StyleRecordID before_style_record, void const* after_longhand_table, void const* after_animated_overlay, StyleValueFFI::FfiTransitionInput&, StyleValueFFI::FfiTransitionAction*) const;
    // Remove the retained input identities for one pseudo-element kind and return its removal.
    [[nodiscard]] StyleRecordDelta remove_computed_pseudo(StyleNodeID node, u8 pseudo_kind);
    // The `@namespace` declarations in scope for a rule's selectors. A prefix means whatever its
    // sheet says it means and an unprefixed type or universal selector means the sheet's default
    // namespace, both of which are CSSOM state, so they are resolved to atoms here and looked up by
    // the selector compiler. A zero default means the sheet declared none.
    // A default namespace declared as the empty string, which constrains a name to elements in no
    // namespace. Zero means no default was declared at all, which constrains nothing.
    static constexpr StyleAtomID no_namespace { 0xffffffff };
    struct NamespaceScope {
        StyleAtomID default_namespace;
        Vector<StyleAtomID> prefixes;
        Vector<StyleAtomID> uris;
    };
    // How many of `scope_roots` and `scope_limits` each enclosing `@scope` contributed, outermost
    // first. Scopes nest and an element is in scope only when it is in every one of them, so the
    // boundaries decide the answer: without them the roots of two levels read as alternatives.
    struct ScopeLevels {
        Vector<u32> root_counts;
        Vector<u32> limit_counts;
        // The style node each level roots at when it wrote no `<scope-start>`, and 0 where it wrote
        // one. That root is an element rather than a selector, so the caller resolves it here and
        // the engine names it by identity.
        Vector<StyleNodeID> implicit_roots;
    };
    [[nodiscard]] StyleEngineRuleID add_style_rule(SheetID sheet, StyleEngineRuleID before_rule, ReadonlySpan<void const*> selectors, NamespaceScope const&, ReadonlySpan<void const*> scope_roots, ReadonlySpan<void const*> scope_limits, ScopeLevels const&);
    void replace_style_rule_selectors(StyleEngineRuleID rule, ReadonlySpan<void const*> selectors, NamespaceScope const&, ReadonlySpan<void const*> scope_roots, ReadonlySpan<void const*> scope_limits, ScopeLevels const&);
    void finish_sheet_rules_replacement(SheetID sheet);
    // A fresh identity for an element-sourced declaration block.
    //
    // A block's contents change while the CSSOM object stays the same, so its address is not what
    // makes one version of it different from the next. A version is: an edit that reported the same
    // identity on both sides would cancel in the journal and invalidate nothing.
    [[nodiscard]] u32 next_declaration_block_version() { return ++m_declaration_block_version; }

    // Interns one selector-mentioned name and returns its process-global atom, retained by this
    // document.
    //
    // Utf16FlyString is already interned, so its one-word raw form is the identity: this is a hash
    // lookup on that word plus one reference to keep the name alive. No string is copied, and
    // neither side pays an ASCII or UTF-16 conversion for a fact a u32 comparison answers.
    StyleAtomID intern_atom(Utf16FlyString const&);
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

    // Interns the exact identity an attribute fact uses and memoizes its namespace and folded
    // forms. Demand expansion revisits every live attribute, so these forms must not cross the
    // boundary again merely to recover an already published name.
    StyleAtomID intern_attribute_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_uri);

    // Interns an attribute value and records what it spells when a selector for this name needs
    // text. Values repeat heavily, so demanded text crosses once per distinct value.
    StyleAtomID intern_attribute_value(StyleAtomID name, Utf16View value);
    // Demand expansion already has every value identity. Check the name before interning the text
    // so attributes no selector reads do not pay another string hash.
    void backfill_attribute_value_text_if_required(StyleAtomID name, Utf16View value);

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
        IsDisplayNone = 1 << 8,
        InDisplayNoneSubtree = 1 << 9,
        HasStyle = 1 << 10,
        DisplayChanged = 1 << 11,
    };
    void record_element_style_input_change(StyleNodeID style_node, u8 reaction = PublishedStyle | RecomputeStyle, u8 inherited_style_groups = 0);
    void record_flat_tree_descendant_style_input_changes(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups = 0);
    [[nodiscard]] Vector<StyleNodeID> viewport_dependent_style_nodes();
    [[nodiscard]] bool has_recorded_element_style_input_change(StyleNodeID style_node) const;
    void record_benchmark_marker(Utf16View);
    [[nodiscard]] bool has_recorded_input() const;
    [[nodiscard]] bool has_pending_transaction() const;
    [[nodiscard]] bool has_deferred_geometry_transaction() const;
    [[nodiscard]] bool has_deferred_element_style_inputs() const;
    [[nodiscard]] bool has_deferred_element_style_input(StyleNodeID style_node) const;
    [[nodiscard]] bool pending_transaction_may_affect_layout_geometry();
    [[nodiscard]] bool defer_pending_transaction_for_geometry_read();
    [[nodiscard]] bool begin_deferred_geometry_transaction_flush();
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
    };

    // Takes pending inputs. The diagnostic transaction reports reaction nodes and then discards
    // its matching outputs. The style transaction publishes versioned match-answer records. False
    // means the result is broad enough to prefer complete matching scratch.
    // NB: The returned reactions borrow Rust storage until the next mutable engine call or an
    //     explicit discard. Consume them synchronously before asking the engine anything else.
    bool take_diagnostic_style_transaction(StyleNodeID root, Function<void(ReadonlySpan<StyleNodeID>)>&&);
    PublishedStyleTransaction take_style_transaction(StyleNodeID root);
    void sort_style_deltas_for_direct_application(Span<PublishedStyleDelta>) const;
    void discard_style_transaction_outputs();

    using RuleMatch = StyleEngineFFI::FfiRuleMatch;

    enum class MatchPurpose {
        Exact,
        Cascade,
    };

    // Every rule that decides for one element, in the order the cascade applies them. Cascade
    // callers may omit rules whose declarations cannot win; exact callers receive the same answer
    // as the document pass. Returns false when matching could not complete.
    bool match_element(StyleNodeID node, Vector<RuleMatch>&, MatchPurpose);
    // Reads the complete match answer published by the style transaction which opened the active
    // traversal. False means that transaction did not publish an answer for this node.
    bool consume_published_match_answer(StyleNodeID node, Vector<RuleMatch>&);
    void* compile_selector_query(ReadonlySpan<void const*> selectors);
    // For an engine with no StyleComputer to reach its elements through: when the new query demands attribute value
    // text that earlier facts were published without, the callback republishes every attribute value the engine holds.
    void* compile_selector_query(ReadonlySpan<void const*> selectors, Function<void()> const& backfill_attribute_value_texts);
    static void destroy_selector_query(void*);
    void prepare_selector_query();
    Optional<bool> selector_query_matches(void const* query, StyleNodeID node, StyleNodeID scope_root, StyleNodeID shadow_root);
    Optional<bool> selector_query_matches_without_document_root(void const* query, StyleNodeID node, StyleNodeID scope_root, StyleNodeID shadow_root);
    bool selector_query_all(void* query, StyleNodeID root, bool include_root, StyleNodeID scope_root, StyleNodeID shadow_root, bool has_document_root, Vector<StyleNodeID>& matches);
    bool selector_query_first(void* query, StyleNodeID root, bool include_root, StyleNodeID scope_root, StyleNodeID shadow_root, bool has_document_root, StyleNodeID& matched);

    // Enumerates the engine's counters. Returns false once index is past the last counter.
    bool counter(size_t index, StringView& out_name, u64& out_value) const;

    [[nodiscard]] void const* rust_handle() const { return m_impl; }

private:
    using InputTransaction = StyleEngineFFI::FfiStyleInputTransaction;

    bool read_matches(StyleNodeID, Vector<RuleMatch>&, Optional<MatchPurpose>);
    void apply_transaction(InputTransaction const&);
    void submit_recorded_input();
    bool refresh_attribute_value_text_requirements();
    [[nodiscard]] bool attribute_name_requires_value_text(StyleAtomID);
    void publish_attribute_value_text(StyleAtomID, Utf16View);

    void* m_impl { nullptr };
    GC::Ptr<StyleComputer> m_style_computer;

    HashMap<FlatPtr, StyleAtomID> m_atoms;
    HashTable<StyleAtomID> m_published_language_atoms;
    HashMap<StyleAtomID, HashMap<StyleAtomID, StyleAtomID>> m_attribute_name_atoms;
    HashMap<StyleAtomID, bool> m_attribute_names_requiring_value_text;
    u64 m_atom_generation { 1 };
    u64 m_attribute_value_text_requirements_version { 0 };
    HashTable<StyleNodeID> m_nodes_with_pending_initial_features;
    HashTable<StyleNodeID> m_nodes_awaiting_first_style_computation;
    HashMap<StyleNodeID, TreeScopeID> m_preallocated_style_nodes;
    size_t m_element_match_capacity { 64 };

    u32 m_declaration_block_version { 1 };
    Vector<StyleEngineFFI::FfiTreeDelta> m_tree_deltas;
    Vector<StyleEngineFFI::FfiElementArrival> m_element_arrivals;
    Vector<u32> m_arrival_custom_state_atoms;
    Vector<StyleEngineFFI::FfiLocalFeatureDelta> m_local_feature_deltas;
    Vector<StyleEngineFFI::FfiStateDelta> m_state_deltas;
    Vector<StyleEngineFFI::FfiElementDeclarationDelta> m_element_declaration_deltas;
    bool m_css_transitions_may_observe_style_changes { false };
};

}
