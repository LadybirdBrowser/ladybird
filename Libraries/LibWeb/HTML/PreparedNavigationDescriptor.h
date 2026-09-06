/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/InitialInsertion.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

struct PreparedNavigationDescriptor {
    URL::URL url;
    DocumentResource document_resource;
    Bindings::NavigationHistoryBehavior history_handling;
    Optional<StorageSerializationRecord> navigation_api_state;
    ReferrerPolicy::ReferrerPolicy referrer_policy;
    UserNavigationInvolvement user_involvement;
    Utf16String navigation_id;
    InitialInsertion initial_insertion;
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type;
    NavigationSourceSnapshot source_snapshot_params;
    URL::Origin initiator_origin_snapshot;
    URL::URL initiator_base_url_snapshot;
};

WEB_API PreparedNavigationDescriptor create_prepared_navigation_descriptor(PreparedNavigation const&);
WEB_API PreparedNavigation create_prepared_navigation_from_descriptor(JS::Realm&, PreparedNavigationDescriptor);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::PreparedNavigationDescriptor const&);
template<>
WEB_API ErrorOr<Web::HTML::PreparedNavigationDescriptor> decode(Decoder&);

}
