/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/StaticNodeList.h>

namespace Web::DOM {

SelectorQuery::SelectorQuery(CSS::SelectorList&& selectors)
    : m_selectors(move(selectors))
{
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors,
// stopping at the first match.
GC::Ptr<Element> SelectorQuery::query_first(ParentNode& root) const
{
    GC::Ptr<Element> result;
    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    root.for_each_in_subtree_of_type<Element>([&](auto& element) {
        for (auto const& selector : m_selectors) {
            SelectorEngine::MatchContext context;
            if (SelectorEngine::matches(selector, element, nullptr, context, root)) {
                result = &element;
                return TraversalDecision::Break;
            }
        }
        return TraversalDecision::Continue;
    });
    return result;
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors.
GC::Ref<NodeList> SelectorQuery::query_all(ParentNode& root) const
{
    Vector<GC::Root<Node>> results;
    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    root.for_each_in_subtree_of_type<Element>([&](auto& element) {
        for (auto const& selector : m_selectors) {
            SelectorEngine::MatchContext context;
            if (SelectorEngine::matches(selector, element, nullptr, context, root)) {
                results.append(element);
                break;
            }
        }
        return TraversalDecision::Continue;
    });
    return StaticNodeList::create(root.realm(), move(results));
}

}
