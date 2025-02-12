/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>

namespace Web::MediaCapabilitiesAPI {

// https://w3c.github.io/media-capabilities/#dictdef-videoconfiguration
struct VideoConfiguration {
    String content_type;
    WebIDL::UnsignedLong width;
    WebIDL::UnsignedLong height;
    Optional<WebIDL::UnsignedLongLong> bitrate;
    double framerate;
    Optional<bool> has_alpha_channel;
    Optional<Bindings::HdrMetadataType> hdr_metadata_type;
    Optional<Bindings::ColorGamut> color_gamut;
    Optional<Bindings::TransferFunction> transfer_function;
    Optional<String> scalability_mode;
    Optional<bool> spatial_scalability;

    // https://w3c.github.io/media-capabilities/#valid-video-configuration
    bool is_valid_video_configuration() const;
};

// https://w3c.github.io/media-capabilities/#dictdef-audioconfiguration
struct AudioConfiguration {
    String content_type;
    Optional<String> channels;
    Optional<WebIDL::UnsignedLongLong> bitrate;
    Optional<WebIDL::UnsignedLong> samplerate;
    Optional<bool> spatial_rendering;

    // https://w3c.github.io/media-capabilities/#valid-audio-configuration
    bool is_valid_audio_configuration() const;
};

// https://w3c.github.io/media-capabilities/#dictdef-mediaconfiguration
struct MediaConfiguration {
    Optional<VideoConfiguration> video;
    Optional<AudioConfiguration> audio;

    // https://w3c.github.io/media-capabilities/#valid-mediaconfiguration
    bool is_valid_media_configuration() const;
};

// https://w3c.github.io/media-capabilities/#keysystemtrackconfiguration
struct KeySystemTrackConfiguration {
    String robustness;
    Optional<String> encryption_scheme;
};

// https://w3c.github.io/media-capabilities/#mediacapabilitieskeysystemconfiguration
struct MediaCapabilitiesKeySystemConfiguration {
    String key_system;
    String init_data_type;
    Bindings::MediaKeysRequirement distinctive_identifier;
    Bindings::MediaKeysRequirement persistent_state;
    Optional<Vector<String>> session_types;
    Optional<KeySystemTrackConfiguration> audio;
    Optional<KeySystemTrackConfiguration> video;
};

// https://w3c.github.io/media-capabilities/#dictdef-mediadecodingconfiguration
struct MediaDecodingConfiguration : public MediaConfiguration {
    Bindings::MediaDecodingType type;
    Optional<MediaCapabilitiesKeySystemConfiguration> key_system_configuration;

    // https://w3c.github.io/media-capabilities/#valid-mediadecodingconfiguration
    bool is_valid_media_decoding_configuration() const;
};

// https://w3c.github.io/media-capabilities/#dictdef-mediaencodingconfiguration
struct MediaEncodingConfiguration : public MediaConfiguration {
    Bindings::MediaEncodingType type;
};

// https://w3c.github.io/media-capabilities/#media-capabilities-info
struct MediaCapabilitiesInfo {
    bool supported;
    bool smooth;
    bool power_efficient;
};

// https://w3c.github.io/media-capabilities/#dictdef-mediacapabilitiesdecodinginfo
struct MediaCapabilitiesDecodingInfo : public MediaCapabilitiesInfo {
    MediaDecodingConfiguration configuration;
    Optional<MediaCapabilitiesKeySystemConfiguration> key_system_configuration;

    GC::Ref<JS::Object> to_object(JS::Realm&);
};

struct MediaCapabilitiesEncodingInfo : public MediaCapabilitiesInfo {
    Optional<MediaEncodingConfiguration> configuration;
};

// https://w3c.github.io/media-capabilities/#media-capabilities-interface
class MediaCapabilities final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaCapabilities, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaCapabilities);

public:
    static GC::Ref<MediaCapabilities> create(JS::Realm&);
    virtual ~MediaCapabilities() override = default;

    // https://w3c.github.io/media-capabilities/#dom-mediacapabilities-decodinginfo
    GC::Ref<WebIDL::Promise> decoding_info(MediaDecodingConfiguration const&);

private:
    MediaCapabilities(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    // https://w3c.github.io/media-capabilities/#valid-mediadecodingconfiguration
    bool is_valid_media_decoding_configuration() const;
};

// https://w3c.github.io/media-capabilities/#queue-a-media-capabilities-task
void queue_a_media_capabilities_task(JS::VM& vm, Function<void()>);

// https://w3c.github.io/media-capabilities/#create-a-mediacapabilitiesdecodinginfo
MediaCapabilitiesDecodingInfo create_a_media_capabilities_decoding_info(MediaDecodingConfiguration);

bool is_able_to_decode_media(MediaDecodingConfiguration configuration);

// https://w3c.github.io/media-capabilities/#valid-audio-mime-type
bool is_valid_audio_mime_type(StringView);
// https://w3c.github.io/media-capabilities/#valid-video-mime-type
bool is_valid_video_mime_type(StringView);

}
