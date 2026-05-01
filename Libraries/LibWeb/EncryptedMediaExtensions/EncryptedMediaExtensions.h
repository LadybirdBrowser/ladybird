/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Bindings/MediaKeySystemAccess.h>

namespace Web::EncryptedMediaExtensions {

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemmediacapability
struct MediaKeySystemMediaCapability {
    Utf16String content_type;
    Optional<Utf16String> encryption_scheme;
    Utf16String robustness;
};

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
    Optional<Bindings::MediaKeySystemConfiguration> configuration;
};

}
