/*
 * Copyright (c) 2020, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeOperations.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StaticNodeList.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(ParentNode);

static bool contains_named_namespace(const CSS::SelectorList& selectors)
{
    for (auto const& selector : selectors) {
        for (auto const& compound_selector : selector->compound_selectors()) {
            for (auto simple_selector : compound_selector.simple_selectors) {
                if (simple_selector.value.has<CSS::Selector::SimpleSelector::QualifiedName>()) {
                    if (simple_selector.qualified_name().namespace_type == CSS::Selector::SimpleSelector::QualifiedName::NamespaceType::Named)
                        return true;
                }

                if (simple_selector.value.has<CSS::Selector::SimpleSelector::PseudoClassSelector>()) {
                    if (contains_named_namespace(simple_selector.pseudo_class().argument_selector_list))
                        return true;
                }
            }
        }
    }
    return false;
}

enum class ReturnMatches {
    First,
    All,
};
// https://dom.spec.whatwg.org/#scope-match-a-selectors-string
static WebIDL::ExceptionOr<Variant<GC::Ptr<Element>, GC::Ref<NodeList>>> scope_match_a_selectors_string(ParentNode& node, StringView selector_text, ReturnMatches return_matches)
{
    // To scope-match a selectors string selectors against a node, run these steps:
    // 1. Let s be the result of parse a selector selectors.
    auto maybe_selectors = parse_selector(CSS::Parser::ParsingParams { node.document() }, selector_text);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!maybe_selectors.has_value())
        return WebIDL::SyntaxError::create(node.realm(), "Failed to parse selector"_string);

    auto selectors = maybe_selectors.value();

    // "Note: Support for namespaces within selectors is not planned and will not be added."
    if (contains_named_namespace(selectors))
        return WebIDL::SyntaxError::create(node.realm(), "Failed to parse selector"_string);

    // 3. Return the result of match a selector against a tree with s and node’s root using scoping root node.
    GC::Ptr<Element> single_result;
    Vector<GC::Root<Node>> results;
    // FIXME: This should be shadow-including. https://drafts.csswg.org/selectors-4/#match-a-selector-against-a-tree
    node.for_each_in_subtree_of_type<Element>([&](auto& element) {
        for (auto& selector : selectors) {
            SelectorEngine::MatchContext context;
            if (SelectorEngine::matches(selector, element, nullptr, context, {}, node)) {
                if (return_matches == ReturnMatches::First) {
                    single_result = &element;
                    return TraversalDecision::Break;
                }
                results.append(element);
                break;
            }
        }
        return TraversalDecision::Continue;
    });

    if (return_matches == ReturnMatches::First)
        return { single_result };

    return { StaticNodeList::create(node.realm(), move(results)) };
}

// https://dom.spec.whatwg.org/#dom-parentnode-queryselector
WebIDL::ExceptionOr<GC::Ptr<Element>> ParentNode::query_selector(StringView selector_text)
{
    // The querySelector(selectors) method steps are to return the first result of running scope-match a selectors string selectors against this,
    // if the result is not an empty list; otherwise null.
    return TRY(scope_match_a_selectors_string(*this, selector_text, ReturnMatches::First)).get<GC::Ptr<Element>>();
}

// https://dom.spec.whatwg.org/#dom-parentnode-queryselectorall
WebIDL::ExceptionOr<GC::Ref<NodeList>> ParentNode::query_selector_all(StringView selector_text)
{
    // The querySelectorAll(selectors) method steps are to return the static result of running scope-match a selectors string selectors against this.
    return TRY(scope_match_a_selectors_string(*this, selector_text, ReturnMatches::All)).get<GC::Ref<NodeList>>();
}

GC::Ptr<Element> ParentNode::first_element_child()
{
    return first_child_of_type<Element>();
}

GC::Ptr<Element> ParentNode::last_element_child()
{
    return last_child_of_type<Element>();
}

// https://dom.spec.whatwg.org/#dom-parentnode-childelementcount
u32 ParentNode::child_element_count() const
{
    u32 count = 0;
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (is<Element>(child))
            ++count;
    }
    return count;
}

void ParentNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_children);
}

// https://dom.spec.whatwg.org/#dom-parentnode-children
GC::Ref<HTMLCollection> ParentNode::children()
{
    // The children getter steps are to return an HTMLCollection collection rooted at this matching only element children.
    if (!m_children) {
        m_children = HTMLCollection::create(*this, HTMLCollection::Scope::Children, [](Element const&) {
            return true;
        });
    }
    return *m_children;
}

// https://dom.spec.whatwg.org/#concept-getelementsbytagname
// NOTE: This method is only exposed on Document and Element, but is in ParentNode to prevent code duplication.
GC::Ref<HTMLCollection> ParentNode::get_elements_by_tag_name(FlyString const& qualified_name)
{
    // 1. If qualifiedName is "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches only descendant elements.
    if (qualified_name == "*") {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const&) {
            return true;
        });
    }

    // 2. Otherwise, if root’s node document is an HTML document, return a HTMLCollection rooted at root, whose filter matches the following descendant elements:
    if (root().document().document_type() == Document::Type::HTML) {
        FlyString qualified_name_in_ascii_lowercase = qualified_name.to_ascii_lowercase();
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [qualified_name, qualified_name_in_ascii_lowercase](Element const& element) {
            // - Whose namespace is the HTML namespace and whose qualified name is qualifiedName, in ASCII lowercase.
            if (element.namespace_uri() == Namespace::HTML)
                return element.qualified_name() == qualified_name_in_ascii_lowercase;

            // - Whose namespace is not the HTML namespace and whose qualified name is qualifiedName.
            return element.qualified_name() == qualified_name;
        });
    }

    // 3. Otherwise, return a HTMLCollection rooted at root, whose filter matches descendant elements whose qualified name is qualifiedName.
    return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [qualified_name](Element const& element) {
        return element.qualified_name() == qualified_name;
    });
}

// https://dom.spec.whatwg.org/#concept-getelementsbytagnamens
// NOTE: This method is only exposed on Document and Element, but is in ParentNode to prevent code duplication.
GC::Ref<HTMLCollection> ParentNode::get_elements_by_tag_name_ns(Optional<FlyString> namespace_, FlyString const& local_name)
{
    // 1. If namespace is the empty string, set it to null.
    if (namespace_ == FlyString {})
        namespace_ = OptionalNone {};

    // 2. If both namespace and localName are "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches descendant elements.
    if (namespace_ == "*" && local_name == "*") {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const&) {
            return true;
        });
    }

    // 3. Otherwise, if namespace is "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches descendant elements whose local name is localName.
    if (namespace_ == "*") {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [local_name](Element const& element) {
            return element.local_name() == local_name;
        });
    }

    // 4. Otherwise, if localName is "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches descendant elements whose namespace is namespace.
    if (local_name == "*") {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [namespace_](Element const& element) {
            return element.namespace_uri() == namespace_;
        });
    }

    // 5. Otherwise, return a HTMLCollection rooted at root, whose filter matches descendant elements whose namespace is namespace and local name is localName.
    return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [namespace_, local_name](Element const& element) {
        return element.namespace_uri() == namespace_ && element.local_name() == local_name;
    });
}

// https://dom.spec.whatwg.org/#dom-parentnode-prepend
WebIDL::ExceptionOr<void> ParentNode::prepend(Vector<Variant<GC::Root<Node>, String>> const& nodes)
{
    // 1. Let node be the result of converting nodes into a node given nodes and this’s node document.
    auto node = TRY(convert_nodes_to_single_node(nodes, document()));

    // 2. Pre-insert node into this before this’s first child.
    (void)TRY(pre_insert(node, first_child()));

    return {};
}

WebIDL::ExceptionOr<void> ParentNode::append(Vector<Variant<GC::Root<Node>, String>> const& nodes)
{
    // 1. Let node be the result of converting nodes into a node given nodes and this’s node document.
    auto node = TRY(convert_nodes_to_single_node(nodes, document()));

    // 2. Append node to this.
    (void)TRY(append_child(node));

    return {};
}

WebIDL::ExceptionOr<void> ParentNode::replace_children(Vector<Variant<GC::Root<Node>, String>> const& nodes)
{
    // 1. Let node be the result of converting nodes into a node given nodes and this’s node document.
    auto node = TRY(convert_nodes_to_single_node(nodes, document()));

    // 2. Ensure pre-insertion validity of node into this before null.
    TRY(ensure_pre_insertion_validity(node, nullptr));

    // 3. Replace all with node within this.
    replace_all(*node);
    return {};
}

// https://dom.spec.whatwg.org/#dom-document-getelementsbyclassname
GC::Ref<HTMLCollection> ParentNode::get_elements_by_class_name(StringView class_names)
{
    Vector<FlyString> list_of_class_names;
    for (auto& name : class_names.split_view_if(Infra::is_ascii_whitespace)) {
        list_of_class_names.append(FlyString::from_utf8(name).release_value_but_fixme_should_propagate_errors());
    }
    return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [list_of_class_names = move(list_of_class_names), quirks_mode = document().in_quirks_mode()](Element const& element) {
        for (auto& name : list_of_class_names) {
            if (!element.has_class(name, quirks_mode ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive))
                return false;
        }
        return !list_of_class_names.is_empty();
    });
}

GC::Ptr<Element> ParentNode::get_element_by_id(FlyString const& id) const
{
    // For document and shadow root we have a cache that allows fast lookup.
    if (is_document()) {
        auto const& document = static_cast<Document const&>(*this);
        return document.element_by_id().get(id);
    }
    if (is_shadow_root()) {
        auto const& shadow_root = static_cast<ShadowRoot const&>(*this);
        return shadow_root.element_by_id().get(id);
    }

    GC::Ptr<Element> found_element;
    const_cast<ParentNode&>(*this).for_each_in_inclusive_subtree_of_type<Element>([&](Element& element) {
        if (element.id() == id) {
            found_element = &element;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return found_element;
}

}
