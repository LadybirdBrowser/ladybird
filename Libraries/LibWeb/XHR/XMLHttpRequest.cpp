/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <LibGC/Heap.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/HeaderList.h>
#include <LibHTTP/Method.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/XMLDocument.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/XHR/XMLHttpRequest.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web::XHR {

struct RequestABI {
    ReadonlyBytes method;
    URL::FFI::RustUrl const* url;
    ReadonlyBytes const* header_names;
    ReadonlyBytes const* header_values;
    size_t header_count;
    void* body;
    bool use_cors_preflight;
    bool include_credentials;
    bool use_url_credentials;
};

extern "C" {

// Defined in Bindings/Rust/src/xhr/exports.rs.
void* web_rust_xhr_create(JSVM*, void* realm);
void web_rust_xhr_process_request_body_chunk_length(JSVM*, void* xhr, u64 request_body_length, u64 bytes_length);
void web_rust_xhr_process_request_end_of_body(JSVM*, void* xhr, u64 request_body_length);
void web_rust_xhr_process_response(JSVM*, void* xhr, Fetch::Infrastructure::ResponseView*);
void web_rust_xhr_process_body_chunk(JSVM*, void* xhr, u64 length, ReadonlyBytes);
void web_rust_xhr_process_end_of_body(JSVM*, void* xhr);
void web_rust_xhr_process_body_error(JSVM*, void* xhr);
void web_rust_xhr_process_response_consume_body(JSVM*, void* xhr, void* sync_send, Fetch::Infrastructure::ResponseView*, u8 kind, ReadonlyBytes);
void web_rust_xhr_fetch_stopped(JSVM*, void* xhr, Fetch::Infrastructure::FetchController*);
bool web_rust_xhr_sync_send_is_over(void* sync_send);

// Called by Bindings/Rust/src/xhr/xml_http_request.rs.
bool web_xhr_is_method(ReadonlyBytes method);
bool web_xhr_is_forbidden_method(ReadonlyBytes method);
void web_xhr_normalize_method(ReadonlyBytes method, void* bytes, void (*on_bytes)(void*, ReadonlyBytes));
void web_xhr_normalize_header_value(ReadonlyBytes value, void* bytes, void (*on_bytes)(void*, ReadonlyBytes));
bool web_xhr_is_header_name(ReadonlyBytes name);
bool web_xhr_is_header_value(ReadonlyBytes value);
bool web_xhr_is_forbidden_request_header(ReadonlyBytes name, ReadonlyBytes value);
bool web_xhr_document_is_html_document(JSObject* document);
Fetch::Infrastructure::Body* web_xhr_document_as_body(void* realm, JSObject* document);
Fetch::Infrastructure::Request* web_xhr_create_request(void* realm, RequestABI const*);
Fetch::Infrastructure::FetchController* web_xhr_fetch(void* realm, Fetch::Infrastructure::Request*, void* xhr, void* sync_send, u64 request_body_length);
void web_xhr_incrementally_read_body(void* realm, Fetch::Infrastructure::ResponseView*, void* xhr, u64 length);
void web_xhr_pause_until_sync_send_is_over(void* sync_send);
DOM::Document* web_xhr_create_document_response(JSObject* global_object, Fetch::Infrastructure::ResponseView*, ReadonlyBytes received_bytes, FlatPtr final_mime, bool final_mime_is_html, ReadonlyBytes final_encoding, bool has_final_encoding);
}

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

static DOM::Document& document_from_wrapper(JSObject* document)
{
    return as<DOM::Document>(*Bindings::wrappable_impl_from(JS::HostABI::object_from_abi(document)));
}

