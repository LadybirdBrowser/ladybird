/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/EncryptedMediaExtensions/KeySystem.h>

namespace Web::EncryptedMediaExtensions {

bool supports_container(String const& container);
bool is_persistent_session_type(String const& session_type);
Vector<Bindings::MediaKeySystemMediaCapability> get_supported_capabilities_for_audio_video_type(RefPtr<KeySystem>, CapabilitiesType, Vector<Bindings::MediaKeySystemMediaCapability>, Bindings::MediaKeySystemConfiguration, MediaKeyRestrictions);
ConsentStatus get_consent_status(Bindings::MediaKeySystemConfiguration const&, MediaKeyRestrictions&, URL::Origin const&);
Optional<ConsentConfiguration> get_supported_configuration_and_consent(RefPtr<KeySystem>, Bindings::MediaKeySystemConfiguration const&, MediaKeyRestrictions&, URL::Origin const&);

}
