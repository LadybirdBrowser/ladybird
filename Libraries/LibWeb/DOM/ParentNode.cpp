/*
 * Copyright (c) 2020, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/NodeOperations.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(ParentNode);

// https://dom.spec.whatwg.org/#dom-parentnode-queryselector
WebIDL::ExceptionOr<GC::Ptr<Element>> ParentNode::query_selector(Utf16View selector_text)
{
    // The querySelector(selectors) method steps are to return the first result of running scope-match a selectors string selectors against this,
    // if the result is not an empty list; otherwise null.

    // Scope-match step 1. Let s be the result of parse a selector selectors.
    auto query = document().selector_query_for(selector_text);

    // Scope-match step 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!query)
        return WebIDL::SyntaxError::create(realm(), "Failed to parse selector"_utf16);

    // Scope-match step 3. Return the result of match a selector against a tree with s and node’s root using scoping root node.
    return query->query_first(*this);
}

// https://dom.spec.whatwg.org/#dom-parentnode-queryselectorall
WebIDL::ExceptionOr<GC::Ref<NodeList>> ParentNode::query_selector_all(Utf16View selector_text)
{
    // The querySelectorAll(selectors) method steps are to return the static result of running scope-match a selectors string selectors against this.

    // Scope-match step 1. Let s be the result of parse a selector selectors.
    auto query = document().selector_query_for(selector_text);

    // Scope-match step 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!query)
        return WebIDL::SyntaxError::create(realm(), "Failed to parse selector"_utf16);

    // Scope-match step 3. Return the result of match a selector against a tree with s and node’s root using scoping root node.
    return query->query_all(*this);
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
GC::Ref<HTMLCollection> ParentNode::get_elements_by_tag_name(Utf16FlyString const& qualified_name)
{
    // 1. If qualifiedName is "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches only descendant elements.
    if (qualified_name == u"*"sv) {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const&) {
            return true;
        });
    }

    // 2. Otherwise, if root’s node document is an HTML document, return a HTMLCollection rooted at root, whose filter matches the following descendant elements:
    if (root().document().document_type() == Document::Type::HTML) {
        auto lowercase_qualified_name = qualified_name.to_ascii_lowercase();
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [qualified_name, lowercase_qualified_name = move(lowercase_qualified_name)](Element const& element) {
            // - Whose namespace is the HTML namespace and whose qualified name is qualifiedName, in ASCII lowercase.
            if (element.namespace_uri() == Namespace::HTML)
                return element.qualified_name() == lowercase_qualified_name;

            // - Whose namespace is not the HTML namespace and whose qualified name is qualifiedName.
            return element.qualified_name().view() == qualified_name.view();
        });
    }

    // 3. Otherwise, return a HTMLCollection rooted at root, whose filter matches descendant elements whose qualified name is qualifiedName.
    return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [qualified_name](Element const& element) {
        return element.qualified_name().view() == qualified_name.view();
    });
}

// https://dom.spec.whatwg.org/#concept-getelementsbytagnamens
// NOTE: This method is only exposed on Document and Element, but is in ParentNode to prevent code duplication.
GC::Ref<HTMLCollection> ParentNode::get_elements_by_tag_name_ns(Optional<Utf16FlyString> namespace_, Utf16FlyString const& local_name)
{
    // 1. If namespace is the empty string, set it to null.
    if (namespace_ == Utf16FlyString {})
        namespace_ = OptionalNone {};

    // 2. If both namespace and localName are "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches descendant elements.
    if (namespace_ == u"*"sv && local_name == u"*"sv) {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const&) {
            return true;
        });
    }

    // 3. Otherwise, if namespace is "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches descendant elements whose local name is localName.
    if (namespace_ == u"*"sv) {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [local_name](Element const& element) {
            return element.local_name().view() == local_name.view();
        });
    }

    // 4. Otherwise, if localName is "*" (U+002A), return a HTMLCollection rooted at root, whose filter matches descendant elements whose namespace is namespace.
    if (local_name == u"*"sv) {
        return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [namespace_](Element const& element) {
            auto element_namespace = element.namespace_uri();
            if (element_namespace.has_value() != namespace_.has_value())
                return false;
            return !namespace_.has_value() || element_namespace->view() == namespace_->view();
        });
    }

    // 5. Otherwise, return a HTMLCollection rooted at root, whose filter matches descendant elements whose namespace is namespace and local name is localName.
    return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [namespace_, local_name](Element const& element) {
        auto element_namespace = element.namespace_uri();
        if (element_namespace.has_value() != namespace_.has_value())
            return false;
        return (!namespace_.has_value() || element_namespace->view() == namespace_->view())
            && element.local_name().view() == local_name.view();
    });
}

// https://dom.spec.whatwg.org/#dom-parentnode-prepend
WebIDL::ExceptionOr<void> ParentNode::prepend(ReadonlySpan<Variant<GC::Ref<Node>, Utf16String>> const& nodes)
{
    // 1. Let node be the result of converting nodes into a node given nodes and this’s node document.
    auto node = TRY(convert_nodes_to_single_node(nodes, document()));

    // 2. Pre-insert node into this before this’s first child.
    (void)TRY(pre_insert(node, first_child()));

    return {};
}

// https://dom.spec.whatwg.org/#dom-parentnode-append
WebIDL::ExceptionOr<void> ParentNode::append(ReadonlySpan<Variant<GC::Ref<Node>, Utf16String>> const& nodes)
{
    // 1. Let node be the result of converting nodes into a node given nodes and this’s node document.
    auto node = TRY(convert_nodes_to_single_node(nodes, document()));

    // 2. Append node to this.
    (void)TRY(append_child(node));

    return {};
}

// https://dom.spec.whatwg.org/#dom-parentnode-replacechildren
WebIDL::ExceptionOr<void> ParentNode::replace_children(ReadonlySpan<Variant<GC::Ref<Node>, Utf16String>> const& nodes)
{
    // 1. Let node be the result of converting nodes into a node given nodes and this’s node document.
    auto node = TRY(convert_nodes_to_single_node(nodes, document()));

    // 2. Ensure pre-insert validity given node, this, null, and this’s children.
    TRY(ensure_pre_insert_validity(realm(), node, nullptr, ChildrenToExclude::AllChildren));

    // 3. Replace all with node within this.
    replace_all(*node);
    return {};
}

// https://dom.spec.whatwg.org/#dom-parentnode-movebefore
WebIDL::ExceptionOr<void> ParentNode::move_before(GC::Ref<Node> node, GC::Ptr<Node> child)
{
    // 1. Let referenceChild be child.
    auto reference_child = child;

    // 2. If referenceChild is node, then set referenceChild to node’s next sibling.
    if (reference_child == node)
        reference_child = node->next_sibling();

    // 3. Move node into this before referenceChild.
    TRY(node->move_node(*this, reference_child));

    return {};
}

// https://dom.spec.whatwg.org/#dom-document-getelementsbyclassname
GC::Ref<HTMLCollection> ParentNode::get_elements_by_class_name(Utf16View class_names)
{
    Vector<Utf16String> list_of_class_names;
    auto append_class_name = [&](Utf16View class_name) {
        if (!list_of_class_names.contains_slow(class_name))
            list_of_class_names.append(Utf16String::from_utf16(class_name));
    };

    Optional<size_t> token_start;
    for (size_t i = 0; i < class_names.length_in_code_units(); ++i) {
        if (Infra::is_ascii_whitespace(class_names.code_unit_at(i))) {
            if (token_start.has_value()) {
                append_class_name(class_names.substring_view(*token_start, i - *token_start));
                token_start = {};
            }
            continue;
        }

        if (!token_start.has_value())
            token_start = i;
    }

    if (token_start.has_value())
        append_class_name(class_names.substring_view(*token_start));

    return HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [list_of_class_names = move(list_of_class_names), quirks_mode = document().in_quirks_mode()](Element const& element) {
        for (auto& name : list_of_class_names) {
            if (!element.has_class(name.utf16_view(), quirks_mode ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive))
                return false;
        }
        return !list_of_class_names.is_empty();
    });
}

GC::Ptr<Element> ParentNode::get_element_by_id(Utf16View id) const
{
    if (is_connected()) {
        // For connected document and shadow root we have a cache that allows fast lookup.
        if (is_document()) {
            auto const& document = static_cast<Document const&>(*this);
            return document.element_by_id().get(id, document);
        }
        if (is_shadow_root()) {
            auto const& shadow_root = static_cast<ShadowRoot const&>(*this);
            return shadow_root.element_by_id().get(id, shadow_root);
        }
        // The document element's inclusive subtree contains every element in the document, so the document's cache
        // gives the same answer as walking our subtree.
        if (auto const& document = this->document(); document.document_element() == this)
            return document.element_by_id().get(id, document);
    }

    GC::Ptr<Element> found_element;
    const_cast<ParentNode&>(*this).for_each_in_inclusive_subtree_of_type<Element>([&](Element& element) {
        if (element.id().has_value() && element.id()->view() == id) {
            found_element = &element;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return found_element;
}

}
