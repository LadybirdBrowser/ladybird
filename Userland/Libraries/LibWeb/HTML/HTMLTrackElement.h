/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

class HTMLTrackElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTrackElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLTrackElement);

public:
    virtual ~HTMLTrackElement() override;

    WebIDL::UnsignedShort ready_state();

    JS::Handle<TextTrack> track() { return m_track; }

private:
    HTMLTrackElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^DOM::Element
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    JS::GCPtr<TextTrack> m_track;
};

}
