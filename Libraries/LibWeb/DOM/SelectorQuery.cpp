/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/QuickSort.h>
#include <LibGC/RootVector.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SelectorRustFFI.h>

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
        auto const facts = CSS::SelectorFFI::rust_selector_query_facts(&selector->rust_selector());
        m_matches_every_element = m_selectors.size() == 1 && facts.matches_every_element;
        // NB: The contained-pseudo-class bitmap may be under-collected for selectors containing the nesting
        //     selector, so we cannot rely on it (and `&` has no useful meaning in a query anyway).
        if (facts.contains_nesting) {
            m_is_result_cacheable = false;
            break;
        }
        for (size_t i = 0; i < to_underlying(CSS::PseudoClass::__Count); ++i) {
            if (!(facts.pseudo_classes[i / 64] & (1ull << (i % 64))))
                continue;
            auto pseudo_class = static_cast<CSS::PseudoClass>(i);
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
    CSS::SelectorFFI::rust_dom_selector_program_destroy(m_program);
}

CSS::SelectorFFI::DomSelectorProgram const& SelectorQuery::program(Document const& document) const
{
    // A document is an HTML document or not for all its life, so a query recompiles only if it is used in both kinds.
    if (m_program && m_program_is_for_html_document == document.is_html_document())
        return *m_program;
    CSS::SelectorFFI::rust_dom_selector_program_destroy(m_program);
    Vector<CSS::SelectorFFI::RustSelector const*> selectors;
    selectors.ensure_capacity(m_selectors.size());
    for (auto const& selector : m_selectors)
        selectors.unchecked_append(&selector->rust_selector());
    m_program_is_for_html_document = document.is_html_document();
    m_program = CSS::SelectorFFI::rust_dom_selector_program_create(selectors.span(), m_program_is_for_html_document ? Namespace::HTML.raw_identity() : 0);
    return *m_program;
}

