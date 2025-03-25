/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ShadowRootPrototype.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/ElementByIdMap.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

class ShadowRoot final : public DocumentFragment {
    WEB_PLATFORM_OBJECT(ShadowRoot, DocumentFragment);
    GC_DECLARE_ALLOCATOR(ShadowRoot);

public:
    Bindings::ShadowRootMode mode() const { return m_mode; }

    Bindings::SlotAssignmentMode slot_assignment() const { return m_slot_assignment; }
    void set_slot_assignment(Bindings::SlotAssignmentMode slot_assignment) { m_slot_assignment = slot_assignment; }

    bool delegates_focus() const { return m_delegates_focus; }
    void set_delegates_focus(bool delegates_focus) { m_delegates_focus = delegates_focus; }

    [[nodiscard]] bool declarative() const { return m_declarative; }
    void set_declarative(bool declarative) { m_declarative = declarative; }

    [[nodiscard]] bool clonable() const { return m_clonable; }
    void set_clonable(bool clonable) { m_clonable = clonable; }

    [[nodiscard]] bool serializable() const { return m_serializable; }
    void set_serializable(bool serializable) { m_serializable = serializable; }

    void set_onslotchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onslotchange();

    bool available_to_element_internals() const { return m_available_to_element_internals; }
    void set_available_to_element_internals(bool available_to_element_internals) { m_available_to_element_internals = available_to_element_internals; }

    // ^EventTarget
    virtual EventTarget* get_parent(Event const&) override;

    WebIDL::ExceptionOr<String> inner_html() const;
    WebIDL::ExceptionOr<void> set_inner_html(StringView);

    WebIDL::ExceptionOr<void> set_html_unsafe(StringView);

    WebIDL::ExceptionOr<String> get_html(GetHTMLOptions const&) const;

    CSS::StyleSheetList& style_sheets();
    CSS::StyleSheetList const& style_sheets() const;

    CSS::StyleSheetList* style_sheets_for_bindings() { return &style_sheets(); }

    GC::Ref<WebIDL::ObservableArray> adopted_style_sheets() const;
    WebIDL::ExceptionOr<void> set_adopted_style_sheets(JS::Value);

    void for_each_css_style_sheet(Function<void(CSS::CSSStyleSheet&)>&& callback) const;

    WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> get_animations();

    ElementByIdMap& element_by_id() const;

    virtual void finalize() override;

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    ShadowRoot(Document&, Element& host, Bindings::ShadowRootMode);
    virtual void initialize(JS::Realm&) override;

    // ^Node
    virtual FlyString node_name() const override { return "#shadow-root"_fly_string; }
    virtual bool is_shadow_root() const final { return true; }

    // NOTE: The specification doesn't seem to specify a default value for mode. Assuming closed for now.
    Bindings::ShadowRootMode m_mode { Bindings::ShadowRootMode::Closed };
    Bindings::SlotAssignmentMode m_slot_assignment { Bindings::SlotAssignmentMode::Named };
    bool m_delegates_focus { false };
    bool m_available_to_element_internals { false };

    // https://dom.spec.whatwg.org/#shadowroot-declarative
    bool m_declarative { false };

    // https://dom.spec.whatwg.org/#shadowroot-clonable
    bool m_clonable { false };

    // https://dom.spec.whatwg.org/#shadowroot-serializable
    bool m_serializable { false };

    mutable OwnPtr<ElementByIdMap> m_element_by_id;

    GC::Ptr<CSS::StyleSheetList> m_style_sheets;
    mutable GC::Ptr<WebIDL::ObservableArray> m_adopted_style_sheets;
};

template<>
inline bool Node::fast_is<ShadowRoot>() const { return node_type() == to_underlying(NodeType::DOCUMENT_FRAGMENT_NODE) && is_shadow_root(); }

// https://dom.spec.whatwg.org/#concept-shadow-including-tree-order
// In shadow-including tree order is shadow-including preorder, depth-first traversal of a node tree.
// Shadow-including preorder, depth-first traversal of a node tree tree is preorder, depth-first traversal
// of tree, with for each shadow host encountered in tree, shadow-including preorder, depth-first traversal
// of that element’s shadow root’s node tree just after it is encountered.

// https://dom.spec.whatwg.org/#concept-shadow-including-descendant
// An object A is a shadow-including descendant of an object B, if A is a descendant of B, or A’s root is a
// shadow root and A’s root’s host is a shadow-including inclusive descendant of B.

// https://dom.spec.whatwg.org/#concept-shadow-including-inclusive-descendant
// A shadow-including inclusive descendant is an object or one of its shadow-including descendants.

template<typename Callback>
inline TraversalDecision Node::for_each_shadow_including_inclusive_descendant(Callback callback)
{
    if (callback(*this) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (is_element()) {
        if (auto shadow_root = static_cast<Element*>(this)->shadow_root()) {
            if (shadow_root->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
    }

    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (child->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

template<typename Callback>
inline TraversalDecision Node::for_each_shadow_including_descendant(Callback callback)
{
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (child->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }
    return TraversalDecision::Continue;
}

}
