/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/HTMLOrSVGElement.h>
#include <LibWeb/HTML/TokenizedFeatures.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#attr-dir
#define ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTES   \
    __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(ltr) \
    __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(rtl) \
    __ENUMERATE_HTML_ELEMENT_DIR_ATTRIBUTE(auto)

// https://html.spec.whatwg.org/#attr-contenteditable
enum class ContentEditableState {
    True,
    False,
    PlaintextOnly,
    Inherit,
};

struct ShowPopoverOptions {
    GC::Ptr<HTMLElement> source;
};

struct TogglePopoverOptions : public ShowPopoverOptions {
    Optional<bool> force {};
};

using TogglePopoverOptionsOrForceBoolean = Variant<TogglePopoverOptions, bool>;

enum class ThrowExceptions {
    Yes,
    No,
};

enum class FocusPreviousElement {
    Yes,
    No,
};

enum class FireEvents {
    Yes,
    No,
};

enum class ExpectedToBeShowing {
    Yes,
    No,
};

class HTMLElement
    : public DOM::Element
    , public HTML::GlobalEventHandlers
    , public HTML::HTMLOrSVGElement<HTMLElement> {
    WEB_PLATFORM_OBJECT(HTMLElement, DOM::Element);
    GC_DECLARE_ALLOCATOR(HTMLElement);

public:
    virtual ~HTMLElement() override;

    Optional<String> title() const { return attribute(HTML::AttributeNames::title); }

    StringView dir() const;
    void set_dir(String const&);

    virtual bool is_focusable() const override;
    bool is_content_editable() const;
    StringView content_editable() const;
    ContentEditableState content_editable_state() const { return m_content_editable_state; }
    WebIDL::ExceptionOr<void> set_content_editable(StringView);

    String inner_text();
    void set_inner_text(StringView);

    [[nodiscard]] String outer_text();
    WebIDL::ExceptionOr<void> set_outer_text(String const&);

    int offset_top() const;
    int offset_left() const;
    int offset_width() const;
    int offset_height() const;
    GC::Ptr<Element> offset_parent() const;

    bool cannot_navigate() const;

    void click();

    [[nodiscard]] String access_key_label() const;

    bool fire_a_synthetic_pointer_event(FlyString const& type, DOM::Element& target, bool not_trusted);

    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const { return false; }

    GC::Ptr<DOM::NodeList> labels();

    virtual Optional<ARIA::Role> default_role() const override;

    String get_an_elements_target() const;
    TokenizedFeature::NoOpener get_an_elements_noopener(StringView target) const;

    WebIDL::ExceptionOr<GC::Ref<ElementInternals>> attach_internals();

    WebIDL::ExceptionOr<void> set_popover(Optional<String> value);
    Optional<String> popover() const;

    enum class PopoverVisibilityState {
        Hidden,
        Showing,
    };
    PopoverVisibilityState popover_visibility_state() const { return m_popover_visibility_state; }

    WebIDL::ExceptionOr<void> show_popover_for_bindings(ShowPopoverOptions const& = {});
    WebIDL::ExceptionOr<void> hide_popover_for_bindings();
    WebIDL::ExceptionOr<bool> toggle_popover(TogglePopoverOptionsOrForceBoolean const&);

    WebIDL::ExceptionOr<void> show_popover(ThrowExceptions throw_exceptions, GC::Ptr<HTMLElement> invoker);
    WebIDL::ExceptionOr<void> hide_popover(FocusPreviousElement focus_previous_element, FireEvents fire_events, ThrowExceptions throw_exceptions);

    WebIDL::ExceptionOr<bool> check_popover_validity(ExpectedToBeShowing expected_to_be_showing, ThrowExceptions throw_exceptions, GC::Ptr<DOM::Document>);

protected:
    HTMLElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) override;
    virtual void inserted() override;

    virtual void visit_edges(Cell::Visitor&) override;

private:
    virtual bool is_html_element() const final { return true; }

    virtual void adjust_computed_style(CSS::StyleProperties&) override;

    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(FlyString const&) override { return *this; }
    virtual void did_receive_focus() override;
    virtual void did_lose_focus() override;

    [[nodiscard]] String get_the_text_steps();
    GC::Ref<DOM::DocumentFragment> rendered_text_fragment(StringView input);

    GC::Ptr<DOM::NodeList> m_labels;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#attached-internals
    GC::Ptr<ElementInternals> m_attached_internals;

    // https://html.spec.whatwg.org/#attr-contenteditable
    ContentEditableState m_content_editable_state { ContentEditableState::Inherit };

    // https://html.spec.whatwg.org/multipage/interaction.html#click-in-progress-flag
    bool m_click_in_progress { false };

    // Popover API

    // https://html.spec.whatwg.org/multipage/popover.html#popover-visibility-state
    PopoverVisibilityState m_popover_visibility_state { PopoverVisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/popover.html#popover-invoker
    GC::Ptr<HTMLElement> m_popover_invoker;

    // https://html.spec.whatwg.org/multipage/popover.html#popover-showing-or-hiding
    bool m_popover_showing_or_hiding { false };
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLElement>() const { return is_html_element(); }
}
