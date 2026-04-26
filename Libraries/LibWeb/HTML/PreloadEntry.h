/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/links.html#preload-key
struct PreloadKey {
    // URL
    //     A URL
    URL::URL url;

    // destination
    //     A preload destination
    Optional<Fetch::Infrastructure::Request::Destination> destination;

    // mode
    //     A request mode, either "same-origin", "cors", or "no-cors"
    Fetch::Infrastructure::Request::Mode mode;

    // credentials mode
    //     A credentials mode
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode;

    [[nodiscard]] bool operator==(PreloadKey const& other) const = default;
};

// https://html.spec.whatwg.org/multipage/links.html#create-a-preload-key
PreloadKey create_a_preload_key(Fetch::Infrastructure::Request const&);

// https://html.spec.whatwg.org/multipage/links.html#translate-a-preload-destination
Variant<Empty, Optional<Fetch::Infrastructure::Request::Destination>> translate_a_preload_destination(Optional<String> const& destination);

// https://html.spec.whatwg.org/multipage/links.html#preload-entry
class PreloadEntry final : public JS::Cell {
    GC_CELL(PreloadEntry, JS::Cell);
    GC_DECLARE_ALLOCATOR(PreloadEntry);

public:
    virtual void visit_edges(Cell::Visitor&) override;

    // integrity metadata
    //     A string
    String integrity_metadata;

    // response
    //     Null or a response
    GC::Ptr<Fetch::Infrastructure::Response> response;

    // on response available
    //     Null, or an algorithm accepting a response or null
    // The callback is always invoked with a non-null response — either entry's resolved response,
    // or the response delivered by the preload's fetch.
    GC::Ptr<GC::Function<void(GC::Ref<Fetch::Infrastructure::Response>)>> on_response_available;
};

// https://html.spec.whatwg.org/multipage/links.html#consume-a-preloaded-resource
bool consume_a_preloaded_resource(
    Window&,
    URL::URL const& url,
    Optional<Fetch::Infrastructure::Request::Destination> destination,
    Fetch::Infrastructure::Request::Mode mode,
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode,
    String const& integrity_metadata,
    GC::Ref<GC::Function<void(GC::Ref<Fetch::Infrastructure::Response>)>> on_response_available);

}

namespace AK {

template<>
struct Traits<Web::HTML::PreloadKey> : public DefaultTraits<Web::HTML::PreloadKey> {
    static unsigned hash(Web::HTML::PreloadKey const& key)
    {
        u32 url_hash = Traits<URL::URL>::hash(key.url);
        // Offset the destination by 1 so that "absent" hashes differently from Destination::Audio (value 0).
        u32 destination_hash = key.destination.has_value() ? static_cast<u32>(*key.destination) + 1 : 0;
        u32 mode_hash = static_cast<u32>(key.mode);
        u32 credentials_mode_hash = static_cast<u32>(key.credentials_mode);
        return pair_int_hash(pair_int_hash(url_hash, destination_hash), pair_int_hash(mode_hash, credentials_mode_hash));
    }
    static bool equals(Web::HTML::PreloadKey const& a, Web::HTML::PreloadKey const& b)
    {
        return a == b;
    }
};

}