namespace {

#include <LibWeb/SelectorStateFactsGenerated.inc>

// Every node crosses to the DOM matcher as a Node pointer, and an element is downcast from one where the matcher asks
// about an element.
Element const& element_from_ffi(Node const* element)
{
    return static_cast<Element const&>(*element);
}

CSS::SelectorFFI::FfiUtf16View utf16_view_to_ffi(Utf16View view)
{
    return {
        .latin1 = view.has_latin1_storage() ? view.latin1_span().data() : nullptr,
        .utf16 = view.has_latin1_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

static_assert(sizeof(Utf16FlyString) == sizeof(uintptr_t));

constexpr CSS::SelectorFFI::FfiDomSelectorCallbacks dom_selector_callbacks {
    .synchronize_style_attribute = [](Node const* element) { element_from_ffi(element).synchronize_all_attributes(); },
    .child_index = [](Node const* element, CSS::SelectorFFI::FfiChildIndex which) {
        using enum CSS::SelectorFFI::FfiChildIndex;
        using enum Element::ChildIndexAmong;
        auto const& subject = element_from_ffi(element);
        switch (which) {
        case FromStart:
            return subject.child_index(Siblings);
        case FromEnd:
            return subject.child_index_from_end(Siblings);
        case OfTypeFromStart:
            return subject.child_index(SiblingsOfType);
        case OfTypeFromEnd:
            return subject.child_index_from_end(SiblingsOfType);
        }
        VERIFY_NOT_REACHED(); },
    .name_filter_bit = [](uintptr_t local_name) { return Element::name_filter_bit(Utf16FlyString::from_raw(local_name)); },
    .matches_state = [](Node const* element, u8 state) { return SelectorMatching::element_matches_state(element_from_ffi(element), pseudo_classes_by_state_fact[state]); },
    .language = [](Node const* element) -> CSS::SelectorFFI::FfiUtf16View {
        auto language = element_from_ffi(element).lang_view();
        if (!language.has_value())
            return {};
        return utf16_view_to_ffi(*language);
    },
    .directionality = [](Node const* element) -> size_t {
        auto const& directionality = element_from_ffi(element).directionality() == Element::Directionality::Rtl ? "rtl"_utf16_fly_string : "ltr"_utf16_fly_string;
        return directionality.raw_identity(); },
    .heading_level = [](Node const* element) -> u32 {
        auto const* heading = as_if<HTML::HTMLHeadingElement>(element_from_ffi(element));
        return heading ? heading->heading_level() : 0;
    },
    .has_custom_state = [](Node const* element, size_t state) {
        auto states = element_from_ffi(element).custom_state_set();
        return states && states->has_state(Utf16FlyString::from_raw(state)); },
};

// A selector query as the DOM matcher takes it, made in the context of the tree `node` is in and scoped to `scope`.
class DomSelectorQuery {
public:
    DomSelectorQuery(CSS::SelectorFFI::DomSelectorProgram const& program, Node const& node, ParentNode const& scope)
    {
        auto const& document = node.document();
        // https://drafts.csswg.org/selectors-4/#scope-pseudo
        // If the :scope elements are not explicitly specified, but the selector is scoped and the scoping root is an
        // element, then :scope represents the scoping root; otherwise, it represents the root of the document
        // (equivalent to :root).
        // FIXME: A query scoped to a DocumentFragment or ShadowRoot has that node as a virtual scoping root, which
        //        :scope should represent instead of the document element.
        auto const* scope_element = as_if<Element>(scope);
        m_query = {
            .program = &program,
            .callbacks = &dom_selector_callbacks,
            .scope = scope_element ? scope_element : document.document_element(),
            .shadow_root = as_if<ShadowRoot>(node.root()),
            .ids_and_classes_ignore_case = document.in_quirks_mode(),
        };
    }

    bool matches(Element const& element) const
    {
        return CSS::SelectorFFI::rust_dom_selector_query_matches(&m_query, &element);
    }

    GC::Ptr<Element const> closest(Element const& element) const
    {
        auto const* found = CSS::SelectorFFI::rust_dom_selector_query_closest(&m_query, &element);
        return found ? &element_from_ffi(found) : nullptr;
    }

    GC::Ptr<Element> first(ParentNode& root) const
    {
        GC::Ptr<Element> result;
        for_each_match(root, [&](Element& element) {
            result = element;
            return true;
        });
        return result;
    }

    void collect(ParentNode& root, GC::RootVector<GC::Ref<Node>>& nodes) const
    {
        for_each_match(root, [&](Element& element) {
            nodes.append(element);
            return false;
        });
    }

private:
    // Calls `callback` with each element under `root` that matches the query, in tree order, until it returns true.
    template<typename Callback>
    void for_each_match(ParentNode& root, Callback callback) const
    {
        using enum CSS::SelectorFFI::FfiDomIdLookupKind;
        auto lookup = CSS::SelectorFFI::rust_dom_selector_program_id_lookup(m_query.program);
        // NB: An id the query names is looked up in the id map of the tree the query is made in, which holds the
        //     elements of a connected tree. A quirks-mode document compares ids ignoring case, which the map does not.
        auto const& document = root.document();
        if (lookup.kind == None || !root.is_connected() || document.in_quirks_mode()) {
            for_each_in_subtree(root, callback);
            return;
        }
        auto const& tree_root = root.root();
        auto const* shadow_root = as_if<ShadowRoot>(tree_root);
        auto const& element_by_id = shadow_root ? shadow_root->element_by_id() : document.element_by_id();
        auto elements = element_by_id.elements_with_id(Utf16FlyString::from_raw(lookup.id));
        auto is_under_root = [&](Node const& node) {
            return &tree_root == &root ? &node != &root : node.is_descendant_of(root);
        };

        if (lookup.kind == Subject) {
            // Only the elements with the id can match.
            GC::RootVector<GC::Ref<Element>, 1> candidates;
            for (auto const& element : elements) {
                if (element && is_under_root(*element))
                    candidates.append(*element);
            }
            if (candidates.size() > 1)
                quick_sort(candidates, [](auto const& a, auto const& b) { return a->is_before(*b); });
            for (auto& element : candidates) {
                if (matches(*element) && callback(*element))
                    return;
            }
            return;
        }

        // No element without the id holds a match.
        if (elements.is_empty())
            return;
        // NB: The subtrees of several elements with the id can nest, which only a walk of the whole tree visits once.
        if (elements.size() > 1 || !elements[0]) {
            for_each_in_subtree(root, callback);
            return;
        }
        // Every match is under the element with the id, or under its parent past a sibling combinator.
        Node* search_root = elements[0].ptr().ptr();
        if (lookup.kind == AncestorsParent)
            search_root = search_root->parent();
        if (!search_root || !(search_root == &root || is_under_root(*search_root))) {
            for_each_in_subtree(root, callback);
            return;
        }
        for_each_in_subtree(as<ParentNode>(*search_root), callback);
    }

    template<typename Callback>
    void for_each_in_subtree(ParentNode& root, Callback callback) const
    {
        CSS::SelectorFFI::rust_dom_selector_query_subtree(&m_query, &root, &callback, [](void* context, Node const* element) {
            return (*static_cast<Callback*>(context))(const_cast<Element&>(element_from_ffi(element)));
        });
    }

    CSS::SelectorFFI::FfiDomSelectorQuery m_query {};
};

}

bool SelectorQuery::matches(Element const& element, ParentNode const& scope) const
{
    if (m_matches_every_element)
        return true;
    return DomSelectorQuery { program(element.document()), element, scope }.matches(element);
}

GC::Ptr<Element const> SelectorQuery::closest(Element const& element) const
{
    return DomSelectorQuery { program(element.document()), element, element }.closest(element);
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors,
// stopping at the first match.
GC::Ptr<Element> SelectorQuery::query_first(ParentNode& root) const
{
    if (!root.first_element_child())
        return nullptr;

    auto& document = root.document();
    if (m_is_result_cacheable) {
        if (auto const* cached_elements = document.query_selector_result_cache().get(root, *this, QuerySelectorResultCache::ResultType::FirstOnly))
            return cached_elements->is_empty() ? nullptr : &cached_elements->first();
    }

    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    GC::Ptr<Element> result;
    if (m_matches_every_element)
        result = root.first_child_of_type<Element>();
    else
        result = DomSelectorQuery { program(document), root, root }.first(root);

    if (m_is_result_cacheable) {
        auto& cache = document.query_selector_result_cache();
        if (result) {
            GC::Ref<Node> node = *result;
            cache.set(root, *this, QuerySelectorResultCache::ResultType::FirstOnly, { &node, 1 });
        } else {
            cache.set(root, *this, QuerySelectorResultCache::ResultType::FirstOnly, {});
        }
    }
    return result;
}

static GC::Ref<NodeList> create_node_list(GC::WeakVector<Element> const& elements)
{
    GC::RootVector<GC::Ref<Node>> nodes;
    nodes.ensure_capacity(elements.size());
    for (auto& element : elements)
        nodes.unchecked_append(element);
    return StaticNodeList::create(move(nodes));
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors.
GC::Ref<NodeList> SelectorQuery::query_all(ParentNode& root) const
{
    if (!root.first_element_child())
        return StaticNodeList::create(ReadonlySpan<GC::Ref<Node>> {});

    auto& document = root.document();
    if (m_is_result_cacheable) {
        if (auto const* cached_elements = document.query_selector_result_cache().get(root, *this, QuerySelectorResultCache::ResultType::All))
            return create_node_list(*cached_elements);
    }

    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    GC::RootVector<GC::Ref<Node>> nodes;
    if (m_matches_every_element) {
        root.for_each_in_subtree_of_type<Element>([&](auto& element) {
            nodes.append(element);
            return TraversalDecision::Continue;
        });
    } else {
        DomSelectorQuery { program(document), root, root }.collect(root, nodes);
    }

    if (m_is_result_cacheable)
        document.query_selector_result_cache().set(root, *this, QuerySelectorResultCache::ResultType::All, nodes);
    return StaticNodeList::create(move(nodes));
}

GC_DEFINE_ALLOCATOR(QuerySelectorResultCache);

QuerySelectorResultCache::QuerySelectorResultCache()
    : GC::WeakContainer(heap())
{
}

GC::WeakVector<Element> const* QuerySelectorResultCache::get(ParentNode const& root, SelectorQuery const& query, ResultType result_type)
{
    auto it = m_entries.find(Key { reinterpret_cast<FlatPtr>(&root), &query });
    if (it == m_entries.end())
        return nullptr;

    auto const& entry = it->value;
    if (entry.root.ptr().ptr() != &root
        || entry.dom_tree_version != root.dom_tree_version()
        || (query.depends_on_character_data() && entry.character_data_version != root.character_data_version())) {
        m_entries.remove(it);
        return nullptr;
    }
    if (result_type == ResultType::All && entry.result_type != ResultType::All)
        return nullptr;

    return &entry.elements;
}

void QuerySelectorResultCache::set(ParentNode const& root, SelectorQuery const& query, ResultType result_type, ReadonlySpan<GC::Ref<Node>> nodes)
{
    static constexpr size_t MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE = 64;

    auto key = Key { reinterpret_cast<FlatPtr>(&root), &query };
    if (!m_entries.contains(key) && m_entries.size() >= MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE)
        m_entries.remove(m_entries.begin());

    Entry entry { root, query, root.dom_tree_version(), root.character_data_version(), result_type, {} };
    entry.elements.ensure_capacity(nodes.size());
    for (auto node : nodes)
        entry.elements.append(static_cast<Element&>(*node));
    m_entries.set(key, move(entry));
}

void QuerySelectorResultCache::remove_dead_cells(Badge<GC::Heap>)
{
    m_entries.remove_all_matching([&](Key const&, Entry& entry) {
        return entry.elements.remove_dead_cells(heap());
    });
}

}