GC::Ref<XMLHttpRequest> XMLHttpRequest::create(JS::Realm& realm)
{
    return *static_cast<XMLHttpRequest*>(web_rust_xhr_create(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

bool web_xhr_is_method(ReadonlyBytes method)
{
    return HTTP::is_method(StringView { method });
}

bool web_xhr_is_forbidden_method(ReadonlyBytes method)
{
    return HTTP::is_forbidden_method(StringView { method });
}

void web_xhr_normalize_method(ReadonlyBytes method, void* bytes, void (*on_bytes)(void*, ReadonlyBytes))
{
    auto normalized_method = HTTP::normalize_method(StringView { method });
    on_bytes(bytes, normalized_method.bytes());
}

void web_xhr_normalize_header_value(ReadonlyBytes value, void* bytes, void (*on_bytes)(void*, ReadonlyBytes))
{
    on_bytes(bytes, HTTP::normalize_header_value(StringView { value }).bytes());
}

bool web_xhr_is_header_name(ReadonlyBytes name)
{
    return HTTP::is_header_name(StringView { name });
}

bool web_xhr_is_header_value(ReadonlyBytes value)
{
    return HTTP::is_header_value(StringView { value });
}

bool web_xhr_is_forbidden_request_header(ReadonlyBytes name, ReadonlyBytes value)
{
    return HTTP::is_forbidden_request_header({ ByteString { name }, ByteString { value } });
}

bool web_xhr_document_is_html_document(JSObject* document)
{
    return document_from_wrapper(document).is_html_document();
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
Fetch::Infrastructure::Body* web_xhr_document_as_body(void* realm, JSObject* document)
{
    // If body is a Document, then set this’s request body to body, serialized, converted, and UTF-8 encoded.
    auto string_serialized_document = MUST(document_from_wrapper(document).serialize_fragment(HTML::RequireWellFormed::No));
    auto string_serialized_document_utf8 = string_serialized_document.to_well_formed_utf8();
    return Fetch::Infrastructure::byte_sequence_as_body(*static_cast<JS::Realm*>(realm), string_serialized_document_utf8.bytes()).ptr();
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
Fetch::Infrastructure::Request* web_xhr_create_request(void* realm, RequestABI const* request_abi)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& relevant_settings_object = HTML::relevant_settings_object(js_realm.global_object());

    // 6. Let req be a new request, initialized as follows:
    auto request = Fetch::Infrastructure::Request::create();

    // method
    //    This’s request method.
    request->set_method(ByteString { request_abi->method });

    // URL
    //    This’s request URL.
    URL::URL url;
    url.set_from_rust(*request_abi->url);
    request->set_url(move(url));

    // header list
    //    This’s author request headers.
    auto header_list = HTTP::HeaderList::create();
    for (size_t i = 0; i < request_abi->header_count; ++i)
        header_list->append({ ByteString { request_abi->header_names[i] }, ByteString { request_abi->header_values[i] } });
    request->set_header_list(move(header_list));

    // unsafe-request flag
    //    Set.
    request->set_unsafe_request(true);

    // body
    //    This’s request body.
    if (request_abi->body)
        request->set_body(static_cast<Fetch::Infrastructure::Body*>(request_abi->body)->fetch_body());

    // client
    //    This’s relevant settings object.
    request->set_client(relevant_settings_object.address());

    // mode
    //    "cors".
    request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);

    // use-CORS-preflight flag
    //    Set if this’s upload listener flag is set.
    request->set_use_cors_preflight(request_abi->use_cors_preflight);

    // credentials mode
    //    If this’s cross-origin credentials is true, then "include"; otherwise "same-origin".
    request->set_credentials_mode(request_abi->include_credentials ? Fetch::Infrastructure::Request::CredentialsMode::Include : Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);

    // use-URL-credentials flag
    //    Set if this’s request URL includes credentials.
    request->set_use_url_credentials(request_abi->use_url_credentials);

    // initiator type
    //    "xmlhttprequest".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::XMLHttpRequest);

    return &request.leak_ref();
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
Fetch::Infrastructure::FetchController* web_xhr_fetch(void* realm, Fetch::Infrastructure::Request* request, void* xhr, void* sync_send, u64 request_body_length)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    GC::Ref<GC::Cell> xhr_cell = *static_cast<GC::Cell*>(xhr);

    dbgln_if(SPAM_DEBUG, "{}XHR send from {} to {}", sync_send ? "\033[33;1mSynchronous\033[0m " : "", HTML::relevant_settings_object(js_realm.global_object()).creation_url, request->url());

    // 11. If this’s synchronous flag is unset, then:
    if (!sync_send) {
        // 7. Let processRequestBodyChunkLength, given a bytesLength, be these steps:
        // NOTE: `this` is kept alive by FetchAlgorithms using GC::HeapFunction.
        auto process_request_body_chunk_length = GC::create_function<void(u64)>(GC::Heap::the(), GC::captures(xhr_cell, request_body_length), [](auto& xhr, auto& request_body_length, u64 bytes_length) {
            web_rust_xhr_process_request_body_chunk_length(vm_abi(), xhr.ptr(), request_body_length, bytes_length);
        });

        // 8. Let processRequestEndOfBody be these steps:
        // NOTE: `this` is kept alive by FetchAlgorithms using GC::HeapFunction.
        auto process_request_end_of_body = GC::create_function<void()>(GC::Heap::the(), GC::captures(xhr_cell, request_body_length), [](auto& xhr, auto& request_body_length) {
            web_rust_xhr_process_request_end_of_body(vm_abi(), xhr.ptr(), request_body_length);
        });

        // 9. Let processResponse, given a response, be these steps:
        // NOTE: `this` is kept alive by FetchAlgorithms using GC::HeapFunction.
        auto process_response = GC::create_function(GC::Heap::the(), GC::captures(xhr_cell), [](auto& xhr, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
            web_rust_xhr_process_response(vm_abi(), xhr.ptr(), response.ptr());
        });

        // 10. Set this’s fetch controller to the result of fetching req with processRequestBodyChunkLength set to processRequestBodyChunkLength, processRequestEndOfBody set to processRequestEndOfBody, and processResponse set to processResponse.
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
        fetch_algorithms_input.process_request_body_chunk_length = process_request_body_chunk_length;
        fetch_algorithms_input.process_request_end_of_body = process_request_end_of_body;
        fetch_algorithms_input.process_response = process_response;
        auto fetch_controller = Fetch::Fetching::fetch(js_realm, *request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));

        // AD-HOC: Stopping the fetch, as aborting the document does, discards it without a response. Hand this an
        //         aborted network error instead, so that it fires abort as for a canceled fetch.
        fetch_controller->set_stop_steps(GC::create_function(GC::Heap::the(), GC::captures(xhr_cell, fetch_controller), [](auto& xhr, auto& fetch_controller) {
            web_rust_xhr_fetch_stopped(vm_abi(), xhr.ptr(), fetch_controller.ptr());
        }));

        return fetch_controller.ptr();
    }

    // 12. Otherwise, if this’s synchronous flag is set:
    GC::Ref<GC::Cell> sync_send_cell = *static_cast<GC::Cell*>(sync_send);

    // 2. Let processResponseConsumeBody, given a response and nullOrFailureOrBytes, be these steps:
    auto process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(xhr_cell, sync_send_cell), [](auto& xhr, auto& sync_send, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes null_or_failure_or_bytes) {
        u8 kind = 0;
        ReadonlyBytes bytes;
        null_or_failure_or_bytes.visit(
            [&](Empty) { kind = 0; },
            [&](Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag) { kind = 1; },
            [&](Core::ImmutableBytes const& byte_sequence) {
                kind = 2;
                bytes = byte_sequence.bytes();
            });
        web_rust_xhr_process_response_consume_body(vm_abi(), xhr.ptr(), sync_send.ptr(), response.ptr(), kind, bytes);
    });

    // 3. Set this’s fetch controller to the result of fetching req with processResponseConsumeBody set to processResponseConsumeBody and useParallelQueue set to true.
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::SyncAccumulate };
    fetch_algorithms_input.process_response_consume_body = process_response_consume_body;
    return Fetch::Fetching::fetch(js_realm, *request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)), Fetch::Fetching::UseParallelQueue::Yes).ptr();
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
void web_xhr_incrementally_read_body(void* realm, Fetch::Infrastructure::ResponseView* response, void* xhr, u64 length)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    GC::Ref<GC::Cell> xhr_cell = *static_cast<GC::Cell*>(xhr);

    auto process_body_chunk = GC::create_function(GC::Heap::the(), GC::captures(xhr_cell, length), [](auto& xhr, auto& length, ByteBuffer bytes) {
        web_rust_xhr_process_body_chunk(vm_abi(), xhr.ptr(), length, bytes);
    });
    auto process_end_of_body = GC::create_function(GC::Heap::the(), GC::captures(xhr_cell), [](auto& xhr) {
        web_rust_xhr_process_end_of_body(vm_abi(), xhr.ptr());
    });
    auto process_body_error = GC::create_function(GC::Heap::the(), GC::captures(xhr_cell), [](auto& xhr, JS::Value) {
        web_rust_xhr_process_body_error(vm_abi(), xhr.ptr());
    });

    // 13. Incrementally read this’s response’s body, given processBodyChunk, processEndOfBody, processBodyError, and this’s relevant global object.
    GC::Ref<JS::Object> global_object = js_realm.global_object();
    response->body()->incrementally_read(js_realm, process_body_chunk, process_end_of_body, process_body_error, global_object);
}

// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
void web_xhr_pause_until_sync_send_is_over(void* sync_send)
{
    GC::Ref<GC::Cell> sync_send_cell = *static_cast<GC::Cell*>(sync_send);

    // NB: While a sync send() is blocked, the event loop must not run tasks or perform microtask checkpoints
    //     (https://html.spec.whatwg.org/multipage/webappapis.html#pause). The fetch above runs on a parallel
    //     queue, and progresses on the platform event loop (deferred invocations, IPC, timers) — all that's
    //     pumped here. So, the response arrives and is read to completion while the HTML event loop stays
    //     paused; see the sync XHR body path in fetch_response_handover(), and the direct delivery in
    //     BodyStreamPullSource::push_while_paused(). Blink/WebKit block the thread outright for a sync load:
    //     Blink on a WaitableEvent (ResourceRequestSender::SendSync), WebKit on a sync IPC reply
    //     (WebLoaderStrategy::loadResourceSynchronously). Gecko instead spins a nested event loop
    //     (XMLHttpRequestMainThread::SendInternal), with the top window suspended and its documents' microtasks
    //     deferred by nsAutoSyncOperation — so its timers and author microtasks hold as well.
    // AD-HOC: We don't update the rendering when pausing: That runs author callbacks (e.g., rAF callbacks),
    //         which must not run inside a blocked send() — itself possibly invoked from within a microtask.
    auto pause_handle = HTML::current_event_loop().pause(HTML::EventLoop::UpdateTheRendering::No);
    Platform::EventLoopPlugin::the().spin_until(GC::create_function(GC::Heap::the(), GC::captures(sync_send_cell), [](auto& sync_send) {
        return web_rust_xhr_sync_send_is_over(sync_send.ptr());
    }));
}

