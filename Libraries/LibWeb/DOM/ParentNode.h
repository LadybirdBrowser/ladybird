/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Node.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class RootedNodeList;

class WEB_API ParentNode : public Node {
    WEB_NON_IDL_WRAPPABLE(ParentNode, Node);
    GC_DECLARE_ALLOCATOR(ParentNode);

public:
    template<typename F>
    void for_each_child(F) const;
    template<typename F>
    void for_each_child(F);

    GC::Ptr<Element> first_element_child();
    GC::Ptr<Element> last_element_child();
    u32 child_element_count() const;

    WebIDL::ExceptionOr<GC::Ptr<Element>> query_selector(Utf16String const&);
    WebIDL::ExceptionOr<GC::Ref<NodeList>> query_selector_all(Utf16String const&);

    GC::Ref<HTMLCollection> children();

    GC::Ref<HTMLCollection> get_elements_by_tag_name(Utf16FlyString const&);
    GC::Ref<HTMLCollection> get_elements_by_tag_name_ns(Optional<Utf16FlyString>, Utf16FlyString const&);

    WebIDL::ExceptionOr<void> prepend(ReadonlySpan<Variant<GC::Ref<Node>, Utf16String>> const& nodes);
    WebIDL::ExceptionOr<void> append(ReadonlySpan<Variant<GC::Ref<Node>, Utf16String>> const& nodes);
    WebIDL::ExceptionOr<void> replace_children(ReadonlySpan<Variant<GC::Ref<Node>, Utf16String>> const& nodes);
    WebIDL::ExceptionOr<void> move_before(GC::Ref<Node> node, GC::Ptr<Node> child);

    GC::Ref<HTMLCollection> get_elements_by_class_name(Utf16View);

    GC::Ptr<Element> get_element_by_id(Utf16View id) const;

protected:
    ParentNode(Document& document, NodeType type);

private:
    friend class Node;

    GC::Ref<HTMLCollection> create_elements_by_tag_name_collection(Utf16FlyString const&);

    // The counters behind Node::dom_tree_version() and Node::character_data_version(), for the tree rooted here, whose
    // layout libweb_rust defines (see Rust/src/dom/tree_version.rs), which reads and moves them in place.
    RustFFI::ParentNodeCore m_parent_node_core {};
};

enum class InTreeOrder : u8 {
    No,
    Yes,
};

// Appends the elements of the connected tree whose root is scope_root, a document or a shadow root, whose ID is id to
// result, which libweb_rust looks up in the tree's map of elements by ID.
void elements_with_id(Node const& scope_root, Utf16View id, InTreeOrder, RootedNodeList& result);

template<typename U>
inline U* Node::first_flat_tree_ancestor_of_type()
{
    for (auto* ancestor = flat_tree_parent(); ancestor; ancestor = ancestor->flat_tree_parent()) {
        if (is<U>(*ancestor))
            return &as<U>(*ancestor);
    }
    return nullptr;
}

template<typename Callback>
inline void ParentNode::for_each_child(Callback callback) const
{
    for (auto* node = first_child(); node; node = node->next_sibling()) {
        if (callback(*node) == IterationDecision::Break)
            return;
    }
}

template<typename Callback>
inline void ParentNode::for_each_child(Callback callback)
{
    for (auto* node = first_child(); node; node = node->next_sibling()) {
        if (callback(*node) == IterationDecision::Break)
            return;
    }
}

}
