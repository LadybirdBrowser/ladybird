/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/MediaKeySystemAccess.h>

namespace Web::EncryptedMediaExtensions {

using MediaKeysRequirement = Bindings::MediaKeysRequirement;
using MediaKeySystemConfiguration = Bindings::MediaKeySystemConfiguration;
using MediaKeySystemMediaCapability = Bindings::MediaKeySystemMediaCapability;

struct MediaKeyRestrictions {
    bool distinctive_identifiers { true };
    bool persist_state { true };
};

enum CapabilitiesType {
    Audio,
    Video
};

enum ConsentStatus {
    ConsentDenied,
    InformUser,
    Allowed
};

struct ConsentConfiguration {
    ConsentStatus status { ConsentStatus::ConsentDenied };
    Optional<MediaKeySystemConfiguration> configuration;
};

}
