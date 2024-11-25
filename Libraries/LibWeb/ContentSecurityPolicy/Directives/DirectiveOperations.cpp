/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-fallback-list
// Will return an ordered set of the fallback directives for a specific directive.
// The returned ordered set is sorted from most relevant to least relevant and it includes the effective directive
// itself.
static HashMap<StringView, Vector<StringView>> fetch_directive_fallback_list {
    // "script-src-elem"
    //      1. Return << "script-src-elem", "script-src", "default-src" >>.
    { "script-src-elem"sv, { "script-src-elem"sv, "script-src"sv, "default-src"sv } },

    // "script-src-attr"
    //      1. Return << "script-src-attr", "script-src", "default-src" >>.
    { "script-src-attr"sv, { "script-src-attr"sv, "script-src"sv, "default-src"sv } },

    // "style-src-elem"
    //      1. Return << "style-src-elem", "style-src", "default-src" >>.
    { "style-src-elem"sv, { "style-src-elem"sv, "style-src"sv, "default-src"sv } },

    // "style-src-attr"
    //      1. Return << "style-src-attr", "style-src", "default-src" >>.
    { "style-src-attr"sv, { "style-src-attr"sv, "style-src"sv, "default-src"sv } },

    // "worker-src"
    //      1. Return << "worker-src", "child-src", "script-src", "default-src" >>.
    { "worker-src"sv, { "worker-src"sv, "child-src"sv, "script-src"sv, "default-src"sv } },

    // "connect-src"
    //      1. Return << "connect-src", "default-src" >>.
    { "connect-src"sv, { "connect-src"sv, "default-src"sv } },

    // "manifest-src"
    //      1. Return << "manifest-src", "default-src" >>.
    { "manifest-src"sv, { "manifest-src"sv, "default-src"sv } },

    // "object-src"
    //      1. Return << "object-src", "default-src" >>.
    { "object-src"sv, { "object-src"sv, "default-src"sv } },

    // "frame-src"
    //      1. Return << "frame-src", "child-src", "default-src" >>.
    { "frame-src"sv, { "frame-src"sv, "child-src"sv, "default-src"sv } },

    // "media-src"
    //      1. Return << "media-src", "default-src" >>.
    { "media-src"sv, { "media-src"sv, "default-src"sv } },

    // "font-src"
    //      1. Return << "font-src", "default-src" >>.
    { "font-src"sv, { "font-src"sv, "default-src"sv } },

    // "img-src"
    //      1. Return << "img-src", "default-src" >>.
    { "img-src"sv, { "img-src"sv, "default-src"sv } },
};

// https://w3c.github.io/webappsec-csp/#effective-directive-for-a-request
Optional<FlyString> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request)
{
    // Each fetch directive controls a specific destination of request. Given a request request, the following algorithm
    // returns either null or the name of the request’s effective directive:
    // 1. If request’s initiator is "prefetch" or "prerender", return default-src.
    if (request->initiator() == Fetch::Infrastructure::Request::Initiator::Prefetch || request->initiator() == Fetch::Infrastructure::Request::Initiator::Prerender)
        return Names::DefaultSrc;

    // 2. Switch on request’s destination, and execute the associated steps:
    // the empty string
    //      1. Return connect-src.
    if (!request->destination().has_value())
        return Names::ConnectSrc;

    switch (request->destination().value()) {
    // "manifest"
    //      1. Return manifest-src.
    case Fetch::Infrastructure::Request::Destination::Manifest:
        return Names::ManifestSrc;
    // "object"
    // "embed"
    //      1. Return object-src.
    case Fetch::Infrastructure::Request::Destination::Object:
    case Fetch::Infrastructure::Request::Destination::Embed:
        return Names::ObjectSrc;
    // "frame"
    // "iframe"
    //      1. Return frame-src.
    case Fetch::Infrastructure::Request::Destination::Frame:
    case Fetch::Infrastructure::Request::Destination::IFrame:
        return Names::FrameSrc;
    // "audio"
    // "track"
    // "video"
    //      1. Return media-src.
    case Fetch::Infrastructure::Request::Destination::Audio:
    case Fetch::Infrastructure::Request::Destination::Track:
    case Fetch::Infrastructure::Request::Destination::Video:
        return Names::MediaSrc;
    // "font"
    //      1. Return font-src.
    case Fetch::Infrastructure::Request::Destination::Font:
        return Names::FontSrc;
    // "image"
    //      1. Return img-src.
    case Fetch::Infrastructure::Request::Destination::Image:
        return Names::ImgSrc;
    // "style"
    //      1. Return style-src-elem.
    case Fetch::Infrastructure::Request::Destination::Style:
        return Names::StyleSrcElem;
    // "script"
    // "xslt"
    // "audioworklet"
    // "paintworklet"
    //      1. Return script-src-elem.
    case Fetch::Infrastructure::Request::Destination::Script:
    case Fetch::Infrastructure::Request::Destination::XSLT:
    case Fetch::Infrastructure::Request::Destination::AudioWorklet:
    case Fetch::Infrastructure::Request::Destination::PaintWorklet:
        return Names::ScriptSrcElem;
    // "serviceworker"
    // "sharedworker"
    // "worker"
    //      1. Return worker-src.
    case Fetch::Infrastructure::Request::Destination::ServiceWorker:
    case Fetch::Infrastructure::Request::Destination::SharedWorker:
    case Fetch::Infrastructure::Request::Destination::Worker:
        return Names::WorkerSrc;
    // "json"
    // "webidentity"
    //      1. Return connect-src.
    case Fetch::Infrastructure::Request::Destination::JSON:
    case Fetch::Infrastructure::Request::Destination::WebIdentity:
        return Names::ConnectSrc;
    // "report"
    //      1. Return null.
    case Fetch::Infrastructure::Request::Destination::Report:
        return OptionalNone {};
    // 3. Return connect-src.
    // Spec Note: The algorithm returns connect-src as a default fallback. This is intended for new fetch destinations
    //            that are added and which don’t explicitly fall into one of the other categories.
    default:
        return Names::ConnectSrc;
    }
}

// https://w3c.github.io/webappsec-csp/#directive-fallback-list
Vector<StringView> get_fetch_directive_fallback_list(Optional<FlyString> directive_name)
{
    if (!directive_name.has_value())
        return {};

    auto list_iterator = fetch_directive_fallback_list.find(directive_name.value());
    if (list_iterator == fetch_directive_fallback_list.end())
        return {};

    return list_iterator->value;
}

// https://w3c.github.io/webappsec-csp/#should-directive-execute
ShouldExecute should_fetch_directive_execute(Optional<FlyString> effective_directive_name, FlyString const& directive_name, GC::Ref<Policy const> policy)
{
    // 1. Let directive fallback list be the result of executing § 6.8.3 Get fetch directive fallback list on effective
    //    directive name.
    auto const& directive_fallback_list = get_fetch_directive_fallback_list(effective_directive_name);

    // 2. For each fallback directive of directive fallback list:
    for (auto fallback_directive : directive_fallback_list) {
        // 1. If directive name is fallback directive, Return "Yes".
        if (directive_name == fallback_directive)
            return ShouldExecute::Yes;

        // 2. If policy contains a directive whose name is fallback directive, Return "No".
        if (policy->contains_directive_with_name(fallback_directive))
            return ShouldExecute::No;
    }

    // 3. Return "No".
    return ShouldExecute::No;
}

}
