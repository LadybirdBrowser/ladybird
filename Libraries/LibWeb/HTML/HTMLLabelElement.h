/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::DOM {

class EventDispatcher;

}

namespace Web::HTML {

class HTMLLabelElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLLabelElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLabelElement);

public:
    virtual ~HTMLLabelElement() override;

    Optional<Utf16String> for_() const { return attribute(HTML::AttributeNames::for_); }

    GC::Ptr<HTMLElement> control() const;
    GC::Ptr<HTMLFormElement> form() const;

    virtual void set_being_activated(bool) override;

    virtual bool is_html_label_element() const override { return true; }

private:
    friend class DOM::EventDispatcher;

    HTMLLabelElement(DOM::Document&, DOM::QualifiedName);

    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;
    bool is_interactive_descendant_event_target(DOM::EventTarget*) const;
    void activation_behavior(DOM::Event const&, bool is_interactive_descendant);

    bool m_click_in_progress { false };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLLabelElement>() const { return is_html_label_element(); }

}
