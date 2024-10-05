/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/PolicyContainers.h>

namespace Web::HTML {

enum class CanUseCrossOriginIsolatedAPIs {
    No,
    Yes,
};

struct SerializedEnvironmentSettingsObject {
    String id;
    URL::URL creation_url;
    URL::URL top_level_creation_url;
    URL::Origin top_level_origin;

    String api_url_character_encoding;
    URL::URL api_base_url;
    URL::Origin origin;
    PolicyContainer policy_container;
    CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SerializedEnvironmentSettingsObject const&);

template<>
ErrorOr<Web::HTML::SerializedEnvironmentSettingsObject> decode(Decoder&);

}
