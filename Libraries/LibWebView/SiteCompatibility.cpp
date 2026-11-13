/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Resource.h>
#include <LibWebCommon/Loader/SiteCompatibility.h>
#include <LibWebView/SiteCompatibility.h>

namespace WebView {

ErrorOr<JsonValue> load_site_compatibility_data(StringView resource_uri)
{
    auto resource = TRY(Core::Resource::load_from_uri(resource_uri));
    auto data = TRY(JsonValue::from_string(resource->data()));
    TRY(Web::SiteCompatibilityData::from_json(data));
    return data;
}

}
