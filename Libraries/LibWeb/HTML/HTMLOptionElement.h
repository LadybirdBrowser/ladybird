/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLOptionElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLOptionElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLOptionElement);

public:
    virtual ~HTMLOptionElement() override;

    bool selected() const { return m_selected; }
    void set_selected(bool);
    void set_selected_internal(bool);
    [[nodiscard]] u64 selectedness_update_index() const { return m_selectedness_update_index; }

    String value() const;
    WebIDL::ExceptionOr<void> set_value(String const&);

    String text() const;
    void set_text(String const&);

    [[nodiscard]] String label() const;
    void set_label(String const&);

    int index() const;

    bool disabled() const;

    GC::Ptr<HTML::HTMLFormElement const> form() const;

    virtual Optional<ARIA::Role> default_role() const override;

    GC::Ptr<HTMLSelectElement> owner_select_element();
    GC::Ptr<HTMLSelectElement const> owner_select_element() const { return const_cast<HTMLOptionElement&>(*this).owner_select_element(); }

private:
    friend class Bindings::OptionConstructor;
    friend class HTMLSelectElement;

    HTMLOptionElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;
    virtual void children_changed() override;

    void ask_for_a_reset();
    void update_selection_label();

    // https://html.spec.whatwg.org/multipage/form-elements.html#concept-option-selectedness
    bool m_selected { false };

    // https://html.spec.whatwg.org/multipage/form-elements.html#concept-option-dirtiness
    bool m_dirty { false };

    u64 m_selectedness_update_index { 0 };
};

}
