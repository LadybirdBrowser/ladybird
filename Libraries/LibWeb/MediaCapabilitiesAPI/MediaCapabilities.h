/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::MediaCapabilitiesAPI {

bool is_valid_video_configuration(Bindings::VideoConfiguration const&);

bool is_valid_audio_configuration(Bindings::AudioConfiguration const&);

bool is_valid_media_configuration(Bindings::MediaConfiguration const&);

bool is_valid_media_decoding_configuration(Bindings::MediaDecodingConfiguration const&);

GC::Ref<JS::Object> to_object(JS::Realm&, Bindings::MediaCapabilitiesDecodingInfo const&);

// https://w3c.github.io/media-capabilities/#media-capabilities-interface
class MediaCapabilities final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaCapabilities, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaCapabilities);

public:
    static GC::Ref<MediaCapabilities> create(JS::Realm&);
    virtual ~MediaCapabilities() override = default;

    // https://w3c.github.io/media-capabilities/#dom-mediacapabilities-decodinginfo
    GC::Ref<WebIDL::Promise> decoding_info(Bindings::MediaDecodingConfiguration const&);

private:
    MediaCapabilities(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

// https://w3c.github.io/media-capabilities/#queue-a-media-capabilities-task
void queue_a_media_capabilities_task(JS::VM& vm, Function<void()>);

// https://w3c.github.io/media-capabilities/#create-a-mediacapabilitiesdecodinginfo
Bindings::MediaCapabilitiesDecodingInfo create_a_media_capabilities_decoding_info(Bindings::MediaDecodingConfiguration);

bool is_able_to_decode_media(Bindings::MediaDecodingConfiguration);

// https://w3c.github.io/media-capabilities/#valid-audio-mime-type
bool is_valid_audio_mime_type(StringView);
// https://w3c.github.io/media-capabilities/#valid-video-mime-type
bool is_valid_video_mime_type(StringView);

}
