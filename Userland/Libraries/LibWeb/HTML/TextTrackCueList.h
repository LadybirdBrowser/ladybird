/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/MarkedVector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/TextTrackCue.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/media.html#texttrackcuelist
class TextTrackCueList final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(TextTrackCueList, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(TextTrackCueList);

public:
    virtual ~TextTrackCueList() override;

    size_t length() const;

    JS::GCPtr<TextTrackCue> get_cue_by_id(StringView id) const;

private:
    TextTrackCueList(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const& property_name) const override;

    Vector<JS::NonnullGCPtr<TextTrackCue>> m_cues;
};

}
