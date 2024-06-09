/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/MarkedVector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

class TextTrackList final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(TextTrackList, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(TextTrackList);

public:
    virtual ~TextTrackList() override;

    size_t length() const;

    JS::GCPtr<TextTrack> get_track_by_id(StringView id) const;

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

    void set_onaddtrack(WebIDL::CallbackType*);
    WebIDL::CallbackType* onaddtrack();

    void set_onremovetrack(WebIDL::CallbackType*);
    WebIDL::CallbackType* onremovetrack();

private:
    TextTrackList(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const& property_name) const override;

    Vector<JS::NonnullGCPtr<TextTrack>> m_text_tracks;
};

}
