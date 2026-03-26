/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::MediaSourceExtensions {

class SourceBufferProcessor;

// https://w3c.github.io/media-source/#dom-sourcebuffer
class SourceBuffer : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SourceBuffer, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SourceBuffer);

public:
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

    // https://w3c.github.io/media-source/#dom-sourcebuffer-updating
    bool updating() const;

    // https://w3c.github.io/media-source/#addsourcebuffer-method
    WebIDL::ExceptionOr<void> append_buffer(GC::Root<WebIDL::BufferSource> const&);

protected:
    SourceBuffer(JS::Realm&, MediaSource&);

    virtual ~SourceBuffer() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    WebIDL::ExceptionOr<void> prepare_append();
    void run_buffer_append_algorithm();
    void run_append_error_algorithm();

    GC::Ref<MediaSource> m_media_source;
    NonnullRefPtr<SourceBufferProcessor> m_processor;
};

}
