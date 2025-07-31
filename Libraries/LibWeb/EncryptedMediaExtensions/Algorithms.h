/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/EncryptedMediaExtensions/KeySystem.h>

namespace Web::EncryptedMediaExtensions {

bool supports_container(Utf16String const& container);
Optional<Vector<Bindings::MediaKeySystemMediaCapability>> get_supported_capabilities_for_audio_video_type(KeySystem const&, CapabilitiesType, Vector<Bindings::MediaKeySystemMediaCapability>, Bindings::MediaKeySystemConfiguration, MediaKeyRestrictions);

}
