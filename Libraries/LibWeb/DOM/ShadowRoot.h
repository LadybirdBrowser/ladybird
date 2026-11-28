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
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

struct AdoptedStyleSheetsAccess;

class WEB_API ShadowRoot final : public DocumentFragment {
    WEB_WRAPPABLE(ShadowRoot, DocumentFragment);
    GC_DECLARE_ALLOCATOR(ShadowRoot);

public:
    [[nodiscard]] static GC::Ref<ShadowRoot> create(Document&, Element& host, ShadowRootMode);

    ShadowRootMode mode() const { return m_mode; }

    SlotAssignmentMode slot_assignment() const { return static_cast<SlotAssignmentMode>(m_shadow_root_core.slot_assignment); }
    void set_slot_assignment(SlotAssignmentMode slot_assignment) { m_shadow_root_core.slot_assignment = to_underlying(slot_assignment); }

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

    WebIDL::ExceptionOr<Utf16String> inner_html() const;
    WebIDL::ExceptionOr<void> set_inner_html(Utf16View, GC::Ptr<TrustedTypes::TrustedHTMLParserOptions> = nullptr);

    WebIDL::ExceptionOr<void> set_html(Utf16String const& html, Bindings::SetHTMLOptions const&);
    WebIDL::ExceptionOr<void> append_html(Utf16String const& html, Bindings::SetHTMLOptions const&);
    WebIDL::ExceptionOr<void> prepend_html(Utf16String const& html, Bindings::SetHTMLOptions const&);

    WebIDL::ExceptionOr<Utf16String> get_html(HTMLSerializationOptions const&) const;

    GC::Ptr<Element> active_element();

    Element const* element_from_point(double x, double y);
    GC::RootVector<GC::Ref<Element>> elements_from_point(double x, double y);

    CSS::StyleSheetList& style_sheets();
    CSS::StyleSheetList const& style_sheets() const;

    void for_each_css_style_sheet(Function<void(CSS::StyleSheetState&)>&& callback) const;
    void for_each_active_css_style_sheet(Function<void(CSS::StyleSheetState&)> const& callback) const;

    AnchorNameMap& anchor_name_map() { return m_anchor_name_map; }
    AnchorNameMap const& anchor_name_map() const { return m_anchor_name_map; }

    void assign_slottables_if_needed();
    void did_start_needing_slot_assignment();

    CSS::StyleScope const& style_scope() const { return m_style_scope; }
    CSS::StyleScope& style_scope() { return m_style_scope; }

    using PartElementMap = HashMap<Utf16FlyString, OrderedHashTable<AbstractElement>>;
    PartElementMap const& part_element_map() const;

    GC::Ptr<HTML::CustomElementRegistry> custom_element_registry() const;
    void set_custom_element_registry(GC::Ptr<HTML::CustomElementRegistry> registry) { m_custom_element_registry = registry; }

    bool keep_custom_element_registry_null() const { return m_keep_custom_element_registry_null; }
    void set_keep_custom_element_registry_null(bool value) { m_keep_custom_element_registry_null = value; }

    HTML::RadioButtonGroupRegistry& ensure_radio_button_group_registry();

    // NB: The buffers of the core are freed when the cell is swept, not when it is finalized, as the C++ members that
    //     held them were, so that the finalizer of another dead cell can still read them.
    GC_ALLOW_CELL_DESTRUCTOR virtual ~ShadowRoot() override;
    virtual void finalize() override;

    GC::Ptr<Element> retargeted_fullscreen_element() const;
    GC::Ptr<Element> retargeted_picture_in_picture_element() const;

    // A shadow root is not an element and has no style of its own, but it is a parent in the style
    // tree: it is what a shadow-tree element's relations name, and it is what bounds the region a
    // `:host()` rule reaches. Without an identity of its own, that region has no name and every
    // route across the boundary widens to the document.
    void set_style_node_id(CSS::StyleNodeID);

    // A shadow root is also a style scope, and that is a longer-lived thing than its place in the
    // style tree. The style node identity is minted when the root joins the tree and given up when
    // it leaves; the scope identity has to survive that, because a sheet adopted into the scope is
    // detached with the identity it was attached with. Only moving to another document retires it,
    // since identities belong to one document's engine.
    [[nodiscard]] CSS::TreeScopeID style_engine_tree_scope() const { return m_document_fragment_core.style_engine_tree_scope; }
    void set_style_engine_tree_scope(CSS::TreeScopeID tree_scope) { m_document_fragment_core.style_engine_tree_scope = tree_scope; }

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    friend struct AdoptedStyleSheetsAccess;

    GC::Ref<WebIDL::ObservableArray> adopted_style_sheets() const;

    ShadowRoot(Document&, Element& host, ShadowRootMode);

    // ^Node
    virtual void adopted_from(Document&) override;

    void calculate_part_element_map();

    // NB: The slot assignment and the slots of the shadow tree, see Rust/src/dom/slots.rs.
    RustFFI::ShadowRootCore m_shadow_root_core { .slots = {}, .slots_needing_assignment = {}, .slot_assignment = to_underlying(SlotAssignmentMode::Named), .flags = 0, .ids = {} };

    // NOTE: The specification doesn't seem to specify a default value for mode. Assuming closed for now.
    ShadowRootMode m_mode { ShadowRootMode::Closed };
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

    AnchorNameMap m_anchor_name_map;

    GC::Member<CSS::StyleSheetList> m_style_sheets;
    mutable GC::Member<WebIDL::ObservableArray> m_adopted_style_sheets;

    IntrusiveListNode<ShadowRoot> m_list_node;
    IntrusiveListNode<ShadowRoot> m_slot_assignment_list_node;
    IntrusiveListNode<ShadowRoot> m_counter_style_settle_list_node;

    CSS::StyleScope m_style_scope;

    mutable PartElementMap m_part_element_map;
    mutable Optional<u64> m_dom_tree_version_when_calculated_part_element_map;

    // https://dom.spec.whatwg.org/#shadowroot-custom-element-registry
    GC::Member<HTML::CustomElementRegistry> m_custom_element_registry;

    // https://dom.spec.whatwg.org/#shadowroot-keep-custom-element-registry-null
    bool m_keep_custom_element_registry_null { false };

    GC::Member<HTML::RadioButtonGroupRegistry> m_radio_button_group_registry;

public:
    using DocumentShadowRootList = IntrusiveList<&ShadowRoot::m_list_node>;
    using DocumentSlotAssignmentList = IntrusiveList<&ShadowRoot::m_slot_assignment_list_node>;
    using DocumentCounterStyleSettleList = IntrusiveList<&ShadowRoot::m_counter_style_settle_list_node>;
};

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
