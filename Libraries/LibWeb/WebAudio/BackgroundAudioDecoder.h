/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Media {

struct DecodedAudioData;

}

namespace Web::WebIDL {

class BufferSource;
class CallbackType;

}

namespace Web::DOM {

class Document;

}

namespace Web::WebAudio {

class BaseAudioContext;

class BackgroundAudioDecoder {
public:
    explicit BackgroundAudioDecoder(DOM::Document&);

    void visit_edges(GC::Cell::Visitor&);

    GC::Ref<WebIDL::Promise> decode_audio_data(BaseAudioContext&, GC::Root<WebIDL::BufferSource> const& audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback);

    void settle(u64 request_id, Optional<Media::DecodedAudioData>&&);

private:
    struct PendingRequest {
        u64 request_id { 0 };
        GC::Ref<WebIDL::Promise> promise;
        GC::Ptr<WebIDL::CallbackType> success_callback;
        GC::Ptr<WebIDL::CallbackType> error_callback;
        GC::Weak<BaseAudioContext> context;
        float sample_rate { 0 };
    };

    void queue_a_document_media_element_task(GC::Ref<GC::Function<void()>>);

    GC::Ref<DOM::Document> m_document;
    HTML::UniqueTaskSource m_media_element_event_task_source;
    Vector<PendingRequest> m_pending_requests;
    u64 m_next_request_id { 1 };
};

}
