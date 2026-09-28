/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/NavigateParams.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/PreparedNavigationDescriptor.h>

namespace Web::HTML {

PreparedNavigationDescriptor create_prepared_navigation_descriptor(PreparedNavigation const& navigation)
{
    VERIFY(!navigation.response);
    VERIFY(!navigation.api_method_tracker);

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
        .api_method_tracker = nullptr,
        .csp_navigation_type = descriptor.csp_navigation_type,
        .source_snapshot_params = create_source_snapshot_params_from_navigation_source_snapshot(realm, descriptor.source_snapshot_params),
        .initiator_origin_snapshot = move(descriptor.initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(descriptor.initiator_base_url_snapshot),
    };
}

}
