/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/StaticNodeList.h>

namespace Web::DOM {

// Whether matching this pseudo-class can only change when Document::dom_tree_version() or
// Document::character_data_version() change, i.e. it depends only on tree structure, attributes and character
// data. Pseudo-classes that also depend on other state (user interaction, element states like checkedness or
// validity, navigation, history, custom element upgrades, ...) must not be on this list.
static bool pseudo_class_matching_is_covered_by_version_counters(CSS::PseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case CSS::PseudoClass::AnyLink: // Presence of an href attribute.
    case CSS::PseudoClass::Empty:   // Child nodes and their character data.
    case CSS::PseudoClass::FirstChild:
    case CSS::PseudoClass::FirstOfType:
    case CSS::PseudoClass::Has: // Container; argument selectors are checked via the recursive pseudo-class bitmap.
    case CSS::PseudoClass::Heading:
    case CSS::PseudoClass::Is:
    case CSS::PseudoClass::LastChild:
    case CSS::PseudoClass::LastOfType:
    case CSS::PseudoClass::Link: // Matches like :any-link; we treat all links as unvisited.
    case CSS::PseudoClass::Not:
    case CSS::PseudoClass::NthChild:
    case CSS::PseudoClass::NthLastChild:
    case CSS::PseudoClass::NthLastOfType:
    case CSS::PseudoClass::NthOfType:
    case CSS::PseudoClass::OnlyChild:
    case CSS::PseudoClass::OnlyOfType:
    case CSS::PseudoClass::Optional:
    case CSS::PseudoClass::Required:
    case CSS::PseudoClass::Root:
    case CSS::PseudoClass::Scope: // The scoping root is part of the result cache key.
    case CSS::PseudoClass::Where:
        return true;
    default:
        return false;
    }
}

SelectorQuery::SelectorQuery(CSS::SelectorList&& selectors)
    : m_selectors(move(selectors))
{
    m_is_result_cacheable = true;
    for (auto const& selector : m_selectors) {
        // NB: The contained-pseudo-class bitmap may be under-collected for selectors containing the nesting
        //     selector, so we cannot rely on it (and `&` has no useful meaning in a query anyway).
        if (selector->contains_the_nesting_selector()) {
            m_is_result_cacheable = false;
            break;
        }
        for (size_t i = 0; i < to_underlying(CSS::PseudoClass::__Count); ++i) {
            auto pseudo_class = static_cast<CSS::PseudoClass>(i);
            if (!selector->contains_pseudo_class(pseudo_class))
                continue;
            if (!pseudo_class_matching_is_covered_by_version_counters(pseudo_class)) {
                m_is_result_cacheable = false;
                break;
            }
            if (pseudo_class == CSS::PseudoClass::Empty)
                m_depends_on_character_data = true;
        }
        if (!m_is_result_cacheable)
            break;
    }
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors,
// stopping at the first match.
GC::Ptr<Element> SelectorQuery::query_first(ParentNode& root) const
{
    if (m_is_result_cacheable) {
        auto& document = root.document();
        if (auto const* cached_elements = document.query_selector_result_cache().get(document, root, *this))
            return cached_elements->is_empty() ? nullptr : cached_elements->first().ptr();
    }

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

static GC::Ref<NodeList> create_node_list(JS::Realm& realm, Vector<GC::RawPtr<Element>> const& elements)
{
    Vector<GC::Root<Node>> nodes;
    nodes.ensure_capacity(elements.size());
    for (auto const& element : elements)
        nodes.unchecked_append(GC::make_root(static_cast<Node&>(*element)));
    return StaticNodeList::create(realm, move(nodes));
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors.
GC::Ref<NodeList> SelectorQuery::query_all(ParentNode& root) const
{
    auto& document = root.document();

    if (m_is_result_cacheable) {
        if (auto const* cached_elements = document.query_selector_result_cache().get(document, root, *this))
            return create_node_list(root.realm(), *cached_elements);
    }

    Vector<GC::RawPtr<Element>> elements;
    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    root.for_each_in_subtree_of_type<Element>([&](auto& element) {
        for (auto const& selector : m_selectors) {
            SelectorEngine::MatchContext context;
            if (SelectorEngine::matches(selector, element, nullptr, context, root)) {
                elements.append(element);
                break;
            }
        }
        return TraversalDecision::Continue;
    });

    auto node_list = create_node_list(root.realm(), elements);
    if (m_is_result_cacheable)
        document.query_selector_result_cache().set(document, root, *this, move(elements));
    return node_list;
}

void QuerySelectorResultCache::clear_if_dom_tree_changed(Document const& document)
{
    if (m_dom_tree_version == document.dom_tree_version())
        return;
    m_entries.clear();
    m_dom_tree_version = document.dom_tree_version();
}

Vector<GC::RawPtr<Element>> const* QuerySelectorResultCache::get(Document const& document, ParentNode const& root, SelectorQuery const& query)
{
    clear_if_dom_tree_changed(document);

    auto it = m_entries.find(Key { &root, &query });
    if (it == m_entries.end())
        return nullptr;

    auto const& entry = it->value;
    if (entry.root.ptr() != &root
        || (query.depends_on_character_data() && entry.character_data_version != document.character_data_version())) {
        m_entries.remove(it);
        return nullptr;
    }

    return &entry.elements;
}

void QuerySelectorResultCache::set(Document const& document, ParentNode const& root, SelectorQuery const& query, Vector<GC::RawPtr<Element>> elements)
{
    static constexpr size_t MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE = 64;

    clear_if_dom_tree_changed(document);

    if (m_entries.size() >= MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE)
        m_entries.remove(m_entries.begin());

    m_entries.set(Key { &root, &query }, Entry { root, query, document.character_data_version(), move(elements) });
}

void QuerySelectorResultCache::visit_edges(GC::Cell::Visitor&)
{
    // Entries intentionally contain only weak or raw GC pointers, so the cache does not keep query roots or matched
    // elements alive.
    (void)m_entries;
}

}
