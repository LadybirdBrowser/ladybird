/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void web_rust_event_source_process_response(JSVM*, void* event_source, Fetch::Infrastructure::ResponseView*);
void web_rust_event_source_process_response_end_of_body(JSVM*, void* event_source, Fetch::Infrastructure::ResponseView*);
void web_rust_event_source_process_body_chunk(JSVM*, void* event_source, ReadonlyBytes);

// Called by Bindings/Rust/src/html/event_source.rs.
Fetch::Infrastructure::Request* web_html_event_source_create_request(EnvironmentSettingsObject*, URL::FFI::RustUrl const*, bool use_credentials);
Fetch::Infrastructure::FetchController* web_html_event_source_fetch(void* realm, Fetch::Infrastructure::Request*, void* event_source);
void web_html_event_source_incrementally_read_body(void* realm, Fetch::Infrastructure::ResponseView*, void* event_source);
}

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
Fetch::Infrastructure::Request* web_html_event_source_create_request(EnvironmentSettingsObject* settings, URL::FFI::RustUrl const* url, bool use_credentials)
{
    URL::URL url_record;
    url_record.set_from_rust(*url);

    // 8. Let request be the result of creating a potential-CORS request given urlRecord, the empty string, and corsAttributeState.
    auto request = create_potential_CORS_request(url_record, {}, use_credentials ? CORSSettingAttribute::UseCredentials : CORSSettingAttribute::Anonymous);

    // 9. Set request's client to settings.
    request->set_client(settings->address());

    // 10. User agents may set (`Accept`, `text/event-stream`) in request's header list.
    request->header_list()->set({ "Accept"sv, "text/event-stream"sv });

    // 11. Set request's cache mode to "no-store".
    request->set_cache_mode(HTTP::CacheMode::NoStore);

    // 12. Set request's initiator type to "other".
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Other);

    return &request.leak_ref();
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
Fetch::Infrastructure::FetchController* web_html_event_source_fetch(void* realm, Fetch::Infrastructure::Request* request, void* event_source)
{
    GC::Ref<GC::Cell> event_source_cell = *static_cast<GC::Cell*>(event_source);

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
    fetch_algorithms_input.process_response_end_of_body = GC::create_function(GC::Heap::the(), GC::captures(event_source_cell), [](auto& event_source, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
        web_rust_event_source_process_response_end_of_body(vm_abi(), event_source.ptr(), response.ptr());
    });
    fetch_algorithms_input.process_response = GC::create_function(GC::Heap::the(), GC::captures(event_source_cell), [](auto& event_source, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        web_rust_event_source_process_response(vm_abi(), event_source.ptr(), response->unsafe_response().ptr());
    });

    return Fetch::Fetching::fetch(*static_cast<JS::Realm*>(realm), *request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input))).ptr();
}

// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
void web_html_event_source_incrementally_read_body(void* realm, Fetch::Infrastructure::ResponseView* response, void* event_source)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    GC::Ref<GC::Cell> event_source_cell = *static_cast<GC::Cell*>(event_source);

    auto process_body_chunk = GC::create_function(GC::Heap::the(), GC::captures(event_source_cell), [](auto& event_source, ByteBuffer bytes) {
        web_rust_event_source_process_body_chunk(vm_abi(), event_source.ptr(), bytes);
    });
    // NB: processEventSourceEndOfBody handles the end of the body and its errors.
    auto process_end_of_body = GC::create_function(GC::Heap::the(), [] { });
    auto process_body_error = GC::create_function(GC::Heap::the(), [](JS::Value) { });

    response->body()->incrementally_read(js_realm, process_body_chunk, process_end_of_body, process_body_error, { js_realm.global_object() });
}

}
