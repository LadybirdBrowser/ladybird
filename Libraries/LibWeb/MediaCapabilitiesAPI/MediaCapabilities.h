/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibWeb/Bindings/MediaCapabilities.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::MediaCapabilitiesAPI {

using ColorGamut = Bindings::ColorGamut;
using HdrMetadataType = Bindings::HdrMetadataType;
using AudioConfiguration = Bindings::AudioConfiguration;
using KeySystemTrackConfiguration = Bindings::KeySystemTrackConfiguration;
using MediaCapabilitiesKeySystemConfiguration = Bindings::MediaCapabilitiesKeySystemConfiguration;
using MediaCapabilitiesDecodingInfo = Bindings::MediaCapabilitiesDecodingInfo;
using MediaDecodingConfiguration = Bindings::MediaDecodingConfiguration;
using MediaDecodingType = Bindings::MediaDecodingType;
using MediaKeysRequirement = Bindings::MediaKeysRequirement;
using TransferFunction = Bindings::TransferFunction;
using VideoConfiguration = Bindings::VideoConfiguration;

bool is_valid_video_configuration(VideoConfiguration const&);

bool is_valid_audio_configuration(AudioConfiguration const&);

bool is_valid_media_decoding_configuration(MediaDecodingConfiguration const&);

// https://w3c.github.io/media-capabilities/#media-capabilities-interface
class MediaCapabilities final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaCapabilities, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaCapabilities);

public:
    static GC::Ref<MediaCapabilities> create();
    virtual ~MediaCapabilities() override = default;

    void decoding_info(MediaDecodingConfiguration const&, GC::Ref<WebIDL::Promise>);

private:
    MediaCapabilities();
};

// https://w3c.github.io/media-capabilities/#create-a-mediacapabilitiesdecodinginfo
MediaCapabilitiesDecodingInfo create_a_media_capabilities_decoding_info(MediaDecodingConfiguration);

Optional<Media::DecoderCapabilities> media_decoding_capabilities(MediaDecodingConfiguration const&);

// https://w3c.github.io/media-capabilities/#valid-audio-mime-type
bool is_valid_audio_mime_type(Utf16View);
// https://w3c.github.io/media-capabilities/#valid-video-mime-type
bool is_valid_video_mime_type(Utf16View);

}
