/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::Geolocation {

namespace {

struct GeolocationPositionResultAbi {
    bool is_error;
    u16 error_code;
    CoordinatesDataAbi coordinates;
};

struct EmulatedPositionDataAbi {
    u8 kind;
    void* coordinates;
    u16 error_code;
};

}

extern "C" {
// Defined in Bindings/Rust/src/geolocation/geolocation.rs.
Geolocation* web_rust_geolocation_create_geolocation(JSVM*, HTML::Window*);
void web_rust_geolocation_receive_position(JSVM*, GC::Cell* request, GeolocationPositionResultAbi const&);
void web_rust_geolocation_emulated_position_data_changed(JSVM*, Geolocation*, u32 watch_id);
void web_rust_geolocation_document_visibility_changed(JSVM*, GC::Cell* wait, bool is_visible);

u64 web_geolocation_request_position(HTML::Window*, GC::Cell* request, bool is_watch);
void web_geolocation_cancel_position_request(HTML::Window*, u64 request_id);
void web_geolocation_emulated_position_data(HTML::Window*, EmulatedPositionDataAbi*);
u64 web_geolocation_register_emulated_position_data_observer(HTML::Window*, Geolocation*, u32 watch_id);
void web_geolocation_unregister_emulated_position_data_observer(HTML::Window*, u64 observer_id);
DOM::DocumentObserver* web_geolocation_observe_document_visibility(DOM::Document*, GC::Cell* wait);
void web_geolocation_stop_observing_document_visibility(DOM::DocumentObserver*);
}

static JSVM* the_vm()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

GC::Ref<Geolocation> Geolocation::create(HTML::Window& window)
{
    return *web_rust_geolocation_create_geolocation(the_vm(), &window);
}

u64 web_geolocation_request_position(HTML::Window* window, GC::Cell* request, bool is_watch)
{
    auto& document = window->associated_document();
    auto position_callback = GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *request }), [](auto& request, Page::GeolocationPositionResult result) {
        GeolocationPositionResultAbi abi {};
        if (result.has<GeolocationPositionErrorCode>()) {
            abi.is_error = true;
            abi.error_code = to_underlying(result.get<GeolocationPositionErrorCode>());
        } else {
            abi.coordinates = coordinates_data_to_abi(result.get<CoordinatesData>());
        }
        web_rust_geolocation_receive_position(the_vm(), request.ptr(), abi);
    });
    return document.page().request_geolocation_position(document.id(), position_callback, is_watch ? Page::GeolocationRequestType::Watch : Page::GeolocationRequestType::OneShot);
}

void web_geolocation_cancel_position_request(HTML::Window* window, u64 request_id)
{
    window->page().cancel_geolocation_position_request(request_id);
}

void web_geolocation_emulated_position_data(HTML::Window* window, EmulatedPositionDataAbi* data)
{
    window->page().emulated_position_data().visit(
        [&](Empty) { data->kind = 0; },
        [&](GC::Ref<GeolocationCoordinates> coordinates) {
            data->kind = 1;
            data->coordinates = coordinates.ptr();
        },
        [&](GeolocationPositionErrorCode code) {
            data->kind = 2;
            data->error_code = to_underlying(code);
        });
}

u64 web_geolocation_register_emulated_position_data_observer(HTML::Window* window, Geolocation* geolocation, u32 watch_id)
{
    return window->page().register_emulated_position_data_observer(GC::create_function(GC::Heap::the(), GC::captures(*geolocation, watch_id), [](auto& geolocation, auto& watch_id) {
        web_rust_geolocation_emulated_position_data_changed(the_vm(), &geolocation, watch_id);
    }));
}

void web_geolocation_unregister_emulated_position_data_observer(HTML::Window* window, u64 observer_id)
{
    window->page().unregister_emulated_position_data_observer(observer_id);
}

DOM::DocumentObserver* web_geolocation_observe_document_visibility(DOM::Document* document, GC::Cell* wait)
{
    auto observer = DOM::DocumentObserver::create(*document);
    observer->set_document_visibility_state_observer(GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *wait }), [](auto& wait, HTML::VisibilityState state) {
        web_rust_geolocation_document_visibility_changed(the_vm(), wait.ptr(), state == HTML::VisibilityState::Visible);
    }));
    return observer.ptr();
}

void web_geolocation_stop_observing_document_visibility(DOM::DocumentObserver* observer)
{
    observer->set_document_visibility_state_observer({});
}

}
