/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Types.h>
#include <LibWeb/Bindings/SourceBuffer.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::MediaSourceExtensions {

using AppendMode = Bindings::AppendMode;

class SourceBufferProcessor;
struct InitializationSegmentData;

// https://w3c.github.io/media-source/#dom-sourcebuffer
class SourceBuffer : public DOM::EventTarget {
    WEB_WRAPPABLE(SourceBuffer, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SourceBuffer);

public:
    static GC::Ref<SourceBuffer> create(MediaSource&, GC::Ref<HTML::AudioTrackList>, GC::Ref<HTML::VideoTrackList>, GC::Ref<HTML::TextTrackList>);

    void set_onupdatestart(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onupdatestart();

    void set_onupdate(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onupdate();

    void set_onupdateend(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onupdateend();

    void set_onerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onerror();

    void set_onabort(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onabort();

    // https://w3c.github.io/media-source/#dom-sourcebuffer-mode
    AppendMode mode() const;
    WebIDL::ExceptionOr<void> set_mode(AppendMode);

    // https://w3c.github.io/media-source/#dom-sourcebuffer-updating
    bool updating() const;

    // https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
    GC::Ref<HTML::TimeRanges> buffered();

    void set_content_type(Utf16View type);

    // https://w3c.github.io/media-source/#addsourcebuffer-method
    WebIDL::ExceptionOr<void> append_buffer(WebIDL::BufferSourceVariant const&);

    // https://w3c.github.io/media-source/#dom-sourcebuffer-abort
    WebIDL::ExceptionOr<void> abort();

    // https://w3c.github.io/media-source/#dom-sourcebuffer-changetype
    WebIDL::ExceptionOr<void> change_type(Utf16String const& type);

    void set_reached_end_of_stream(Badge<MediaSource>);
    void clear_reached_end_of_stream(Badge<MediaSource>);

    void abort_if_updating(Badge<MediaSource>);

protected:
    SourceBuffer(MediaSource&, GC::Ref<HTML::AudioTrackList>, GC::Ref<HTML::VideoTrackList>, GC::Ref<HTML::TextTrackList>);

    virtual ~SourceBuffer() override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    WebIDL::ExceptionOr<void> prepare_append(size_t new_data_size);
    void run_buffer_append_algorithm(u64 append_generation);
    void abort_buffer_append_algorithm();
    void run_append_error_algorithm();
    void on_first_initialization_segment_processed(InitializationSegmentData const&);
    void update_ready_state_and_duration_after_coded_frame_processing();
    void finish_buffer_append();

    GC::Ref<MediaSource> m_media_source;
    NonnullRefPtr<SourceBufferProcessor> m_processor;

    // NB: The generation of the current buffer-append run — captured by the run when it starts. Bumped by
    //     abort_buffer_append_algorithm(). A run whose captured generation no longer matches does nothing further.
    u64 m_append_generation { 0 };

    // https://w3c.github.io/media-source/#dom-sourcebuffer-audiotracks
    GC::Ref<HTML::AudioTrackList> m_audio_tracks;

    // https://w3c.github.io/media-source/#dom-sourcebuffer-videotracks
    GC::Ref<HTML::VideoTrackList> m_video_tracks;

    // https://w3c.github.io/media-source/#dom-sourcebuffer-texttracks
    GC::Ref<HTML::TextTrackList> m_text_tracks;
};

}
