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

struct InvalidationPlan;

struct DescendantInvalidationRule {
    InvalidationSet match_set;
    bool match_any { false };
    NonnullRefPtr<InvalidationPlan> payload;
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
};

struct InvalidationPlan final : RefCounted<InvalidationPlan> {
    static NonnullRefPtr<InvalidationPlan> create() { return adopt_ref(*new InvalidationPlan); }

    bool is_empty() const;
    void include_all_from(InvalidationPlan const&);

    bool invalidate_self { false };
    bool invalidate_whole_subtree { false };
    Vector<DescendantInvalidationRule> descendant_rules;
    Vector<SiblingInvalidationRule> sibling_rules;
};

struct StyleInvalidationData;

void build_invalidation_sets_for_simple_selector(Selector::SimpleSelector const&, InvalidationSet&, ExcludePropertiesNestedInNotPseudoClass, StyleInvalidationData&, InsideNthChildPseudoClass);

struct StyleInvalidationData {
    HashMap<InvalidationSet::Property, NonnullRefPtr<InvalidationPlan>> invalidation_plans;
    HashTable<FlyString> ids_used_in_has_selectors;
    HashTable<FlyString> class_names_used_in_has_selectors;
    HashTable<FlyString> attribute_names_used_in_has_selectors;
    HashTable<FlyString> tag_names_used_in_has_selectors;
    HashTable<PseudoClass> pseudo_classes_used_in_has_selectors;

    void build_invalidation_sets_for_selector(Selector const& selector);
};

}
