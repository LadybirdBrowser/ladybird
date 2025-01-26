/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLOptionsCollection.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLSelectElement final
    : public HTMLElement
    , public FormAssociatedElement {
    WEB_PLATFORM_OBJECT(HTMLSelectElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLSelectElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLSelectElement)

public:
    virtual ~HTMLSelectElement() override;

    virtual void adjust_computed_style(CSS::ComputedProperties&) override;

    WebIDL::UnsignedLong size() const;
    WebIDL::ExceptionOr<void> set_size(WebIDL::UnsignedLong);

    GC::Ptr<HTMLOptionsCollection> const& options();

    WebIDL::UnsignedLong length();
    WebIDL::ExceptionOr<void> set_length(WebIDL::UnsignedLong);
    HTMLOptionElement* item(WebIDL::UnsignedLong index);
    HTMLOptionElement* named_item(FlyString const& name);
    WebIDL::ExceptionOr<void> add(HTMLOptionOrOptGroupElement element, Optional<HTMLElementOrElementIndex> before = {});
    void remove();
    void remove(WebIDL::Long);

    GC::Ref<DOM::HTMLCollection> selected_options();

    WebIDL::Long selected_index() const;
    void set_selected_index(WebIDL::Long);

    virtual String value() const override;
    WebIDL::ExceptionOr<void> set_value(String const&);

    bool is_open() const { return m_is_open; }
    void set_is_open(bool);

    WebIDL::ExceptionOr<void> show_picker();

    Vector<GC::Root<HTMLOptionElement>> list_of_options() const;

    // ^EventTarget
    // https://html.spec.whatwg.org/multipage/interaction.html#the-tabindex-attribute:the-select-element
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // https://html.spec.whatwg.org/multipage/semantics-other.html#concept-element-disabled
    virtual bool is_focusable() const override;

    // ^FormAssociatedElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    virtual bool is_submittable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-reset
    virtual bool is_resettable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_auto_capitalize_inheriting() const override { return true; }

    // ^HTMLElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const override { return true; }

    virtual void reset_algorithm() override;

    String const& type() const;

    virtual Optional<ARIA::Role> default_role() const override;

    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;

    virtual void form_associated_element_was_inserted() override;
    virtual void form_associated_element_was_removed(DOM::Node*) override;

    void did_select_item(Optional<u32> const& id);

    void update_selectedness();

    void update_inner_text_element(Badge<HTMLOptionElement>);

private:
    HTMLSelectElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^DOM::Element
    virtual i32 default_tab_index_value() const override;

    virtual void computed_properties_changed() override;

    virtual void children_changed(ChildrenChangedMetadata const*) override;

    void update_cached_list_of_options() const;
    void show_the_picker_if_applicable();

    void create_shadow_tree_if_needed();
    void update_inner_text_element();
    void queue_input_and_change_events();

    u32 display_size() const;

    mutable Vector<GC::Ref<HTMLOptionElement>> m_cached_list_of_options;
    mutable size_t m_cached_number_of_selected_options { 0 };

    GC::Ptr<HTMLOptionsCollection> m_options;
    GC::Ptr<DOM::HTMLCollection> m_selected_options;
    bool m_is_open { false };
    Vector<SelectItem> m_select_items;
    GC::Ptr<DOM::Element> m_inner_text_element;
    GC::Ptr<DOM::Element> m_chevron_icon_element;
};

}
