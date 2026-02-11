/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2024-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Timer.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/AutocompleteElement.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class WEB_API HTMLTextAreaElement final
    : public HTMLElement
    , public FormAssociatedTextControlElement
    , public AutocompleteElement {
    WEB_PLATFORM_OBJECT(HTMLTextAreaElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTextAreaElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLTextAreaElement);
    AUTOCOMPLETE_ELEMENT(HTMLElement, HTMLTextAreaElement);

public:
    virtual ~HTMLTextAreaElement() override;

    virtual void adjust_computed_style(CSS::ComputedProperties&) override;

    String const& type() const
    {
        static String const textarea = "textarea"_string;
        return textarea;
    }

    // ^EventTarget
    // https://html.spec.whatwg.org/multipage/interaction.html#the-tabindex-attribute:the-textarea-element
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // https://html.spec.whatwg.org/multipage/semantics-other.html#concept-element-disabled
    virtual bool is_focusable() const override;

    virtual void did_lose_focus() override;
    virtual void did_receive_focus() override;

    // ^FormAssociatedElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    virtual bool is_submittable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-reset
    virtual bool is_resettable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_autocapitalize_and_autocorrect_inheriting() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const override { return true; }

    virtual void reset_algorithm() override;
    virtual void clear_algorithm() override;

    virtual WebIDL::ExceptionOr<void> cloned(Node&, bool) const override;

    virtual void form_associated_element_was_inserted() override;
    virtual void form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    virtual void children_changed(ChildrenChangedMetadata const*) override;

    // https://www.w3.org/TR/html-aria/#el-textarea
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::textbox; }

    Utf16String default_value() const;
    void set_default_value(Utf16String const&);

    Utf16String value() const override;
    void set_value(Utf16String const&);

    // https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element:concept-fe-api-value-3
    Utf16String api_value() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-relevant-value
    virtual Utf16String relevant_value() const override { return api_value(); }
    virtual WebIDL::ExceptionOr<void> set_relevant_value(Utf16String const& value) override;

    virtual void set_dirty_value_flag(bool flag) override { m_dirty_value = flag; }

    bool user_validity() const { return m_user_validity; }
    void set_user_validity(bool flag) { m_user_validity = flag; }

    u32 text_length() const;

    WebIDL::Long max_length() const;
    WebIDL::ExceptionOr<void> set_max_length(WebIDL::Long);

    WebIDL::Long min_length() const;
    WebIDL::ExceptionOr<void> set_min_length(WebIDL::Long);

    WebIDL::UnsignedLong cols() const;
    void set_cols(unsigned);

    WebIDL::UnsignedLong rows() const;
    void set_rows(WebIDL::UnsignedLong);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionstart
    WebIDL::UnsignedLong selection_start_binding() const;
    WebIDL::ExceptionOr<void> set_selection_start_binding(WebIDL::UnsignedLong const&);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionend
    WebIDL::UnsignedLong selection_end_binding() const;
    WebIDL::ExceptionOr<void> set_selection_end_binding(WebIDL::UnsignedLong const&);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectiondirection
    String selection_direction_binding() const;
    void set_selection_direction_binding(String const& direction);

    void set_dirty_value_flag(Badge<FormAssociatedElement>, bool flag) { m_dirty_value = flag; }

    // ^FormAssociatedTextControlElement
    virtual void did_edit_text_node(FlyString const& input_type, Optional<Utf16String> const& data) override;
    virtual GC::Ptr<DOM::Text> form_associated_element_to_text_node() override { return m_text_node; }
    virtual GC::Ptr<DOM::Element> text_control_scroll_container() override { return this; }

    // https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element%3Asuffering-from-being-missing
    virtual bool suffering_from_being_missing() const override;

    // https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element:concept-fe-mutable
    virtual bool is_mutable() const override;

    GC::Ptr<DOM::Element> placeholder_element() { return m_placeholder_element; }
    GC::Ptr<DOM::Element const> placeholder_element() const { return m_placeholder_element; }

    Optional<String> placeholder_value() const;

private:
    HTMLTextAreaElement(DOM::Document&, DOM::QualifiedName);

    virtual EventResult handle_return_key(FlyString const& ui_input_type) override;

    virtual bool is_html_textarea_element() const final { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    void set_raw_value(Utf16String);

    // ^DOM::Element
    virtual i32 default_tab_index_value() const override;

    void create_shadow_tree_if_needed();

    void handle_maxlength_attribute();

    void queue_firing_input_event();

    void update_placeholder_visibility();

    GC::Ptr<DOM::Element> m_placeholder_element;
    GC::Ptr<DOM::Text> m_placeholder_text_node;

    GC::Ptr<DOM::Element> m_inner_text_element;
    GC::Ptr<DOM::Text> m_text_node;

    RefPtr<Core::Timer> m_input_event_timer;
    FlyString m_pending_input_event_type;
    Optional<Utf16String> m_pending_input_event_data;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fe-dirty
    bool m_dirty_value { false };

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#user-validity
    bool m_user_validity { false };

    // https://html.spec.whatwg.org/multipage/form-elements.html#concept-textarea-raw-value
    Utf16String m_raw_value;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fe-api-value
    mutable Optional<Utf16String> m_api_value;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLTextAreaElement>() const { return is_html_textarea_element(); }

}
