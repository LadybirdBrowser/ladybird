/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/WindowEventHandlers.h>

namespace Web::HTML {

// NOTE: This element is marked as obsolete, but is still listed as required by the specification.
class HTMLFrameSetElement final
    : public HTMLElement
    , public WindowEventHandlers {
    WEB_WRAPPABLE(HTMLFrameSetElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLFrameSetElement);

public:
    virtual ~HTMLFrameSetElement() override;

    virtual CSS::ElementBoxKind box_kind() const override;

    // Gives the style engine what layout and painting need from this element's attributes and children.
    void publish_layout_data() const;
    void layout_data_changed();

private:
    HTMLFrameSetElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_frameset_element() const override { return true; }

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual void children_changed(ChildrenChangedMetadata const&) override;

    void own_layout_data_changed();
    u32 frame_border_thickness() const;

    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<EventTarget> global_event_handlers_to_event_target(Utf16FlyString const& event_name) override;

    // ^HTML::WindowEventHandlers
    virtual GC::Ptr<EventTarget> window_event_handlers_to_event_target() override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLFrameSetElement>() const { return is_html_frameset_element(); }

}
