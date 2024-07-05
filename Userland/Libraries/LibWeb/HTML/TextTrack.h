/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/TextTrackPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class TextTrack final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(TextTrack, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(TextTrack);

public:
    static JS::NonnullGCPtr<TextTrack> create(JS::Realm&);
    virtual ~TextTrack() override;

    Bindings::TextTrackKind kind();
    void set_kind(Bindings::TextTrackKind);

    String label();
    void set_label(String);

    String language();
    void set_language(String);

    String id();
    void set_id(String);

    void set_oncuechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncuechange();

private:
    TextTrack(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    Bindings::TextTrackKind m_kind { Bindings::TextTrackKind::Subtitles };
    String m_label {};
    String m_language {};

    String m_id {};
};

Bindings::TextTrackKind text_track_kind_from_string(String);

}
