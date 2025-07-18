/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Node.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class WEB_API ParentNode : public Node {
    WEB_NON_IDL_PLATFORM_OBJECT(ParentNode, Node);
    GC_DECLARE_ALLOCATOR(ParentNode);

public:
    template<typename F>
    void for_each_child(F) const;
    template<typename F>
    void for_each_child(F);

    GC::Ptr<Element> first_element_child();
    GC::Ptr<Element> last_element_child();
    u32 child_element_count() const;

    WebIDL::ExceptionOr<GC::Ptr<Element>> query_selector(StringView);
    WebIDL::ExceptionOr<GC::Ref<NodeList>> query_selector_all(StringView);

    GC::Ref<HTMLCollection> children();

    GC::Ref<HTMLCollection> get_elements_by_tag_name(FlyString const&);
    GC::Ref<HTMLCollection> get_elements_by_tag_name_ns(Optional<FlyString>, FlyString const&);

    WebIDL::ExceptionOr<void> prepend(Vector<Variant<GC::Root<Node>, Utf16String>> const& nodes);
    WebIDL::ExceptionOr<void> append(Vector<Variant<GC::Root<Node>, Utf16String>> const& nodes);
    WebIDL::ExceptionOr<void> replace_children(Vector<Variant<GC::Root<Node>, Utf16String>> const& nodes);
    WebIDL::ExceptionOr<void> move_before(GC::Ref<Node> node, GC::Ptr<Node> child);

    GC::Ref<HTMLCollection> get_elements_by_class_name(StringView);

    GC::Ptr<Element> get_element_by_id(FlyString const& id) const;

protected:
    ParentNode(JS::Realm& realm, Document& document, NodeType type)
        : Node(realm, document, type)
    {
    }

    ParentNode(Document& document, NodeType type)
        : Node(document, type)
    {
    }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ptr<HTMLCollection> m_children;
};

template<>
inline bool Node::fast_is<ParentNode>() const { return is_parent_node(); }

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
