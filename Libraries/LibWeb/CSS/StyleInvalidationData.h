/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

enum class ExcludePropertiesNestedInNotPseudoClass : bool {
    No,
    Yes,
};

enum class InsideNthChildPseudoClass {
    No,
    Yes,
};

enum class HasArgumentScope : u8 {
    ChildrenOnly,
    AllDescendants,
    NextSiblingOnly,
    AllFollowingSiblings,
    Complex,
};

struct InvalidationPlan;

struct InvalidationGuard {
    bool is_empty() const { return property_sets.is_empty(); }
    bool operator==(InvalidationGuard const&) const;

    // Every set is an OR group; the guard matches when every group matches.
    Vector<InvalidationSet> property_sets;
};

struct GuardedInvalidationRule {
    InvalidationGuard guard;
    NonnullRefPtr<InvalidationPlan const> payload;

    bool operator==(GuardedInvalidationRule const&) const;
};

struct DescendantInvalidationRule {
    InvalidationSet match_set;
    bool match_any { false };
    NonnullRefPtr<InvalidationPlan const> payload;

    bool operator==(DescendantInvalidationRule const&) const;
};

enum class SiblingInvalidationReach {
    Adjacent,
    Subsequent,
};

struct SiblingInvalidationRule {
    SiblingInvalidationReach reach;
    InvalidationSet match_set;
    bool match_any { false };
    NonnullRefPtr<InvalidationPlan const> payload;

    bool operator==(SiblingInvalidationRule const&) const;
};

struct InvalidationPlan final : RefCounted<InvalidationPlan> {
    static NonnullRefPtr<InvalidationPlan> create() { return adopt_ref(*new InvalidationPlan); }

    bool is_empty() const;
    void include_all_from(InvalidationPlan const&);
    bool operator==(InvalidationPlan const&) const;

    void add_descendant_rule(DescendantInvalidationRule);
    void add_sibling_rule(SiblingInvalidationRule);
    void add_guarded_rule(GuardedInvalidationRule);

    // The merge index is derived acceleration data, so dropping it does not change logical state.
    void clear_rule_merge_index() const { m_rule_merge_index = nullptr; }

    bool invalidate_self { false };
    bool invalidate_whole_subtree { false };
    bool invalidate_self_and_structurally_affected_siblings { false };
    Vector<DescendantInvalidationRule> descendant_rules;
    Vector<SiblingInvalidationRule> sibling_rules;
    Vector<GuardedInvalidationRule> guarded_rules;

private:
    // Only interning may hash a plan: the hash is cached, so hashing a plan that is still being mutated would go
    // stale. Interned plans are immutable, which interning enforces by handing back a pointer-to-const.
    friend struct StyleInvalidationData;
    u32 hash() const;

    // Merging dedups rules by payload identity: payload plans are interned per StyleInvalidationData build, so
    // structurally equal payloads share one pointer and a pointer-derived key can stand in for deep equality.
    struct RuleMergeIndex {
        HashMap<FlatPtr, size_t> descendant_rule_indexes;
        HashMap<FlatPtr, size_t> sibling_rule_indexes;
    };
    mutable OwnPtr<RuleMergeIndex> m_rule_merge_index;
    mutable Optional<u32> m_hash;

    // Set by interning. Mutating an interned plan would corrupt every plan sharing it, so mutators VERIFY against
    // this. Payload pointers are const, but this also catches mutation through a reference obtained before interning.
    bool m_interned { false };
};

struct HasInvalidationMetadata {
    Selector const* relative_selector { nullptr };
    HasArgumentScope scope { HasArgumentScope::Complex };

    bool operator==(HasInvalidationMetadata const&) const = default;
};

struct StyleInvalidationData;

void build_invalidation_sets_for_simple_selector(Selector::SimpleSelector const&, InvalidationSet&, ExcludePropertiesNestedInNotPseudoClass, StyleInvalidationData&, InsideNthChildPseudoClass);

struct StyleInvalidationData {
    HashMap<FlyString, Vector<HasInvalidationMetadata>> ids_used_in_has_selectors;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> class_names_used_in_has_selectors;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> attribute_names_used_in_has_selectors;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> tag_names_used_in_has_selectors;
    HashMap<PseudoClass, Vector<HasInvalidationMetadata>> pseudo_classes_used_in_has_selectors;
    bool has_selectors_sensitive_to_featureless_subtree_changes { false };

    void build_invalidation_sets_for_selector(Selector const& selector);
    void build_invalidation_sets_for_scope_boundary_selector(Selector const& selector);

    // Returns the canonical plan for the given plan's structure, so that structurally equal payload plans share one
    // pointer within this StyleInvalidationData.
    NonnullRefPtr<InvalidationPlan const> intern_invalidation_plan(NonnullRefPtr<InvalidationPlan>);

    InvalidationPlan& ensure_invalidation_plan_being_built(InvalidationSet::Property const&);

    // Publishes the plans being built into invalidation_plans() and drops build-only acceleration structures (the
    // intern table and per-plan merge indexes).
    void did_finish_building();

    HashMap<InvalidationSet::Property, NonnullRefPtr<InvalidationPlan const>> const& invalidation_plans() const
    {
        // Reading plans that did_finish_building() has not published yet would silently miss invalidations.
        VERIFY(m_finished_building);
        return m_invalidation_plans;
    }

private:
    HashMap<u32, Vector<NonnullRefPtr<InvalidationPlan const>>> m_interned_invalidation_plans;

    // Plans are mutable while selectors are merged into them and become immutable when did_finish_building()
    // publishes them into m_invalidation_plans.
    HashMap<InvalidationSet::Property, NonnullRefPtr<InvalidationPlan>> m_invalidation_plans_being_built;
    HashMap<InvalidationSet::Property, NonnullRefPtr<InvalidationPlan const>> m_invalidation_plans;
    bool m_finished_building { false };
};

}
