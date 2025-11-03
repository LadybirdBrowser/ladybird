/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {
class HTMLMediaElement;
}

namespace Web::MediaSourceExtensions {

class SourceBuffer;
class SourceBufferList;

// https://w3c.github.io/media-source/#dom-mediasource
class MediaSource : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaSource, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaSource);

public:
    enum class ReadyState {
        Closed,
        Open,
        Ended
    };

    enum class EndOfStreamError {
        Network,
        Decode
    };

    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<MediaSource>> construct_impl(JS::Realm&);

    // https://w3c.github.io/media-source/#dom-mediasource-canconstructindedicatedworker
    static bool can_construct_in_dedicated_worker(JS::VM&) { return true; }

    // Event handlers
    void set_onsourceopen(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onsourceopen();

    void set_onsourceended(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onsourceended();

    void set_onsourceclose(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onsourceclose();

    // Properties
    Bindings::ReadyState ready_state() const { return static_cast<Bindings::ReadyState>(m_ready_state); }
    WebIDL::ExceptionOr<void> set_duration(double);
    double duration() const { return m_duration; }

    GC::Ptr<SourceBufferList> source_buffers() const { return m_source_buffers; }
    GC::Ptr<SourceBufferList> active_source_buffers() const { return m_active_source_buffers; }

    // Methods
    WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> add_source_buffer(String const& type);
    WebIDL::ExceptionOr<void> remove_source_buffer(SourceBuffer&);
    WebIDL::ExceptionOr<void> end_of_stream(Optional<Bindings::EndOfStreamError> error);
    WebIDL::ExceptionOr<void> set_live_seekable_range(double start, double end);
    WebIDL::ExceptionOr<void> clear_live_seekable_range();

    static bool is_type_supported(JS::VM&, String const&);

    // Internal methods for HTMLMediaElement integration
    void attach_to_media_element(HTML::HTMLMediaElement&);
    void detach_from_media_element();
    HTML::HTMLMediaElement* media_element() const { return m_media_element.ptr(); }

    // Called by SourceBuffer when data is appended
    void source_buffer_data_appended();

    virtual void visit_edges(Cell::Visitor&) override;

protected:
    MediaSource(JS::Realm&);

    virtual ~MediaSource() override;

    virtual void initialize(JS::Realm&) override;

private:
    void set_ready_state(ReadyState);
    bool is_type_supported_internal(String const& type) const;

    ReadyState m_ready_state { ReadyState::Closed };
    double m_duration { NAN };
    GC::Ptr<SourceBufferList> m_source_buffers;
    GC::Ptr<SourceBufferList> m_active_source_buffers;
    GC::Ptr<HTML::HTMLMediaElement> m_media_element;

    // Live seekable range
    bool m_has_live_seekable_range { false };
    double m_live_seekable_range_start { 0.0 };
    double m_live_seekable_range_end { 0.0 };
};

}
