/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct StyleInvalidationData {
    HashMap<InvalidationSet::Property, InvalidationSet> descendant_invalidation_sets;
    HashTable<FlyString> ids_used_in_has_selectors;
    HashTable<FlyString> class_names_used_in_has_selectors;
    HashTable<FlyString> attribute_names_used_in_has_selectors;
    HashTable<FlyString> tag_names_used_in_has_selectors;
    HashTable<PseudoClass> pseudo_classes_used_in_has_selectors;

    void build_invalidation_sets_for_selector(Selector const& selector);
};

}
