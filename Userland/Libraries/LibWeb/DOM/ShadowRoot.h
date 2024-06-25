/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ShadowRootPrototype.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

class ShadowRoot final : public DocumentFragment {
    WEB_PLATFORM_OBJECT(ShadowRoot, DocumentFragment);
    JS_DECLARE_ALLOCATOR(ShadowRoot);

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

    JS::NonnullGCPtr<WebIDL::ObservableArray> adopted_style_sheets() const;
    WebIDL::ExceptionOr<void> set_adopted_style_sheets(JS::Value);

    void for_each_css_style_sheet(Function<void(CSS::CSSStyleSheet&)>&& callback) const;

    Vector<JS::NonnullGCPtr<Animations::Animation>> get_animations();

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

    JS::GCPtr<CSS::StyleSheetList> m_style_sheets;
    mutable JS::GCPtr<WebIDL::ObservableArray> m_adopted_style_sheets;
};

template<>
inline bool Node::fast_is<ShadowRoot>() const { return node_type() == to_underlying(NodeType::DOCUMENT_FRAGMENT_NODE) && is_shadow_root(); }

template<typename Callback>
inline TraversalDecision Node::for_each_shadow_including_inclusive_descendant(Callback callback)
{
    if (callback(*this) == TraversalDecision::Break)
        return TraversalDecision::Break;
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (child->is_element()) {
            if (auto shadow_root = static_cast<Element*>(child)->shadow_root()) {
                if (shadow_root->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
        if (child->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }
    return TraversalDecision::Continue;
}

template<typename Callback>
inline TraversalDecision Node::for_each_shadow_including_descendant(Callback callback)
{
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (child->is_element()) {
            if (JS::GCPtr<ShadowRoot> shadow_root = static_cast<Element*>(child)->shadow_root()) {
                if (shadow_root->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
        if (child->for_each_shadow_including_inclusive_descendant(callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }
    return TraversalDecision::Continue;
}

}
