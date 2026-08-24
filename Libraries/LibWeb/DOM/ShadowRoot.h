/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ShadowRoot.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/AnchorNameMap.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/ElementByIdMap.h>
#include <LibWeb/DOM/SlotRegistry.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

struct AdoptedStyleSheetsAccess;

class WEB_API ShadowRoot final : public DocumentFragment {
    WEB_WRAPPABLE(ShadowRoot, DocumentFragment);
    GC_DECLARE_ALLOCATOR(ShadowRoot);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<ShadowRoot> create(Document&, Element& host, ShadowRootMode);

    ShadowRootMode mode() const { return m_mode; }

    SlotAssignmentMode slot_assignment() const { return m_slot_assignment; }
    void set_slot_assignment(SlotAssignmentMode slot_assignment) { m_slot_assignment = slot_assignment; }

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

    [[nodiscard]] bool is_user_agent_internal() const { return m_user_agent_internal; }
    void set_user_agent_internal(bool user_agent_internal) { m_user_agent_internal = user_agent_internal; }

    [[nodiscard]] bool uses_document_style_sheets() const { return m_uses_document_style_sheets; }
    void set_uses_document_style_sheets(bool value) { m_uses_document_style_sheets = value; }

    // ^EventTarget
    virtual EventTarget* get_parent(Event const&) override;

    WebIDL::ExceptionOr<Utf16String> inner_html() const;
    WebIDL::ExceptionOr<void> set_inner_html(StringView);

    WebIDL::ExceptionOr<void> set_html_unsafe(StringView);

    WebIDL::ExceptionOr<Utf16String> get_html(HTMLSerializationOptions const&) const;

    GC::Ptr<Element> active_element();

    CSS::StyleSheetList& style_sheets();
    CSS::StyleSheetList const& style_sheets() const;

    void for_each_css_style_sheet(Function<void(CSS::CSSStyleSheet&)>&& callback) const;
    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const;

    WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> get_animations();

    ElementByIdMap& element_by_id() const;

    AnchorNameMap& anchor_name_map() { return m_anchor_name_map; }
    AnchorNameMap const& anchor_name_map() const { return m_anchor_name_map; }

    void register_slot(HTML::HTMLSlotElement&);
    void unregister_slot(HTML::HTMLSlotElement&);

    template<typename Callback>
    void for_each_registered_slot(Callback callback)
    {
        if (m_slot_registry)
            m_slot_registry->for_each_slot(callback);
    }

    GC::Ptr<HTML::HTMLSlotElement> first_slot_with_name(Utf16View name) const;

    CSS::StyleScope const& style_scope() const { return m_style_scope; }
    CSS::StyleScope& style_scope() { return m_style_scope; }

    using PartElementMap = HashMap<Utf16FlyString, OrderedHashTable<AbstractElement>>;
    PartElementMap const& part_element_map() const;

    GC::Ptr<HTML::CustomElementRegistry> custom_element_registry() const;
    void set_custom_element_registry(GC::Ptr<HTML::CustomElementRegistry> registry) { m_custom_element_registry = registry; }

    bool keep_custom_element_registry_null() const { return m_keep_custom_element_registry_null; }
    void set_keep_custom_element_registry_null(bool value) { m_keep_custom_element_registry_null = value; }

    HTML::RadioButtonGroupRegistry& ensure_radio_button_group_registry();

    virtual void finalize() override;

    GC::Ptr<Element> retargeted_fullscreen_element() const;

    // A shadow root is not an element and has no style of its own, but it is a parent in the style
    // tree: it is what a shadow-tree element's relations name, and it is what bounds the region a
    // `:host()` rule reaches. Without an identity of its own, that region has no name and every
    // route across the boundary widens to the document.
    [[nodiscard]] CSS::StyleNodeID style_node_id() const { return m_style_node_id; }
    void set_style_node_id(CSS::StyleNodeID style_node_id) { m_style_node_id = style_node_id; }

    // A shadow root is also a style scope, and that is a longer-lived thing than its place in the
    // style tree. The style node identity is minted when the root joins the tree and given up when
    // it leaves; the scope identity has to survive that, because a sheet adopted into the scope is
    // detached with the identity it was attached with. Only moving to another document retires it,
    // since identities belong to one document's engine.
    [[nodiscard]] CSS::TreeScopeID style_engine_tree_scope() const { return m_style_engine_tree_scope; }
    void set_style_engine_tree_scope(CSS::TreeScopeID tree_scope) { m_style_engine_tree_scope = tree_scope; }

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    friend struct AdoptedStyleSheetsAccess;

    GC::Ref<WebIDL::ObservableArray> adopted_style_sheets() const;

    ShadowRoot(Document&, Element& host, ShadowRootMode);

    // ^Node
    virtual Utf16FlyString node_name() const override { return "#shadow-root"_utf16_fly_string; }
    virtual bool is_shadow_root() const final { return true; }
    virtual void adopted_from(Document&) override;

    void calculate_part_element_map();

    CSS::StyleNodeID m_style_node_id;
    CSS::TreeScopeID m_style_engine_tree_scope;

    // NOTE: The specification doesn't seem to specify a default value for mode. Assuming closed for now.
    ShadowRootMode m_mode { ShadowRootMode::Closed };
    SlotAssignmentMode m_slot_assignment { SlotAssignmentMode::Named };
    bool m_delegates_focus { false };
    bool m_available_to_element_internals { false };
    bool m_user_agent_internal { false };
    bool m_uses_document_style_sheets { false };

    // https://dom.spec.whatwg.org/#shadowroot-declarative
    bool m_declarative { false };

    // https://dom.spec.whatwg.org/#shadowroot-clonable
    bool m_clonable { false };

    // https://dom.spec.whatwg.org/#shadowroot-serializable
    bool m_serializable { false };

    mutable OwnPtr<ElementByIdMap> m_element_by_id;

    AnchorNameMap m_anchor_name_map;

    OwnPtr<SlotRegistry> m_slot_registry;

    GC::Ptr<CSS::StyleSheetList> m_style_sheets;
    mutable GC::Ptr<WebIDL::ObservableArray> m_adopted_style_sheets;

    IntrusiveListNode<ShadowRoot> m_list_node;

    CSS::StyleScope m_style_scope;

    mutable PartElementMap m_part_element_map;
    mutable u64 m_dom_tree_version_when_calculated_part_element_map { 0 };

    // https://dom.spec.whatwg.org/#shadowroot-custom-element-registry
    GC::Ptr<HTML::CustomElementRegistry> m_custom_element_registry;

    // https://dom.spec.whatwg.org/#shadowroot-keep-custom-element-registry-null
    bool m_keep_custom_element_registry_null { false };

    GC::Ptr<HTML::RadioButtonGroupRegistry> m_radio_button_group_registry;

public:
    using DocumentShadowRootList = IntrusiveList<&ShadowRoot::m_list_node>;
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
    auto decision = callback(*this);
    if (decision == TraversalDecision::Break)
        return TraversalDecision::Break;
    if (decision == TraversalDecision::SkipChildrenAndContinue)
        return TraversalDecision::Continue;

    if (this->for_each_shadow_including_descendant(callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    return TraversalDecision::Continue;
}

template<typename Callback>
inline TraversalDecision Node::for_each_shadow_including_descendant(Callback callback)
{
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

}
