/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/TokenizedFeatures.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#attr-dir
#define ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTES   \
    __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(ltr) \
    __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(rtl) \
    __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(auto)

class HTMLElement
    : public DOM::Element
    , public HTML::GlobalEventHandlers {
    WEB_PLATFORM_OBJECT(HTMLElement, DOM::Element);
    JS_DECLARE_ALLOCATOR(HTMLElement);

public:
    virtual ~HTMLElement() override;

    Optional<String> title() const { return attribute(HTML::AttributeNames::title); }

    StringView dir() const;
    void set_dir(String const&);

    virtual bool is_editable() const final;
    virtual bool is_focusable() const override;
    bool is_content_editable() const;
    StringView content_editable() const;
    WebIDL::ExceptionOr<void> set_content_editable(StringView);

    String inner_text();
    void set_inner_text(StringView);

    [[nodiscard]] String outer_text();
    WebIDL::ExceptionOr<void> set_outer_text(String);

    int offset_top() const;
    int offset_left() const;
    int offset_width() const;
    int offset_height() const;
    JS::GCPtr<Element> offset_parent() const;

    bool cannot_navigate() const;

    [[nodiscard]] JS::NonnullGCPtr<DOMStringMap> dataset();

    void focus();

    void click();

    void blur();

    [[nodiscard]] String access_key_label() const;

    bool fire_a_synthetic_pointer_event(FlyString const& type, DOM::Element& target, bool not_trusted);

    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const { return false; }

    JS::GCPtr<DOM::NodeList> labels();

    virtual Optional<ARIA::Role> default_role() const override;

    String get_an_elements_target() const;
    TokenizedFeature::NoOpener get_an_elements_noopener(StringView target) const;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<ElementInternals>> attach_internals();

protected:
    HTMLElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& value) override;

    virtual void visit_edges(Cell::Visitor&) override;

private:
    virtual bool is_html_element() const final { return true; }

    // ^HTML::GlobalEventHandlers
    virtual JS::GCPtr<DOM::EventTarget> global_event_handlers_to_event_target(FlyString const&) override { return *this; }
    virtual void did_receive_focus() override;

    [[nodiscard]] String get_the_text_steps();

    JS::GCPtr<DOMStringMap> m_dataset;

    JS::GCPtr<DOM::NodeList> m_labels;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#attached-internals
    JS::GCPtr<ElementInternals> m_attached_internals;

    enum class ContentEditableState {
        True,
        False,
        Inherit,
    };
    ContentEditableState m_content_editable_state { ContentEditableState::Inherit };

    // https://html.spec.whatwg.org/multipage/interaction.html#locked-for-focus
    bool m_locked_for_focus { false };

    // https://html.spec.whatwg.org/multipage/interaction.html#click-in-progress-flag
    bool m_click_in_progress { false };
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLElement>() const { return is_html_element(); }
}
