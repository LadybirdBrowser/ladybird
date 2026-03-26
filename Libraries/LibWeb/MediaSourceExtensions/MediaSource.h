/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#dom-mediasource
class MediaSource : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaSource, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaSource);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<MediaSource>> construct_impl(JS::Realm&);

    void queue_a_media_source_task(GC::Ref<GC::Function<void()>>);

    Bindings::ReadyState ready_state() const;
    bool ready_state_is_closed() const;
    void set_has_ever_been_attached();
    void set_ready_state_to_open_and_fire_sourceopen_event();

    // https://w3c.github.io/media-source/#dom-mediasource-canconstructindedicatedworker
    static bool can_construct_in_dedicated_worker(JS::VM&) { return false; }

    void set_assigned_to_media_element(Badge<HTML::HTMLMediaElement>, HTML::HTMLMediaElement&);
    void unassign_from_media_element(Badge<HTML::HTMLMediaElement>);
    GC::Ptr<HTML::HTMLMediaElement> media_element_assigned_to() { return m_media_element_assigned_to; }

    void set_onsourceopen(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onsourceopen();

    void set_onsourceended(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onsourceended();

    void set_onsourceclose(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onsourceclose();

    static bool is_type_supported(JS::VM&, String const&);

protected:
    MediaSource(JS::Realm&);

    virtual ~MediaSource() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    Bindings::ReadyState m_ready_state { Bindings::ReadyState::Closed };
    bool m_has_ever_been_attached { false };
    GC::Ptr<HTML::HTMLMediaElement> m_media_element_assigned_to;
};

}
