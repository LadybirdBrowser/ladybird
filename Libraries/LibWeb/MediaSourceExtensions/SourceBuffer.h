/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/TimeRanges.h>
#include <LibGC/Root.h>

namespace Media::FFmpeg {
class MSEDemuxer;
}

namespace Media {
class PlaybackManager;
}

namespace Web::MimeSniff {
class MimeType;
}

namespace Web::WebIDL {
class BufferSource;
}

namespace Web::MediaSourceExtensions {

class MediaSource;

// https://w3c.github.io/media-source/#dom-sourcebuffer
class SourceBuffer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBuffer, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SourceBuffer);

public:
    // Properties
    bool updating() const { return m_updating; }
    GC::Ref<HTML::TimeRanges> buffered() const;

    double timestamp_offset() const { return m_timestamp_offset; }
    void set_timestamp_offset(double offset);

    // Methods
    WebIDL::ExceptionOr<void> append_buffer(GC::Root<WebIDL::BufferSource> const& data);
    WebIDL::ExceptionOr<void> abort();
    WebIDL::ExceptionOr<void> remove(double start, double end);

    // Event handlers
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

    virtual void visit_edges(Cell::Visitor&) override;

protected:
    friend class MediaSource;

    SourceBuffer(JS::Realm&);
    SourceBuffer(JS::Realm&, MediaSource&, MimeSniff::MimeType const&);

    virtual ~SourceBuffer() override;

    virtual void initialize(JS::Realm&) override;

private:
    void schedule_update_end();
    void process_append_buffer();
    void refresh_buffered_ranges();

    GC::Ptr<MediaSource> m_media_source;
    GC::Ptr<HTML::TimeRanges> m_buffered;
    bool m_updating { false };

    // https://w3c.github.io/media-source/#dom-sourcebuffer-timestampoffset
    double m_timestamp_offset { 0.0 };

    // Pending append buffer data
    Vector<ByteBuffer> m_pending_buffers;

    // MSE decoder integration
    RefPtr<Media::FFmpeg::MSEDemuxer> m_demuxer;
    RefPtr<Media::PlaybackManager> m_playback_manager;
    bool m_first_media_segment_appended { false };
};

}
