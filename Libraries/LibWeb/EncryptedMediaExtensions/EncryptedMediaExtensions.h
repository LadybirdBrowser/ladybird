/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Bindings/MediaKeySystemAccessPrototype.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Bindings {

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemmediacapability
struct MediaKeySystemMediaCapability {
    WebIDL::DOMString content_type;
    Optional<WebIDL::DOMString> encryption_scheme;
    WebIDL::DOMString robustness;
};

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemconfiguration
struct MediaKeySystemConfiguration {
    WebIDL::DOMString label;
    Vector<WebIDL::DOMString> init_data_types;
    Vector<MediaKeySystemMediaCapability> audio_capabilities;
    Vector<MediaKeySystemMediaCapability> video_capabilities;
    Bindings::MediaKeysRequirement distinctive_identifier = Bindings::MediaKeysRequirement::Optional;
    Bindings::MediaKeysRequirement persistent_state = Bindings::MediaKeysRequirement::Optional;
    Optional<Vector<WebIDL::DOMString>> session_types;
};

}

namespace Web::EncryptedMediaExtensions {

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
