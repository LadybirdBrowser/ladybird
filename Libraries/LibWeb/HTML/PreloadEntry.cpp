/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/PreloadEntry.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PreloadEntry);

void PreloadEntry::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(response);
    visitor.visit(on_response_available);
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
Variant<Empty, Optional<Fetch::Infrastructure::Request::Destination>> translate_a_preload_destination(Optional<String> const& destination)
{
    // 1. If destination is not "fetch", "font", "image", "script", "style", or "track", then return null.
    if (!destination.has_value() || !destination->is_one_of("fetch"sv, "font"sv, "image"sv, "script"sv, "style"sv, "track"sv))
        return {};

    // 2. Return the result of translating destination.
    return Fetch::Infrastructure::translate_potential_destination(*destination);
}

// https://html.spec.whatwg.org/multipage/links.html#consume-a-preloaded-resource
bool consume_a_preloaded_resource(
    Window& window,
    URL::URL const& url,
    Optional<Fetch::Infrastructure::Request::Destination> destination,
    Fetch::Infrastructure::Request::Mode mode,
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode,
    String const& integrity_metadata,
    GC::Ref<GC::Function<void(GC::Ref<Fetch::Infrastructure::Response>)>> on_response_available)
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

    // 8. Remove preloads[key].
    preloads.remove(it);

    // 9. If entry's response is null, then set entry's on response available to onResponseAvailable.
    if (!entry->response) {
        entry->on_response_available = on_response_available;
    }
    // 10. Otherwise, call onResponseAvailable with entry's response.
    else {
        on_response_available->function()(*entry->response);
    }

    // 11. Return true.
    return true;
}

// Implements steps 1, 2, and 5 of the processResponseConsumeBody callback in the preload algorithm:
// https://html.spec.whatwg.org/multipage/links.html#preload (step 11).
GC::Ref<Fetch::Infrastructure::Response> deliver_preload_response(
    JS::Realm& realm,
    PreloadEntry& entry,
    GC::Ref<Fetch::Infrastructure::Response> response,
    ByteBuffer const* body_bytes)
{
    // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
    //        https://github.com/whatwg/html/issues/9355
    response = response->unsafe_response();

    // 1. If bodyBytes is a byte sequence, then set response's body to bodyBytes as a body.
    if (body_bytes)
        response->set_body(Fetch::Infrastructure::byte_sequence_as_body(realm, *body_bytes));
    // 2. Otherwise, set response to a network error.
    else
        response = Fetch::Infrastructure::Response::network_error(realm.vm(), "Expected preload response to contain a body"_string);

    // 5. If entry's on response available is null, then set entry's response to response; otherwise call entry's
    //    on response available given response.
    if (!entry.on_response_available)
        entry.response = response;
    else
        entry.on_response_available->function()(response);

    return response;
}

}
