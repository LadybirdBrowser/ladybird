/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Node.h>

namespace Web::DOM {

class ParentNode : public Node {
    WEB_PLATFORM_OBJECT(ParentNode, Node);
    JS_DECLARE_ALLOCATOR(ParentNode);

public:
    template<typename F>
    void for_each_child(F) const;
    template<typename F>
    void for_each_child(F);

    JS::GCPtr<Element> first_element_child();
    JS::GCPtr<Element> last_element_child();
    u32 child_element_count() const;

    WebIDL::ExceptionOr<JS::GCPtr<Element>> query_selector(StringView);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<NodeList>> query_selector_all(StringView);

    JS::NonnullGCPtr<HTMLCollection> children();

    JS::NonnullGCPtr<HTMLCollection> get_elements_by_tag_name(FlyString const&);
    JS::NonnullGCPtr<HTMLCollection> get_elements_by_tag_name_ns(Optional<FlyString>, FlyString const&);

    WebIDL::ExceptionOr<void> prepend(Vector<Variant<JS::Handle<Node>, String>> const& nodes);
    WebIDL::ExceptionOr<void> append(Vector<Variant<JS::Handle<Node>, String>> const& nodes);
    WebIDL::ExceptionOr<void> replace_children(Vector<Variant<JS::Handle<Node>, String>> const& nodes);

    JS::NonnullGCPtr<HTMLCollection> get_elements_by_class_name(StringView);

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
    JS::GCPtr<HTMLCollection> m_children;
};

template<>
inline bool Node::fast_is<ParentNode>() const { return is_parent_node(); }

template<typename U>
inline U* Node::shadow_including_first_ancestor_of_type()
{
    for (auto* ancestor = parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (is<U>(*ancestor))
            return &verify_cast<U>(*ancestor);
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
