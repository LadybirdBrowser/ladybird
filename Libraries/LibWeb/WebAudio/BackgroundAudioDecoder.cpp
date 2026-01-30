/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/NumericLimits.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibMedia/Audio/AudioDecoding.h>
#include <LibThreading/BackgroundAction.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/BackgroundAudioDecoder.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

using DecodeResult = Optional<Media::DecodedAudioData>;

BackgroundAudioDecoder::BackgroundAudioDecoder(DOM::Document& document)
    : m_document(document)
{
}

void BackgroundAudioDecoder::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
    for (auto& request : m_pending_requests) {
        visitor.visit(request.promise);
        visitor.visit(request.success_callback);
        visitor.visit(request.error_callback);
    }
}

void BackgroundAudioDecoder::queue_a_document_media_element_task(GC::Ref<GC::Function<void()>> steps)
{
    auto task = HTML::Task::create(m_document->vm(), m_media_element_event_task_source.source, m_document, steps);
    HTML::main_thread_event_loop().task_queue().add(task);
}

static void decode_audio_data_async(GC::Weak<DOM::Document> document, u64 request_id, ByteBuffer encoded_audio_data, Optional<u32> target_sample_rate)
{
    using Action = Threading::BackgroundAction<DecodeResult>;

    auto decode_work = AK::Function<ErrorOr<DecodeResult>(Action&)>(
        [encoded_audio_data = move(encoded_audio_data), target_sample_rate](Action& action) mutable -> ErrorOr<DecodeResult> {
            if (encoded_audio_data.is_empty())
                return DecodeResult {};

            auto decoded_or_error = Media::decode_first_audio_track_to_pcm_f32(move(encoded_audio_data), target_sample_rate, [&action] {
                return action.is_canceled();
            });
            if (decoded_or_error.is_error())
                return DecodeResult {};

            return decoded_or_error.release_value();
        });

    auto on_decode_complete = AK::Function<ErrorOr<void>(DecodeResult)>(
        [document = move(document), request_id](DecodeResult decoded_audio_data) mutable -> ErrorOr<void> {
            if (auto document_ptr = document.ptr(); document_ptr)
                document_ptr->background_audio_decoder().settle(request_id, move(decoded_audio_data));
            return {};
        });

    (void)Threading::BackgroundAction<DecodeResult>::construct(move(decode_work), move(on_decode_complete));
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
// Web Audio API: BaseAudioContext.decodeAudioData(audioData, successCallback, errorCallback)
//
// (A) When decodeAudioData is called, the following steps MUST be performed on the control thread
GC::Ref<WebIDL::Promise> BackgroundAudioDecoder::decode_audio_data(BaseAudioContext& context, GC::Root<WebIDL::BufferSource> const& audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    ASSERT_CONTROL_THREAD();
    JS::Realm& realm = context.realm();

    auto promise = WebIDL::create_promise(realm);

    // (A) 1. If this's relevant global object's associated Document is not fully active then
    //        return a promise rejected with an "InvalidStateError" DOMException.
    bool is_detached = false;
    if (auto* window = as_if<HTML::Window>(HTML::relevant_global_object(context))) {
        auto navigable = window->navigable();
        is_detached = navigable && navigable->has_been_destroyed();
    }
    if (is_detached || !m_document->is_fully_active()) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Document not active"_utf16));
        return promise;
    }

    auto reject_with_data_clone_error = [&](Utf16String message, Optional<u64> request_id_to_remove = {}) {
        // (A) 4.1. Let error be a DataCloneError.
        auto exception = WebIDL::DataCloneError::create(realm, message);

        // (A) 4.2. Reject promise with error, and remove it from [[pending promises]].
        if (request_id_to_remove.has_value()) {
            (void)context.take_pending_promise(promise);
            m_pending_requests.remove_first_matching([&](auto const& request) {
                return request.request_id == *request_id_to_remove;
            });
        }
        WebIDL::reject_promise(realm, promise, exception);

        // (A) 4.3. Queue a media element task to invoke errorCallback with error.
        if (error_callback) {
            queue_a_document_media_element_task(GC::create_function(m_document->heap(), [document = m_document, error_callback, message = move(message)]() mutable {
                JS::Realm& realm = document->realm();
                HTML::TemporaryExecutionContext execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                auto exception = WebIDL::DataCloneError::create(realm, message);
                (void)WebIDL::invoke_callback(*error_callback, {}, WebIDL::ExceptionBehavior::Report, { { exception } });
            }));
        }
    };

    JS::Value buffer_source_value { audio_data->raw_object().ptr() };
    if (WebIDL::is_buffer_source_detached(buffer_source_value)) {
        reject_with_data_clone_error("Audio data is detached"_utf16);
        return promise;
    }

    // (A) 3.1. Append promise to [[pending promises]].
    context.m_pending_promises.append(promise);

    u64 request_id = m_next_request_id++;
    m_pending_requests.append(PendingRequest {
        .request_id = request_id,
        .promise = promise,
        .success_callback = success_callback,
        .error_callback = error_callback,
        .context = context,
        .sample_rate = context.sample_rate(),
    });

    // Copy the buffer source on the main thread. The background thread must only see plain data.
    auto encoded_or_error = WebIDL::get_buffer_source_copy(audio_data->raw_object());

    auto viewed_array_buffer = audio_data->viewed_array_buffer();
    VERIFY(viewed_array_buffer);
    // (A) 3.2. Detach the audioData ArrayBuffer. If this operation throws, jump to step 4.1.
    auto detach_result = JS::detach_array_buffer(realm.vm(), *viewed_array_buffer);
    if (detach_result.is_throw_completion()) {
        reject_with_data_clone_error("Unable to detach audio data"_utf16, request_id);

        return promise;
    }

    // (A) 3.3. Queue a decoding operation to be performed on another thread.
    if (encoded_or_error.is_error()) {
        settle(request_id, {});
        return promise;
    }

    Optional<u32> target_sample_rate;
    if (context.sample_rate() > 0)
        target_sample_rate = static_cast<u32>(context.sample_rate());

    decode_audio_data_async(GC::Weak<DOM::Document> { m_document }, request_id, encoded_or_error.release_value(), target_sample_rate);
    return promise;
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
// Web Audio API: BaseAudioContext.decodeAudioData(audioData, successCallback, errorCallback)
//
// (B) When queuing a decoding operation to be performed on another thread, the following steps
//     MUST happen on a thread that is not the control thread nor the rendering thread (the
//     "decoding thread")
void BackgroundAudioDecoder::settle(u64 request_id, DecodeResult&& decoded_audio_data)
{
    Optional<size_t> request_index = m_pending_requests.find_first_index_if([&](auto const& request) {
        return request.request_id == request_id;
    });

    if (!request_index.has_value())
        return;

    PendingRequest request = m_pending_requests[*request_index];
    m_pending_requests.remove(*request_index);

    // AD-HOC: Ensure the promise doesn't stay pending forever.
    if (auto context = request.context.ptr(); context)
        (void)context->take_pending_promise(request.promise);

    // (B) 5.2. Queue a media element task to resolve the promise and invoke callbacks.
    queue_a_document_media_element_task(GC::create_function(m_document->heap(), [document = m_document, request, decoded_audio_data = move(decoded_audio_data)]() mutable {
        JS::Realm& realm = document->realm();
        HTML::TemporaryExecutionContext execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        auto reject_with_exception = [&](GC::Ref<WebIDL::DOMException> exception) {
            WebIDL::reject_promise(realm, request.promise, exception);
            if (request.error_callback)
                (void)WebIDL::invoke_callback(*request.error_callback, {}, WebIDL::ExceptionBehavior::Report, { { exception } });
        };

        if (!decoded_audio_data.has_value()) {
            // (B) 4. If can decode is false, queue a media element task to reject with "EncodingError".
            reject_with_exception(WebIDL::EncodingError::create(realm, "Unable to decode"_utf16));
            return;
        }

        u8 const channel_count = decoded_audio_data->sample_specification.channel_count();
        u32 const frame_count = decoded_audio_data->frame_count();

        auto buffer_or_exception = AudioBuffer::create(realm, channel_count, frame_count, request.sample_rate);
        if (buffer_or_exception.is_exception()) {
            // (B) 4. If can decode is false, queue a media element task to reject with "EncodingError".
            reject_with_exception(WebIDL::EncodingError::create(realm, "Unable to decode"_utf16));
            return;
        }
        // (B) 5.2.1. Let buffer be an AudioBuffer containing the final result.
        auto buffer = buffer_or_exception.release_value();

        auto const& interleaved_samples = decoded_audio_data->interleaved_f32_samples;
        size_t const expected_sample_count = static_cast<size_t>(frame_count) * channel_count;
        if (interleaved_samples.size() < expected_sample_count) {
            reject_with_exception(WebIDL::EncodingError::create(realm, "Unable to decode"_utf16));
            return;
        }

        for (u32 channel_index = 0; channel_index < channel_count; ++channel_index) {
            auto channel_array_or_exception = buffer->get_channel_data(channel_index);
            if (channel_array_or_exception.is_exception()) {
                // (B) 4. If can decode is false, queue a media element task to reject with "EncodingError".
                reject_with_exception(WebIDL::EncodingError::create(realm, "Unable to decode"_utf16));
                return;
            }

            auto channel_array = channel_array_or_exception.release_value();
            auto channel_data = channel_array->data();

            for (u32 frame_index = 0; frame_index < frame_count; ++frame_index)
                channel_data[frame_index] = interleaved_samples[(frame_index * channel_count) + channel_index];
        }

        // (B) 5.2.2. Resolve promise with buffer.
        WebIDL::resolve_promise(realm, request.promise, buffer);
        if (request.success_callback)
            // (B) 5.2.3. If successCallback is not missing, invoke successCallback with buffer.
            (void)WebIDL::invoke_callback(*request.success_callback, {}, WebIDL::ExceptionBehavior::Report, { { buffer } });
    }));
}

}
