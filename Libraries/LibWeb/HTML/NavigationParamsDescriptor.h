/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibHTTP/Header.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicyEnforcementResult.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// Process-safe representations of the records produced by creating navigation params. The
// selected WebContent process materializes the GC-backed Request, Response, and Environment from
// these records when it runs the navigation-and-traversal task queued by population step 5.
struct NavigationRequestDescriptor {
    Vector<URL::URL> url_list;
    ByteString method;
    Fetch::Infrastructure::Request::ReferrerType referrer;
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };
    SerializedPolicyContainer policy_container;
};

struct NavigationResponseBodyHandle {
    // This pair identifies a RequestServer transfer lease. It remains stable as the
    // response moves from the fetch worker through the UI process to the document host.
    int request_server_client_id { -1 };
    u64 request_server_request_id { 0 };
};

using NavigationResponseBody = Variant<Empty, NavigationResponseBodyHandle, ByteBuffer>;

struct NavigationResponseDescriptor {
    Vector<URL::URL> url_list;
    u16 status { 200 };
    ByteString status_message;
    Vector<HTTP::Header> headers;
    Optional<String> network_error_message;
    bool timing_allow_passed { false };
    NavigationResponseBody body;
};

struct NavigationEnvironmentDescriptor {
    Utf16String id;
    URL::URL creation_url;
    Optional<URL::URL> top_level_creation_url;
    Optional<URL::Origin> top_level_origin;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigation-params
struct NavigationParamsDescriptor {
    Optional<Utf16String> id;
    CrossProcessId navigable_id;
    Optional<NavigationRequestDescriptor> request;
    NavigationResponseDescriptor response;
    OpenerPolicyEnforcementResult coop_enforcement_result;
    Optional<NavigationEnvironmentDescriptor> reserved_environment;
    URL::Origin origin;
    SerializedPolicyContainer policy_container;
    SandboxingFlagSet final_sandboxing_flag_set {};
    ReferrerPolicy::ReferrerPolicy iframe_element_referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
    OpenerPolicy opener_policy;
    Optional<URL::URL> about_base_url;
    UserNavigationInvolvement user_involvement { UserNavigationInvolvement::None };
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#non-fetch-scheme-navigation-params
struct NonFetchSchemeNavigationParamsDescriptor {
    Optional<Utf16String> id;
    CrossProcessId navigable_id;
    URL::URL url;
    SandboxingFlagSet target_snapshot_sandboxing_flags {};
    bool source_snapshot_has_transient_activation { false };
    URL::Origin initiator_origin;
    UserNavigationInvolvement user_involvement { UserNavigationInvolvement::None };
};

using NavigationParamsVariantDescriptor = Variant<NavigationParamsNullOrError, NavigationParamsDescriptor, NonFetchSchemeNavigationParamsDescriptor>;
using NavigationParamsDescriptorCompletion = GC::Function<void(NavigationParamsVariantDescriptor)>;

WEB_API void create_navigation_params_descriptor(JS::Realm&, NavigationParamsVariant, GC::Ref<NavigationParamsDescriptorCompletion>);
WEB_API ErrorOr<NavigationParamsVariant> create_navigation_params_from_descriptor(JS::Realm&, LocalNavigable&, NavigationParamsVariantDescriptor);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::OpenerPolicy const&);

template<>
WEB_API ErrorOr<Web::HTML::OpenerPolicy> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::OpenerPolicyEnforcementResult const&);

template<>
WEB_API ErrorOr<Web::HTML::OpenerPolicyEnforcementResult> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationRequestDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationRequestDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationResponseBodyHandle const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationResponseBodyHandle> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationResponseDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationResponseDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationEnvironmentDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationEnvironmentDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NonFetchSchemeNavigationParamsDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::NonFetchSchemeNavigationParamsDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NavigationParamsDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::NavigationParamsDescriptor> decode(Decoder&);

}
