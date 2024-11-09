/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/RequestPrototype.h>
#include <LibWeb/Bindings/ResponsePrototype.h>
#include <LibWeb/Fetch/Enums.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::Fetch {

// We have a handful of enums that have both a generated and a handwritten version, and need to
// convert between some of them. This has three reasons:
// - Some enums have more internal values in the spec than what is exposed to JS. An example of
//   this is Request::Destination's ServiceWorker member and Request::Mode's WebSocket member,
//   both of which are not present in the IDL-defined enums.
// - The generated enums are not perfect, e.g. "no-cors" becomes NoCors, not NoCORS. This is fine
//   for the generated constructor/prototype code, but not great for the remaining handwritten
//   code.
// - Fetch has use-cases beyond its JS interface, so having to refer to the 'Bindings' namespace
//   constantly is irritating.

ReferrerPolicy::ReferrerPolicy from_bindings_enum(Bindings::ReferrerPolicy referrer_policy)
{
    switch (referrer_policy) {
    case Bindings::ReferrerPolicy::Empty:
        return ReferrerPolicy::ReferrerPolicy::EmptyString;
    case Bindings::ReferrerPolicy::NoReferrer:
        return ReferrerPolicy::ReferrerPolicy::NoReferrer;
    case Bindings::ReferrerPolicy::NoReferrerWhenDowngrade:
        return ReferrerPolicy::ReferrerPolicy::NoReferrerWhenDowngrade;
    case Bindings::ReferrerPolicy::SameOrigin:
        return ReferrerPolicy::ReferrerPolicy::SameOrigin;
    case Bindings::ReferrerPolicy::Origin:
        return ReferrerPolicy::ReferrerPolicy::Origin;
    case Bindings::ReferrerPolicy::StrictOrigin:
        return ReferrerPolicy::ReferrerPolicy::StrictOrigin;
    case Bindings::ReferrerPolicy::OriginWhenCrossOrigin:
        return ReferrerPolicy::ReferrerPolicy::OriginWhenCrossOrigin;
    case Bindings::ReferrerPolicy::StrictOriginWhenCrossOrigin:
        return ReferrerPolicy::ReferrerPolicy::StrictOriginWhenCrossOrigin;
    case Bindings::ReferrerPolicy::UnsafeUrl:
        return ReferrerPolicy::ReferrerPolicy::UnsafeURL;
    default:
        VERIFY_NOT_REACHED();
    }
}

Infrastructure::Request::Mode from_bindings_enum(Bindings::RequestMode mode)
{
    switch (mode) {
    case Bindings::RequestMode::SameOrigin:
        return Infrastructure::Request::Mode::SameOrigin;
    case Bindings::RequestMode::Cors:
        return Infrastructure::Request::Mode::CORS;
    case Bindings::RequestMode::NoCors:
        return Infrastructure::Request::Mode::NoCORS;
    case Bindings::RequestMode::Navigate:
        return Infrastructure::Request::Mode::Navigate;
    default:
        VERIFY_NOT_REACHED();
    }
}

Infrastructure::Request::CredentialsMode from_bindings_enum(Bindings::RequestCredentials request_credentials)
{
    switch (request_credentials) {
    case Bindings::RequestCredentials::Omit:
        return Infrastructure::Request::CredentialsMode::Omit;
    case Bindings::RequestCredentials::SameOrigin:
        return Infrastructure::Request::CredentialsMode::SameOrigin;
    case Bindings::RequestCredentials::Include:
        return Infrastructure::Request::CredentialsMode::Include;
    default:
        VERIFY_NOT_REACHED();
    }
}

Infrastructure::Request::CacheMode from_bindings_enum(Bindings::RequestCache request_cache)
{
    switch (request_cache) {
    case Bindings::RequestCache::Default:
        return Infrastructure::Request::CacheMode::Default;
    case Bindings::RequestCache::NoStore:
        return Infrastructure::Request::CacheMode::NoStore;
    case Bindings::RequestCache::Reload:
        return Infrastructure::Request::CacheMode::Reload;
    case Bindings::RequestCache::NoCache:
        return Infrastructure::Request::CacheMode::NoCache;
    case Bindings::RequestCache::ForceCache:
        return Infrastructure::Request::CacheMode::ForceCache;
    case Bindings::RequestCache::OnlyIfCached:
        return Infrastructure::Request::CacheMode::OnlyIfCached;
    default:
        VERIFY_NOT_REACHED();
    }
}

