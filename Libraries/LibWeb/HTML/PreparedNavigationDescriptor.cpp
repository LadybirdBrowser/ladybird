/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/NavigateParams.h>
#include <LibWeb/HTML/PreparedNavigationDescriptor.h>

namespace Web::HTML {

PreparedNavigationDescriptor create_prepared_navigation_descriptor(PreparedNavigation const& navigation)
{
    VERIFY(!navigation.response);

    return {
        .url = navigation.url,
        .document_resource = navigation.document_resource,
        .history_handling = navigation.history_handling,
        .navigation_api_state = navigation.navigation_api_state,
        .referrer_policy = navigation.referrer_policy,
        .user_involvement = navigation.user_involvement,
        .navigation_id = navigation.navigation_id,
        .initial_insertion = navigation.initial_insertion,
        .csp_navigation_type = navigation.csp_navigation_type,
        .source_snapshot_params = create_navigation_source_snapshot(navigation.source_snapshot_params),
        .initiator_origin_snapshot = navigation.initiator_origin_snapshot,
        .initiator_base_url_snapshot = navigation.initiator_base_url_snapshot,
    };
}

PreparedNavigation create_prepared_navigation_from_descriptor(JS::Realm& realm, PreparedNavigationDescriptor descriptor)
{
    return {
        .url = move(descriptor.url),
        .document_resource = move(descriptor.document_resource),
        .response = nullptr,
        .history_handling = descriptor.history_handling,
        .navigation_api_state = move(descriptor.navigation_api_state),
        .form_data_entry_list = {},
        .referrer_policy = descriptor.referrer_policy,
        .user_involvement = descriptor.user_involvement,
        .navigation_id = move(descriptor.navigation_id),
        .source_element = nullptr,
        .initial_insertion = descriptor.initial_insertion,
        .csp_navigation_type = descriptor.csp_navigation_type,
        .source_snapshot_params = create_source_snapshot_params_from_navigation_source_snapshot(realm, descriptor.source_snapshot_params),
        .initiator_origin_snapshot = move(descriptor.initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(descriptor.initiator_base_url_snapshot),
    };
}

}

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
        .csp_navigation_type = TRY(decoder.decode<Web::ContentSecurityPolicy::Directives::Directive::NavigationType>()),
        .source_snapshot_params = TRY(decoder.decode<Web::HTML::NavigationSourceSnapshot>()),
        .initiator_origin_snapshot = TRY(decoder.decode<URL::Origin>()),
        .initiator_base_url_snapshot = TRY(decoder.decode<URL::URL>()),
    };
}

}
