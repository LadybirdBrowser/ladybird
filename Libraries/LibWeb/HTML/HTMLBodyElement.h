/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/WindowEventHandlers.h>

namespace Web::HTML {

class HTMLBodyElement final
    : public HTMLElement
    , public WindowEventHandlers {
    WEB_PLATFORM_OBJECT(HTMLBodyElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLBodyElement);

public:
    virtual ~HTMLBodyElement() override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    // https://www.w3.org/TR/html-aria/#el-body
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

private:
    HTMLBodyElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Visitor&) override;

    // ^DOM::Node
    virtual bool is_html_body_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;

    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(FlyString const& event_name) override;

    // ^HTML::WindowEventHandlers
    virtual GC::Ptr<DOM::EventTarget> window_event_handlers_to_event_target() override;

    RefPtr<CSS::ImageStyleValue> m_background_style_value;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLBodyElement>() const { return is_html_body_element(); }
}
