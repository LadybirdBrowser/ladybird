/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;
class Node;

}

namespace Web::CSS {

enum class PseudoClass;
struct StyleInvalidationData;
class StyleScope;

namespace Invalidation {

class HasMutationFeatureCollector {
public:
    explicit HasMutationFeatureCollector(StyleInvalidationData const&);

    [[nodiscard]] bool has_any_metadata() const;
    [[nodiscard]] bool element_has_feature_used_in_has_selector(DOM::Element const&) const;
    [[nodiscard]] bool subtree_has_feature_used_in_has_selector(DOM::Node&) const;

private:
    StyleInvalidationData const& m_data;
};

[[nodiscard]] bool element_has_feature_used_in_has_selector(DOM::Element const&, PseudoClass, StyleScope const&);
[[nodiscard]] bool subtree_has_feature_used_in_has_selector(DOM::Node&, StyleScope const&);

}

}
