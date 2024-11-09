/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedEnvironmentSettingsObject const& object)
{
    TRY(encoder.encode(object.id));
    TRY(encoder.encode(object.creation_url));
    TRY(encoder.encode(object.top_level_creation_url));
    TRY(encoder.encode(object.top_level_origin));
    TRY(encoder.encode(object.api_url_character_encoding));
    TRY(encoder.encode(object.api_base_url));
    TRY(encoder.encode(object.origin));
    TRY(encoder.encode(object.policy_container));
    TRY(encoder.encode(object.cross_origin_isolated_capability));

    return {};
}

template<>
ErrorOr<Web::HTML::SerializedEnvironmentSettingsObject> decode(Decoder& decoder)
{
    Web::HTML::SerializedEnvironmentSettingsObject object {};

    object.id = TRY(decoder.decode<String>());
    object.creation_url = TRY(decoder.decode<URL::URL>());
    object.top_level_creation_url = TRY(decoder.decode<URL::URL>());
    object.top_level_origin = TRY(decoder.decode<URL::Origin>());
    object.api_url_character_encoding = TRY(decoder.decode<String>());
    object.api_base_url = TRY(decoder.decode<URL::URL>());
    object.origin = TRY(decoder.decode<URL::Origin>());
    object.policy_container = TRY(decoder.decode<Web::HTML::PolicyContainer>());
    object.cross_origin_isolated_capability = TRY(decoder.decode<Web::HTML::CanUseCrossOriginIsolatedAPIs>());

    return object;
}

}
