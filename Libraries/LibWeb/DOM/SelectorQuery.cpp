/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
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
    m_matches_every_element = m_selectors.size() == 1
        && CSS::SelectorFFI::rust_selector_matches_every_element(&m_selectors.first()->rust_selector());

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
    m_program = CSS::SelectorFFI::rust_dom_selector_program_create(selectors.data(), selectors.size(), m_program_is_for_html_document ? Namespace::HTML.raw_identity() : 0);
    return *m_program;
}

namespace {

#include <LibWeb/SelectorStateFactsGenerated.inc>

// Every node crosses to the DOM matcher as a Node pointer, and an element is downcast from one where the matcher asks
// about an element.
Node const& node_from_ffi(void const* node)
{
    return *static_cast<Node const*>(node);
}

Element const& element_from_ffi(void const* element)
{
    return static_cast<Element const&>(node_from_ffi(element));
}

void const* node_to_ffi(Node const* node)
{
    return node;
}

CSS::SelectorFFI::FfiUtf16View utf16_view_to_ffi(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

static_assert(sizeof(Utf16FlyString) == sizeof(uintptr_t));

constexpr CSS::SelectorFFI::FfiDomSelectorCallbacks dom_selector_callbacks {
    .element = [](void const* pointer, uintptr_t const* names, size_t name_count, CSS::SelectorFFI::DomAttribute* attributes, size_t capacity, CSS::SelectorFFI::FfiDomElement* facts) {
        auto const& element = element_from_ffi(pointer);
        auto const& namespace_uri = element.namespace_uri();
        auto const& classes = element.class_names();
        size_t attribute_count = 0;
        if (name_count > 0) {
            ReadonlySpan<uintptr_t> wanted_names { names, name_count };
            for (auto const& attribute : element.attribute_list()) {
                auto local_name = attribute.name.local_name().raw_identity();
                if (!wanted_names.contains_slow(local_name))
                    continue;
                if (attribute_count < capacity) {
                    auto const& attribute_namespace = attribute.name.namespace_();
                    attributes[attribute_count] = {
                        .local_name = local_name,
                        .namespace_uri = attribute_namespace.has_value() && !attribute_namespace->is_empty() ? attribute_namespace->raw_identity() : 0,
                        .value = utf16_view_to_ffi(attribute.value),
                    };
                }
                ++attribute_count;
            }
        }
        *facts = {
            .local_name = element.local_name().raw_identity(),
            .namespace_uri = namespace_uri.has_value() && !namespace_uri->is_empty() ? namespace_uri->raw_identity() : 0,
            .id = element.id().has_value() ? element.id()->raw_identity() : 0,
            .classes = reinterpret_cast<uintptr_t const*>(classes.data()),
            .class_count = classes.size(),
            .attribute_count = attribute_count,
        }; },
    .same_type = [](void const* element, void const* other) {
        auto const& a = element_from_ffi(element);
        auto const& b = element_from_ffi(other);
        return a.local_name() == b.local_name() && a.namespace_uri() == b.namespace_uri(); },
    .parent = [](void const* pointer) -> void const* {
        // The node is an element or a shadow root, and a shadow root has no parent here.
        auto const& node = node_from_ffi(pointer);
        if (!node.is_element())
            return nullptr;
        auto const* parent = node.parent();
        if (!parent || !(parent->is_element() || is<ShadowRoot>(*parent)))
            return nullptr;
        return node_to_ffi(parent);
    },
    .previous_element_sibling = [](void const* element) { return node_to_ffi(element_from_ffi(element).previous_element_sibling()); },
    .next_element_sibling = [](void const* element) { return node_to_ffi(element_from_ffi(element).next_element_sibling()); },
    .first_element_child = [](void const* node) { return node_to_ffi(node_from_ffi(node).first_child_of_type<Element>()); },
    .first_element_sibling = [](void const* pointer) {
        auto const& node = node_from_ffi(pointer);
        auto const* parent = node.parent();
        return node_to_ffi(parent ? parent->first_child_of_type<Element>() : &node); },
    .child_index = [](void const* element, CSS::SelectorFFI::FfiChildIndex which) {
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
    .next_element_in_subtree = [](void const* node, void const* root, uintptr_t local_name) -> void const* {
        auto const& stay_within = node_from_ffi(root);
        for (auto const* next = node_from_ffi(node).next_in_pre_order(&stay_within); next; next = next->next_in_pre_order(&stay_within)) {
            auto const* element = as_if<Element>(*next);
            if (element && (!local_name || element->local_name().raw_identity() == local_name))
                return node_to_ffi(element);
        }
        return nullptr;
    },
    .shadow_root = [](void const* element) { return node_to_ffi(element_from_ffi(element).shadow_root().ptr()); },
    .host = [](void const* shadow_root) { return node_to_ffi(static_cast<ShadowRoot const&>(node_from_ffi(shadow_root)).host()); },
    .id_or_class_equals_ignoring_ascii_case = [](void const* pointer, bool is_class, uintptr_t name_identity) {
        auto const& element = element_from_ffi(pointer);
        auto name = Utf16FlyString::from_raw(name_identity);
        if (!is_class)
            return element.id().has_value() && element.id()->equals_ignoring_ascii_case(name);
        return any_of(element.class_names(), [&](auto const& class_name) { return class_name.equals_ignoring_ascii_case(name); }); },
    .matches_state = [](void const* element, u8 state) { return SelectorMatching::element_matches_state(element_from_ffi(element), pseudo_classes_by_state_fact[state]); },
    .language = [](void const* element) -> CSS::SelectorFFI::FfiUtf16View {
        auto language = element_from_ffi(element).lang_view();
        if (!language.has_value())
            return {};
        return utf16_view_to_ffi(*language);
    },
    .directionality = [](void const* element) -> size_t {
        auto const& directionality = element_from_ffi(element).directionality() == Element::Directionality::Rtl ? "rtl"_utf16_fly_string : "ltr"_utf16_fly_string;
        return directionality.raw_identity(); },
    .heading_level = [](void const* element) -> u32 {
        auto const* heading = as_if<HTML::HTMLHeadingElement>(element_from_ffi(element));
        return heading ? heading->heading_level() : 0;
    },
    .has_custom_state = [](void const* element, uintptr_t state) {
        auto states = element_from_ffi(element).custom_state_set();
        return states && states->has_state(Utf16FlyString::from_raw(state)); },
    .is_empty = [](void const* pointer) {
        // An element is never its own child, so ignoring itself ignores nothing.
        auto const& element = element_from_ffi(pointer);
        return SelectorMatching::element_is_empty_ignoring_child(element, element); },
    .is_document_element = [](void const* element) {
        auto const* parent = element_from_ffi(element).parent();
        return parent && parent->is_document(); },
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
            .scope = node_to_ffi(scope_element ? scope_element : document.document_element()),
            .shadow_root = node_to_ffi(as_if<ShadowRoot>(node.root())),
            .ids_and_classes_ignore_case = document.in_quirks_mode(),
        };
    }

    bool matches(Element const& element) const
    {
        return CSS::SelectorFFI::rust_dom_selector_query_matches(&m_query, node_to_ffi(&element));
    }

    GC::Ptr<Element const> closest(Element const& element) const
    {
        auto const* found = CSS::SelectorFFI::rust_dom_selector_query_closest(&m_query, node_to_ffi(&element));
        return found ? &element_from_ffi(found) : nullptr;
    }

    GC::Ptr<Element> first(ParentNode& root) const
    {
        GC::Ptr<Element> result;
        for_each_in_subtree(root, [&](Element& element) {
            result = element;
            return true;
        });
        return result;
    }

    void collect(ParentNode& root, Vector<GC::RawPtr<Element>>& elements) const
    {
        for_each_in_subtree(root, [&](Element& element) {
            elements.append(element);
            return false;
        });
    }

private:
    template<typename Callback>
    void for_each_in_subtree(ParentNode& root, Callback callback) const
    {
        CSS::SelectorFFI::rust_dom_selector_query_subtree(&m_query, node_to_ffi(&root), &callback, [](void* context, void const* element) {
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
            return cached_elements->is_empty() ? nullptr : cached_elements->first().ptr();
    }

    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    GC::Ptr<Element> result;
    if (m_matches_every_element)
        result = root.first_child_of_type<Element>();
    else
        result = DomSelectorQuery { program(document), root, root }.first(root);

    if (m_is_result_cacheable) {
        Vector<GC::RawPtr<Element>> elements;
        if (result)
            elements.append(*result);
        document.query_selector_result_cache().set(root, *this, QuerySelectorResultCache::ResultType::FirstOnly, move(elements));
    }
    return result;
}

static GC::Ref<NodeList> create_node_list(Vector<GC::RawPtr<Element>> const& elements)
{
    // The elements are descendants of a live query root, which keeps them alive until the list
    // owns them; rooting each of them here would cost a heap registration per element.
    Vector<GC::RawRef<Node>> nodes;
    nodes.ensure_capacity(elements.size());
    for (auto const& element : elements)
        nodes.unchecked_append(static_cast<Node&>(*element));
    return StaticNodeList::create(move(nodes));
}

// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
// This implements step 3, "match a selector against a tree" with the parsed selectors.
GC::Ref<NodeList> SelectorQuery::query_all(ParentNode& root) const
{
    if (!root.first_element_child())
        return create_node_list({});

    auto& document = root.document();
    if (m_is_result_cacheable) {
        if (auto const* cached_elements = document.query_selector_result_cache().get(root, *this, QuerySelectorResultCache::ResultType::All))
            return create_node_list(*cached_elements);
    }

    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    Vector<GC::RawPtr<Element>> elements;
    if (m_matches_every_element) {
        root.for_each_in_subtree_of_type<Element>([&](auto& element) {
            elements.append(element);
            return TraversalDecision::Continue;
        });
    } else {
        DomSelectorQuery { program(document), root, root }.collect(root, elements);
    }

    auto node_list = create_node_list(elements);
    if (m_is_result_cacheable)
        document.query_selector_result_cache().set(root, *this, QuerySelectorResultCache::ResultType::All, move(elements));
    return node_list;
}

Vector<GC::RawPtr<Element>> const* QuerySelectorResultCache::get(ParentNode const& root, SelectorQuery const& query, ResultType result_type)
{
    auto it = m_entries.find(Key { &root, &query });
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

void QuerySelectorResultCache::set(ParentNode const& root, SelectorQuery const& query, ResultType result_type, Vector<GC::RawPtr<Element>> elements)
{
    static constexpr size_t MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE = 64;

    auto key = Key { &root, &query };
    if (!m_entries.contains(key) && m_entries.size() >= MAX_QUERY_SELECTOR_RESULT_CACHE_SIZE)
        m_entries.remove(m_entries.begin());

    m_entries.set(key, Entry { root, query, root.dom_tree_version(), root.character_data_version(), result_type, move(elements) });
}

void QuerySelectorResultCache::visit_edges(GC::Cell::Visitor&)
{
    // Entries intentionally contain only weak or raw GC pointers, so the cache does not keep query roots or matched
    // elements alive.
    (void)m_entries;
}

}
