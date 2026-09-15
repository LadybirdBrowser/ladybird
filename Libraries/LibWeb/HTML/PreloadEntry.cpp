/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/PreloadEntry.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PreloadEntry);

GC::Ref<PreloadEntry> PreloadEntry::create()
{
    return GC::Heap::the().allocate<PreloadEntry>();
}

void PreloadEntry::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(response);
    visitor.visit(on_response_available);
    visitor.visit(controller);
}

// https://html.spec.whatwg.org/multipage/links.html#create-a-preload-key
PreloadKey create_a_preload_key(Fetch::Infrastructure::Request const& request)
{
    // To create a preload key for a request request, return a new preload key whose URL is request's URL, destination
    // is request's destination, mode is request's mode, and credentials mode is request's credentials mode.
    return PreloadKey {
        .url = request.url(),
        .destination = request.destination(),
        .mode = request.mode(),
        .credentials_mode = request.credentials_mode(),
    };
}

// https://html.spec.whatwg.org/multipage/links.html#translate-a-preload-destination
Variant<Empty, Optional<Fetch::Infrastructure::Request::Destination>> translate_a_preload_destination(Utf16View destination)
{
    // 1. If destination is not "fetch", "font", "image", "script", "style", or "track", then return null.
    if (!destination.is_one_of("fetch"sv, "font"sv, "image"sv, "script"sv, "style"sv, "track"sv))
        return {};

    // 2. Return the result of translating destination.
    return Fetch::Infrastructure::translate_potential_destination(destination);
}

// https://html.spec.whatwg.org/multipage/links.html#consume-a-preloaded-resource
bool consume_a_preloaded_resource(
    Window& window,
    URL::URL const& url,
    Optional<Fetch::Infrastructure::Request::Destination> destination,
    Fetch::Infrastructure::Request::Mode mode,
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode,
    Utf16View integrity_metadata,
    GC::Ref<GC::Function<void(GC::Ref<Fetch::Infrastructure::Response>)>> on_response_available,
    Fetch::Infrastructure::TaskDestination const& consumer_task_destination)
{
    // 1. Let key be a preload key whose URL is url, destination is destination, mode is mode, and credentials mode is
    //    credentialsMode.
    auto key = PreloadKey {
        .url = url,
        .destination = destination,
        .mode = mode,
        .credentials_mode = credentials_mode,
    };

    // 2. Let preloads be window's associated Document's map of preloaded resources.
    auto& preloads = window.associated_document().map_of_preloaded_resources();

    // 3. If key does not exist in preloads, then return false.
    auto it = preloads.find(key);
    if (it == preloads.end())
        return false;

    // 4. Let entry be preloads[key].
    auto entry = it->value;

    // FIXME: 5. Let consumerIntegrityMetadata be the result of parsing integrityMetadata via SRI::parse_metadata.
    // FIXME: 6. Let preloadIntegrityMetadata be the result of parsing entry's integrity metadata via SRI::parse_metadata.
    // FIXME: 7. If none of the following conditions apply:
    //              - consumerIntegrityMetadata is no metadata;
    //              - consumerIntegrityMetadata is equal to preloadIntegrityMetadata;
    //           then return false.
    (void)integrity_metadata;

    // AD-HOC: If entry's response is null but the preload's fetch has already begun its response processing, a consumer
    //         on a parallel queue must not park on the entry: That processing captured the preload's event-loop task
    //         destination when it was scheduled — so it's beyond the reach of the re-targeting in step 9 below. And
    //         with the consumer's event loop paused (sync XHR send()), it can never run to hand the response over.
    //         Return false — so the consumer performs an ordinary fetch of its own instead. And leave the entry in the
    //         map; the preload still completes – for any later consumer — once the event loop resumes.
    if (!entry->response && consumer_task_destination.has<NonnullRefPtr<ParallelQueue>>() && entry->controller
        && entry->controller->response_processing_started()) {
        return false;
    }

    // 8. Remove preloads[key].
    preloads.remove(it);

    // 9. If entry's response is null, then set entry's on response available to onResponseAvailable.
    if (!entry->response) {
        entry->on_response_available = on_response_available;

        // AD-HOC: The preload's fetch is still in flight. If the consumer's fetch runs on a parallel queue, its event
        //         loop is paused for as long as it waits (sync XHR send()). So the preload's fetch — whose response is
        //         otherwise handed over through event-loop tasks — could never deliver. Move the preload's fetch onto
        //         a parallel queue of its own. So, fetch response handover reads its body + runs its algorithms without
        //         the event loop — same way it does for the consumer. See fetch_response_handover()'s parallel-queue
        //         path. (This re-targeting only works because the fetch has not yet begun its response processing —
        //         the guard above step 8 sends the consumer elsewhere once it has.)
        if (consumer_task_destination.has<NonnullRefPtr<ParallelQueue>>() && entry->controller) {
            if (auto fetch_params = entry->controller->fetch_params())
                fetch_params->set_task_destination(ParallelQueue::create());
        }
    }
    // 10. Otherwise, call onResponseAvailable with entry's response.
    else {
        on_response_available->function()(*entry->response);
    }

    // 11. Return true.
    return true;
}

}