// https://xhr.spec.whatwg.org/#document-response
DOM::Document* web_xhr_create_document_response(JSObject* global_object, Fetch::Infrastructure::ResponseView* response, ReadonlyBytes received_bytes, FlatPtr final_mime, bool final_mime_is_html, ReadonlyBytes final_encoding, bool has_final_encoding)
{
    auto& global = *JS::HostABI::object_from_abi(global_object);
    Optional<StringView> final_encoding_name;
    if (has_final_encoding)
        final_encoding_name = StringView { final_encoding };

    // 5. If finalMIME is an HTML MIME type, then:
    Optional<Utf16String> charset;
    GC::Ptr<DOM::Document> document;
    auto& global_scope = HTML::relevant_window_or_worker_global_scope(global);
    auto& page = [&]() -> Page& {
        if (auto* window = as_if<HTML::Window>(&global_scope))
            return window->page();
        auto* worker_global_scope = as_if<HTML::WorkerGlobalScope>(&global_scope);
        VERIFY(worker_global_scope);
        auto* worker_page = worker_global_scope->page();
        VERIFY(worker_page);
        return *worker_page;
    }();

    if (final_mime_is_html) {
        // 5.1. Let charset be the result of get a final encoding for xhr.
        if (final_encoding_name.has_value())
            charset = Utf16String::from_utf8(*final_encoding_name);

        // 5.2. If charset is null, prescan the first 1024 bytes of xhr’s received bytes and if that does not terminate unsuccessfully then let charset be the return value.
        document = DOM::Document::create(page, global_scope.this_impl());
        if (!charset.has_value())
            if (auto found_charset = HTML::run_prescan_byte_stream_algorithm(*document, received_bytes); found_charset.has_value())
                charset = Utf16String::from_utf8(MUST(String::from_byte_string(found_charset.value())));

        // 5.3. If charset is null, then set charset to UTF-8.
        if (!charset.has_value())
            charset = "UTF-8"_utf16;

        // 5.4. Let document be a document that represents the result parsing xhr’s received bytes following the rules set forth in the HTML Standard for an HTML parser with scripting disabled and a known definite encoding charset.
        auto scripting_mode = document->is_scripting_enabled() ? HTML::ParserScriptingMode::Normal : HTML::ParserScriptingMode::Disabled;
        auto charset_for_parser = charset->to_utf8();
        auto parser = HTML::HTMLParser::create_from_byte_string(*document, received_bytes, scripting_mode, charset_for_parser);
        parser->run(document->url());

        // 5.5. Flag document as an HTML document.
        document->set_document_type(DOM::Document::Type::HTML);
    }

    // 6. Otherwise, let document be a document that represents the result of running the XML parser with XML scripting support disabled on xhr’s received bytes. If that fails (unsupported character encoding, namespace well-formedness error, etc.), then return null.
    else {
        document = DOM::XMLDocument::create(page, global_scope.this_impl(), response->url().value_or({}));
        if (!Web::build_xml_document(*document, MUST(ByteBuffer::copy(received_bytes)), final_encoding_name, XMLScriptingSupport::Disabled))
            return nullptr;
    }

    // 7. If charset is null, then set charset to UTF-8.
    if (!charset.has_value())
        charset = "UTF-8"_utf16;

    // 8. Set document’s encoding to charset.
    document->set_encoding(move(charset));

    // 9. Set document’s content type to finalMIME.
    auto final_mime_string = Utf16String::from_raw(final_mime);
    document->set_content_type(Utf16FlyString::from_utf16(final_mime_string.utf16_view()));

    // 10. Set document’s URL to xhr’s response’s URL.
    document->set_url(response->url().value_or({}));

    // 11. Set document’s origin to xhr’s relevant settings object’s origin.
    document->set_origin(HTML::relevant_settings_object(global).origin());

    return document.ptr();
}

}
