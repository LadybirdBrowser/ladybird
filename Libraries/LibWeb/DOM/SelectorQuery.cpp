/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::DOM {

class IsolatedSelectorQueryEngine {
public:
    IsolatedSelectorQueryEngine(ParentNode& root, CSS::SelectorList const& selectors)
        : m_engine(CSS::StyleEngine::DeviceClass::ForegroundDesktop)
        , m_has_document_root(is<Document>(root))
    {
        // Attribute-name and default value case behavior are compiled into the query, so publish
        // the document kind before compiling rather than while facts are populated afterward.
        CSS::configure_isolated_selector_query_engine(m_engine, root.document());
        Vector<void const*> selector_handles;
        selector_handles.ensure_capacity(selectors.size());
        for (auto const& selector : selectors)
            selector_handles.unchecked_append(&selector->rust_selector());
        m_query = m_engine.compile_selector_query(selector_handles);

        CSS::populate_isolated_selector_query_engine(m_engine, root, [&](GC::Ref<Element> element, CSS::StyleNodeID identity) {
            m_identities.set(element, identity);
        });
    }

    ~IsolatedSelectorQueryEngine()
    {
        CSS::StyleEngine::destroy_selector_query(m_query);
    }

    bool matches(Element const& element, ParentNode const& scope)
    {
        auto node = m_identities.get(GC::Ptr { element });
        VERIFY(node.has_value());
        CSS::StyleNodeID scope_root;
        if (GC::Ptr<Element const> scope_element = as_if<Element>(scope))
            scope_root = m_identities.get(scope_element).value_or(0);
        auto result = m_has_document_root
            ? m_engine.selector_query_matches(m_query, *node, scope_root, 0)
            : m_engine.selector_query_matches_without_document_root(m_query, *node, scope_root, 0);
        VERIFY(result.has_value());
        return *result;
    }

private:
    CSS::StyleEngine m_engine;
    HashMap<GC::Ptr<Element const>, CSS::StyleNodeID> m_identities;
    void* m_query { nullptr };
    bool m_has_document_root { false };
};

class IsolatedSelectorQueryCacheEntry {
public:
    IsolatedSelectorQueryCacheEntry(ParentNode& root, CSS::SelectorList const& selectors)
        : root(root)
        , dom_tree_version(root.document().dom_tree_version())
        , character_data_version(root.document().character_data_version())
        , engine(root, selectors)
    {
    }

    GC::Weak<ParentNode> root;
    u64 dom_tree_version { 0 };
    u64 character_data_version { 0 };
    IsolatedSelectorQueryEngine engine;
};

template<typename Matcher>
static GC::Ptr<Element> first_match(ParentNode& root, Matcher&& matcher)
{
    GC::Ptr<Element> result;
    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    root.for_each_in_subtree_of_type<Element>([&](auto& element) {
        if (matcher(element)) {
            result = GC::Ptr { element };
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return result;
}

template<typename Matcher>
static void collect_matches(ParentNode& root, Matcher&& matcher, Vector<GC::RawPtr<Element>>& elements)
{
    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    root.for_each_in_subtree_of_type<Element>([&](auto& element) {
        if (matcher(element))
            elements.append(element);
        return TraversalDecision::Continue;
    });
}

static void settle_connected_selector_query(Document& document)
{
    document.style_computer().style_engine().prepare_selector_query();
}

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

SelectorQuery::SelectorQuery(Document& document, CSS::SelectorList&& selectors)
    : m_selectors(move(selectors))
{
    // An early script can compile a query before the first sheet attaches, which is otherwise what
    // tells the engine whether HTML name folding applies in this document.
    CSS::record_document_kind(document);
    Vector<void const*> selector_handles;
    selector_handles.ensure_capacity(m_selectors.size());
    for (auto const& selector : m_selectors)
        selector_handles.unchecked_append(&selector->rust_selector());
    m_engine_query = document.style_computer().style_engine().compile_selector_query(selector_handles);
    m_can_match_in_dom = m_selectors.size() == 1
        && CSS::SelectorFFI::rust_selector_supports_simple_dom_matching(&m_selectors.first()->rust_selector());

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

SelectorQuery::~SelectorQuery()
{
    CSS::StyleEngine::destroy_selector_query(m_engine_query);
}

static uintptr_t interned_name_identity(Utf16FlyString const& name)
{
    return name.raw_identity();
}

bool SelectorQuery::matches_simple_selector_in_dom(Element const& element) const
{
    auto const& classes = element.class_names();
    Vector<uintptr_t, 8> class_identities;
    Vector<Utf16FlyString, 8> lowercase_classes;
    Vector<uintptr_t, 8> lowercase_class_identities;
    class_identities.ensure_capacity(classes.size());
    for (auto const& class_name : classes)
        class_identities.unchecked_append(interned_name_identity(class_name));
    if (element.document().in_quirks_mode()) {
        lowercase_classes.ensure_capacity(classes.size());
        lowercase_class_identities.ensure_capacity(classes.size());
        for (auto const& class_name : classes)
            lowercase_classes.unchecked_append(class_name.to_ascii_lowercase());
        for (auto const& class_name : lowercase_classes)
            lowercase_class_identities.unchecked_append(interned_name_identity(class_name));
    }

    auto id = element.id().value_or({});
    auto result = CSS::SelectorFFI::rust_selector_matches_simple_dom(
        &m_selectors.first()->rust_selector(),
        interned_name_identity(element.local_name()),
        interned_name_identity(element.lowercased_local_name()),
        interned_name_identity(id),
        interned_name_identity(id.to_ascii_lowercase()),
        class_identities.data(),
        lowercase_class_identities.data(),
        class_identities.size(),
        element.is_html_element() && element.document().is_html_document(),
        element.document().in_quirks_mode());
    VERIFY(result != NumericLimits<u8>::max());
    return result != 0;
}

bool SelectorQuery::matches(Element const& element, ParentNode const& scope) const
{
    if (m_can_match_in_dom)
        return matches_simple_selector_in_dom(element);

    auto& document = const_cast<Document&>(element.document());
    if (element.is_connected()) {
        settle_connected_selector_query(document);
        return matches_in_style_engine(element, scope);
    }

    auto& root = as<ParentNode>(const_cast<Node&>(element.root()));
    if (m_is_result_cacheable)
        return isolated_engine_for(root).matches(element, scope);
    IsolatedSelectorQueryEngine engine(root, m_selectors);
    return engine.matches(element, scope);
}

bool SelectorQuery::matches_in_style_engine(Element const& element, ParentNode const& scope) const
{
    VERIFY(element.style_node_id() != 0);

    CSS::StyleNodeID scope_root;
    if (GC::Ptr<Element const> scope_element = as_if<Element>(scope))
        scope_root = scope_element->style_node_id();

    CSS::StyleNodeID shadow_root;
    if (GC::Ptr<ShadowRoot const> root = as_if<ShadowRoot>(element.root()))
        shadow_root = root->style_node_id();

    auto result = const_cast<Document&>(element.document()).style_computer().style_engine().selector_query_matches(m_engine_query, element.style_node_id(), scope_root, shadow_root);
    VERIFY(result.has_value());
    return *result;
}

IsolatedSelectorQueryEngine& SelectorQuery::isolated_engine_for(ParentNode& root) const
{
    auto dom_tree_version = root.document().dom_tree_version();
    auto character_data_version = root.document().character_data_version();
    for (size_t index = 0; index < m_isolated_engine_cache.size();) {
        auto& entry = *m_isolated_engine_cache[index];
        if (!entry.root) {
            m_isolated_engine_cache.remove(index);
            continue;
        }
        if (entry.root.ptr() != GC::Ptr { root }) {
            ++index;
            continue;
        }
        if (entry.dom_tree_version == dom_tree_version && entry.character_data_version == character_data_version)
            return entry.engine;
        m_isolated_engine_cache.remove(index);
        break;
    }

    static constexpr size_t max_isolated_engine_cache_size = 64;
    if (m_isolated_engine_cache.size() >= max_isolated_engine_cache_size)
        m_isolated_engine_cache.remove(0);
    auto entry = make<IsolatedSelectorQueryCacheEntry>(root, m_selectors);
    auto& engine = entry->engine;
    m_isolated_engine_cache.append(move(entry));
    return engine;
}

GC::Ptr<Element const> SelectorQuery::closest(Element const& element) const
{
    if (m_can_match_in_dom) {
        if (matches_simple_selector_in_dom(element))
            return &element;
        for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
            if (matches_simple_selector_in_dom(*ancestor))
                return ancestor;
        }
        return nullptr;
    }

    if (element.is_connected()) {
        settle_connected_selector_query(const_cast<Document&>(element.document()));
        for (GC::Ptr<Element const> ancestor = GC::Ptr { element }; ancestor; ancestor = ancestor->parent_element()) {
            if (matches_in_style_engine(*ancestor, element))
                return ancestor;
        }
        return nullptr;
    }

    auto& root = as<ParentNode>(const_cast<Node&>(element.root()));
    if (m_is_result_cacheable) {
        auto& engine = isolated_engine_for(root);
        for (GC::Ptr<Element const> ancestor = &element; ancestor; ancestor = ancestor->parent_element()) {
            if (engine.matches(*ancestor, element))
                return ancestor;
        }
        return nullptr;
    }

    IsolatedSelectorQueryEngine engine(root, m_selectors);
    for (GC::Ptr<Element const> ancestor = GC::Ptr { element }; ancestor; ancestor = ancestor->parent_element()) {
        if (engine.matches(*ancestor, element))
            return ancestor;
    }
    return nullptr;
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors,
// stopping at the first match.
GC::Ptr<Element> SelectorQuery::query_first(ParentNode& root) const
{
    auto cache_result = [&](GC::Ptr<Element> result) {
        if (m_is_result_cacheable) {
            Vector<GC::RawPtr<Element>> elements;
            if (result)
                elements.append(*result);
            root.document().query_selector_result_cache().set(root.document(), root, *this, QuerySelectorResultCache::ResultType::FirstOnly, move(elements));
        }
        return result;
    };

    if (m_is_result_cacheable) {
        auto& document = root.document();
        if (auto const* cached_elements = document.query_selector_result_cache().get(document, root, *this, QuerySelectorResultCache::ResultType::FirstOnly))
            return cached_elements->is_empty() ? nullptr : cached_elements->first().ptr();
    }

    if (!root.is_connected()) {
        auto& tree_root = as<ParentNode>(root.root());
        if (m_is_result_cacheable) {
            auto& engine = isolated_engine_for(tree_root);
            return cache_result(first_match(root, [&](auto& element) { return engine.matches(element, root); }));
        }
        IsolatedSelectorQueryEngine engine(tree_root, m_selectors);
        return first_match(root, [&](auto& element) { return engine.matches(element, root); });
    }

    settle_connected_selector_query(root.document());
    return cache_result(first_match(root, [&](auto& element) { return matches_in_style_engine(element, root); }));
}

static GC::Ref<NodeList> create_node_list(Vector<GC::RawPtr<Element>> const& elements)
{
    Vector<GC::Root<Node>> nodes;
    nodes.ensure_capacity(elements.size());
    for (auto const& element : elements)
        nodes.unchecked_append(GC::make_root(static_cast<Node&>(*element)));
    return StaticNodeList::create(move(nodes));
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors.
GC::Ref<NodeList> SelectorQuery::query_all(ParentNode& root) const
{
    auto& document = root.document();

    if (m_is_result_cacheable) {
        if (auto const* cached_elements = document.query_selector_result_cache().get(document, root, *this, QuerySelectorResultCache::ResultType::All))
            return create_node_list(*cached_elements);
    }

    Vector<GC::RawPtr<Element>> elements;
    if (!root.is_connected()) {
        auto& tree_root = as<ParentNode>(root.root());
        if (m_is_result_cacheable) {
            auto& engine = isolated_engine_for(tree_root);
            collect_matches(root, [&](auto& element) { return engine.matches(element, root); }, elements);
        } else {
            IsolatedSelectorQueryEngine engine(tree_root, m_selectors);
            collect_matches(root, [&](auto& element) { return engine.matches(element, root); }, elements);
        }
    } else {
        settle_connected_selector_query(document);
        collect_matches(root, [&](auto& element) { return matches_in_style_engine(element, root); }, elements);
    }

    auto node_list = create_node_list(elements);
    if (m_is_result_cacheable)
        document.query_selector_result_cache().set(document, root, *this, QuerySelectorResultCache::ResultType::All, move(elements));
    return node_list;
}

void QuerySelectorResultCache::clear_if_dom_tree_changed(Document const& document)
{
    if (m_dom_tree_version == document.dom_tree_version())
        return;
    m_entries.clear();
    m_dom_tree_version = document.dom_tree_version();
}

Vector<GC::RawPtr<Element>> const* QuerySelectorResultCache::get(Document const& document, ParentNode const& root, SelectorQuery const& query, ResultType result_type)
{
    clear_if_dom_tree_changed(document);

    auto it = m_entries.find(Key { &root, &query });
    if (it == m_entries.end())
        return nullptr;

    auto const& entry = it->value;
    if (entry.root.ptr().ptr() != &root
        || (query.depends_on_character_data() && entry.character_data_version != document.character_data_version())) {
        m_entries.remove(it);
        return nullptr;
    }
    if (result_type == ResultType::All && entry.result_type != ResultType::All)
        return nullptr;

    return &entry.elements;
}

void QuerySelectorResultCache::set(Document const& document, ParentNode const& root, SelectorQuery const& query, ResultType result_type, Vector<GC::RawPtr<Element>> elements)
{
    static constexpr size_t MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE = 64;

    clear_if_dom_tree_changed(document);

    auto key = Key { &root, &query };
    if (!m_entries.contains(key) && m_entries.size() >= MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE)
        m_entries.remove(m_entries.begin());

    m_entries.set(key, Entry { root, query, document.character_data_version(), result_type, move(elements) });
}

void QuerySelectorResultCache::visit_edges(GC::Cell::Visitor&)
{
    // Entries intentionally contain only weak or raw GC pointers, so the cache does not keep query roots or matched
    // elements alive.
    (void)m_entries;
}

}
