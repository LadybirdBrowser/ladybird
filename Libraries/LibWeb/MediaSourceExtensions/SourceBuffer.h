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

    GC::Ptr<MediaSource> m_media_source;
    GC::Ptr<HTML::TimeRanges> m_buffered;
    bool m_updating { false };

    // Pending append buffer data
    Vector<ByteBuffer> m_pending_buffers;
};

}
