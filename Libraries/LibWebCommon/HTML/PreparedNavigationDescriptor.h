/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Bindings/Navigation.h>
#include <LibWebCommon/ContentSecurityPolicy/Directives/NavigationType.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/InitialInsertion.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>
#include <LibWebCommon/HTML/POSTResource.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/HTML/UserNavigationInvolvement.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

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
    ContentSecurityPolicy::Directives::NavigationType csp_navigation_type;
    NavigationSourceSnapshot source_snapshot_params;
    URL::Origin initiator_origin_snapshot;
    URL::URL initiator_base_url_snapshot;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::PreparedNavigationDescriptor const&);
template<>
WEBCOMMON_API ErrorOr<Web::HTML::PreparedNavigationDescriptor> decode(Decoder&);

}