Infrastructure::Request::RedirectMode from_bindings_enum(Bindings::RequestRedirect request_redirect)
{
    switch (request_redirect) {
    case Bindings::RequestRedirect::Follow:
        return Infrastructure::Request::RedirectMode::Follow;
    case Bindings::RequestRedirect::Error:
        return Infrastructure::Request::RedirectMode::Error;
    case Bindings::RequestRedirect::Manual:
        return Infrastructure::Request::RedirectMode::Manual;
    default:
        VERIFY_NOT_REACHED();
    }
}

Infrastructure::Request::Priority from_bindings_enum(Bindings::RequestPriority request_priority)
{
    switch (request_priority) {
    case Bindings::RequestPriority::High:
        return Infrastructure::Request::Priority::High;
    case Bindings::RequestPriority::Low:
        return Infrastructure::Request::Priority::Low;
    case Bindings::RequestPriority::Auto:
        return Infrastructure::Request::Priority::Auto;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::ReferrerPolicy to_bindings_enum(ReferrerPolicy::ReferrerPolicy referrer_policy)
{
    switch (referrer_policy) {
    case ReferrerPolicy::ReferrerPolicy::EmptyString:
        return Bindings::ReferrerPolicy::Empty;
    case ReferrerPolicy::ReferrerPolicy::NoReferrer:
        return Bindings::ReferrerPolicy::NoReferrer;
    case ReferrerPolicy::ReferrerPolicy::NoReferrerWhenDowngrade:
        return Bindings::ReferrerPolicy::NoReferrerWhenDowngrade;
    case ReferrerPolicy::ReferrerPolicy::SameOrigin:
        return Bindings::ReferrerPolicy::SameOrigin;
    case ReferrerPolicy::ReferrerPolicy::Origin:
        return Bindings::ReferrerPolicy::Origin;
    case ReferrerPolicy::ReferrerPolicy::StrictOrigin:
        return Bindings::ReferrerPolicy::StrictOrigin;
    case ReferrerPolicy::ReferrerPolicy::OriginWhenCrossOrigin:
        return Bindings::ReferrerPolicy::OriginWhenCrossOrigin;
    case ReferrerPolicy::ReferrerPolicy::StrictOriginWhenCrossOrigin:
        return Bindings::ReferrerPolicy::StrictOriginWhenCrossOrigin;
    case ReferrerPolicy::ReferrerPolicy::UnsafeURL:
        return Bindings::ReferrerPolicy::UnsafeUrl;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::RequestDestination to_bindings_enum(Optional<Infrastructure::Request::Destination> const& destination)
{
    if (!destination.has_value())
        return Bindings::RequestDestination::Empty;
    switch (*destination) {
    case Infrastructure::Request::Destination::Audio:
        return Bindings::RequestDestination::Audio;
    case Infrastructure::Request::Destination::AudioWorklet:
        return Bindings::RequestDestination::Audioworklet;
    case Infrastructure::Request::Destination::Document:
        return Bindings::RequestDestination::Document;
    case Infrastructure::Request::Destination::Embed:
        return Bindings::RequestDestination::Embed;
    case Infrastructure::Request::Destination::Font:
        return Bindings::RequestDestination::Font;
    case Infrastructure::Request::Destination::Frame:
        return Bindings::RequestDestination::Frame;
    case Infrastructure::Request::Destination::IFrame:
        return Bindings::RequestDestination::Iframe;
    case Infrastructure::Request::Destination::Image:
        return Bindings::RequestDestination::Image;
    case Infrastructure::Request::Destination::JSON:
        return Bindings::RequestDestination::Json;
    case Infrastructure::Request::Destination::Manifest:
        return Bindings::RequestDestination::Manifest;
    case Infrastructure::Request::Destination::Object:
        return Bindings::RequestDestination::Object;
    case Infrastructure::Request::Destination::PaintWorklet:
        return Bindings::RequestDestination::Paintworklet;
    case Infrastructure::Request::Destination::Report:
        return Bindings::RequestDestination::Report;
    case Infrastructure::Request::Destination::Script:
        return Bindings::RequestDestination::Script;
    case Infrastructure::Request::Destination::ServiceWorker:
        // NOTE: "serviceworker" is omitted from RequestDestination as it cannot be observed from JavaScript.
        //       Implementations will still need to support it as a destination.
        VERIFY_NOT_REACHED();
    case Infrastructure::Request::Destination::SharedWorker:
        return Bindings::RequestDestination::Sharedworker;
    case Infrastructure::Request::Destination::Style:
        return Bindings::RequestDestination::Style;
    case Infrastructure::Request::Destination::Track:
        return Bindings::RequestDestination::Track;
    case Infrastructure::Request::Destination::Video:
        return Bindings::RequestDestination::Video;
    case Infrastructure::Request::Destination::Worker:
        return Bindings::RequestDestination::Worker;
    case Infrastructure::Request::Destination::XSLT:
        return Bindings::RequestDestination::Xslt;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::RequestMode to_bindings_enum(Infrastructure::Request::Mode mode)
{
    switch (mode) {
    case Infrastructure::Request::Mode::SameOrigin:
        return Bindings::RequestMode::SameOrigin;
    case Infrastructure::Request::Mode::CORS:
        return Bindings::RequestMode::Cors;
    case Infrastructure::Request::Mode::NoCORS:
        return Bindings::RequestMode::NoCors;
    case Infrastructure::Request::Mode::Navigate:
        return Bindings::RequestMode::Navigate;
    case Infrastructure::Request::Mode::WebSocket:
        // NOTE: "websocket" is omitted from RequestMode as it cannot be used nor observed from JavaScript.
        VERIFY_NOT_REACHED();
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::RequestCredentials to_bindings_enum(Infrastructure::Request::CredentialsMode credentials_mode)
{
    switch (credentials_mode) {
    case Infrastructure::Request::CredentialsMode::Omit:
        return Bindings::RequestCredentials::Omit;
    case Infrastructure::Request::CredentialsMode::SameOrigin:
        return Bindings::RequestCredentials::SameOrigin;
    case Infrastructure::Request::CredentialsMode::Include:
        return Bindings::RequestCredentials::Include;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::RequestCache to_bindings_enum(Infrastructure::Request::CacheMode cache_mode)
{
    switch (cache_mode) {
    case Infrastructure::Request::CacheMode::Default:
        return Bindings::RequestCache::Default;
    case Infrastructure::Request::CacheMode::NoStore:
        return Bindings::RequestCache::NoStore;
    case Infrastructure::Request::CacheMode::Reload:
        return Bindings::RequestCache::Reload;
    case Infrastructure::Request::CacheMode::NoCache:
        return Bindings::RequestCache::NoCache;
    case Infrastructure::Request::CacheMode::ForceCache:
        return Bindings::RequestCache::ForceCache;
    case Infrastructure::Request::CacheMode::OnlyIfCached:
        return Bindings::RequestCache::OnlyIfCached;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::RequestRedirect to_bindings_enum(Infrastructure::Request::RedirectMode redirect_mode)
{
    switch (redirect_mode) {
    case Infrastructure::Request::RedirectMode::Follow:
        return Bindings::RequestRedirect::Follow;
    case Infrastructure::Request::RedirectMode::Error:
        return Bindings::RequestRedirect::Error;
    case Infrastructure::Request::RedirectMode::Manual:
        return Bindings::RequestRedirect::Manual;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::ResponseType to_bindings_enum(Infrastructure::Response::Type type)
{
    switch (type) {
    case Infrastructure::Response::Type::Basic:
        return Bindings::ResponseType::Basic;
    case Infrastructure::Response::Type::CORS:
        return Bindings::ResponseType::Cors;
    case Infrastructure::Response::Type::Default:
        return Bindings::ResponseType::Default;
    case Infrastructure::Response::Type::Error:
        return Bindings::ResponseType::Error;
    case Infrastructure::Response::Type::Opaque:
        return Bindings::ResponseType::Opaque;
    case Infrastructure::Response::Type::OpaqueRedirect:
        return Bindings::ResponseType::Opaqueredirect;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
