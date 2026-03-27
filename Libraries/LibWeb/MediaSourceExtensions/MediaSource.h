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

    GC::Ref<SourceBufferList> source_buffers();
    GC::Ref<SourceBufferList> active_source_buffers();

    Utf16String next_track_id();

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

    WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> add_source_buffer(String const& type);

    WebIDL::ExceptionOr<void> end_of_stream(Optional<Bindings::EndOfStreamError> const& error = {});
    void run_end_of_stream_algorithm(Badge<SourceBuffer>, Optional<Bindings::EndOfStreamError> const& error) { run_end_of_stream_algorithm(error); }

    // https://w3c.github.io/media-source/#dom-mediasource-duration
    double duration() const;
    WebIDL::ExceptionOr<void> set_duration(double);

    // https://w3c.github.io/media-source/#duration-change-algorithm
    void run_duration_change_algorithm(double new_duration);

    static bool is_type_supported(String const&);
    static bool is_type_supported(JS::VM&, String const& type) { return is_type_supported(type); }

protected:
    MediaSource(JS::Realm&);

    virtual ~MediaSource() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://w3c.github.io/media-source/#end-of-stream-algorithm
    void run_end_of_stream_algorithm(Optional<Bindings::EndOfStreamError> const&);

    Bindings::ReadyState m_ready_state { Bindings::ReadyState::Closed };
    bool m_has_ever_been_attached { false };
    GC::Ptr<HTML::HTMLMediaElement> m_media_element_assigned_to;

    GC::Ref<SourceBufferList> m_source_buffers;
    GC::Ref<SourceBufferList> m_active_source_buffers;

    double m_duration { NAN };

    u64 m_next_track_id = 1;
};

}
