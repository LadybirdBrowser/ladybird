/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::PreparedNavigationDescriptor const& navigation)
{
    TRY(encoder.encode(navigation.url));
    TRY(encoder.encode(navigation.document_resource));
    TRY(encoder.encode(navigation.history_handling));
    TRY(encoder.encode(navigation.navigation_api_state));
    TRY(encoder.encode(navigation.referrer_policy));
    TRY(encoder.encode(navigation.user_involvement));
    TRY(encoder.encode(navigation.navigation_id));
    TRY(encoder.encode(navigation.initial_insertion));
    TRY(encoder.encode(navigation.csp_navigation_type));
    TRY(encoder.encode(navigation.source_snapshot_params));
    TRY(encoder.encode(navigation.initiator_origin_snapshot));
    TRY(encoder.encode(navigation.initiator_base_url_snapshot));
    return {};
}

template<>
ErrorOr<Web::HTML::PreparedNavigationDescriptor> decode(Decoder& decoder)
{
    return Web::HTML::PreparedNavigationDescriptor {
        .url = TRY(decoder.decode<URL::URL>()),
        .document_resource = TRY(decoder.decode<Web::HTML::DocumentResource>()),
        .history_handling = TRY(decoder.decode<Web::Bindings::NavigationHistoryBehavior>()),
        .navigation_api_state = TRY(decoder.decode<Optional<Web::HTML::StorageSerializationRecord>>()),
        .referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>()),
        .user_involvement = TRY(decoder.decode<Web::HTML::UserNavigationInvolvement>()),
        .navigation_id = TRY(decoder.decode<Utf16String>()),
        .initial_insertion = TRY(decoder.decode<Web::HTML::InitialInsertion>()),
        .csp_navigation_type = TRY(decoder.decode<Web::ContentSecurityPolicy::Directives::NavigationType>()),
        .source_snapshot_params = TRY(decoder.decode<Web::HTML::NavigationSourceSnapshot>()),
        .initiator_origin_snapshot = TRY(decoder.decode<URL::Origin>()),
        .initiator_base_url_snapshot = TRY(decoder.decode<URL::URL>()),
    };
}

}
