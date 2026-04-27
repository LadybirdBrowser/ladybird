/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
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

struct DescendantInvalidationRule {
    InvalidationSet match_set;
    bool match_any { false };
    NonnullRefPtr<InvalidationPlan> payload;

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
    NonnullRefPtr<InvalidationPlan> payload;

    bool operator==(SiblingInvalidationRule const&) const;
};

struct InvalidationPlan final : RefCounted<InvalidationPlan> {
    static NonnullRefPtr<InvalidationPlan> create() { return adopt_ref(*new InvalidationPlan); }

    bool is_empty() const;
    void include_all_from(InvalidationPlan const&);
    bool operator==(InvalidationPlan const&) const;

    bool invalidate_self { false };
    bool invalidate_whole_subtree { false };
    Vector<DescendantInvalidationRule> descendant_rules;
    Vector<SiblingInvalidationRule> sibling_rules;
};

struct HasInvalidationMetadata {
    Selector const* relative_selector { nullptr };
    HasArgumentScope scope { HasArgumentScope::Complex };

    bool operator==(HasInvalidationMetadata const&) const = default;
};

struct StyleInvalidationData;

void build_invalidation_sets_for_simple_selector(Selector::SimpleSelector const&, InvalidationSet&, ExcludePropertiesNestedInNotPseudoClass, StyleInvalidationData&, InsideNthChildPseudoClass);

struct StyleInvalidationData {
    HashMap<InvalidationSet::Property, NonnullRefPtr<InvalidationPlan>> invalidation_plans;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> ids_used_in_has_selectors;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> class_names_used_in_has_selectors;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> attribute_names_used_in_has_selectors;
    HashMap<FlyString, Vector<HasInvalidationMetadata>> tag_names_used_in_has_selectors;
    HashMap<PseudoClass, Vector<HasInvalidationMetadata>> pseudo_classes_used_in_has_selectors;
    bool has_selectors_sensitive_to_featureless_subtree_changes { false };

    void build_invalidation_sets_for_selector(Selector const& selector);
};

}
