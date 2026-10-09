/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibMediaCodecs/AudioDecoderRegistry.h>

namespace Media {

static Vector<AudioDecoderRegistration>& installed_audio_decoders()
{
    static NeverDestroyed<Vector<AudioDecoderRegistration>> registrations;
    return *registrations;
}

void install_audio_decoders(Vector<ReadonlySpan<AudioDecoderRegistration>> sets)
{
    auto& registrations = installed_audio_decoders();
    registrations.clear();
    for (auto set : sets)
        registrations.append(set.data(), set.size());
}

Optional<DecoderCapabilities> audio_decoder_capabilities(ParsedCodec const& codec)
{
    for (auto const& registration : installed_audio_decoders()) {
        auto capabilities = registration.capabilities(codec);
        if (capabilities.has_value())
            return capabilities;
    }
    return {};
}

AudioDecoderSelection select_audio_decoder(ParsedCodec const& codec, AudioDecoderSelection after)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Audio)
        return {};

    auto const& registrations = installed_audio_decoders();
    auto registration_count = static_cast<i32>(registrations.size());
    for (auto index = after.registration_index() + 1; index < registration_count; index++) {
        if (registrations[index].capabilities(codec).has_value())
            return AudioDecoderSelection { index };
    }
    return {};
}

DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audio_decoder(AudioDecoderSelection selection, CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
{
    auto const& registrations = installed_audio_decoders();
    VERIFY(selection.has_value());
    VERIFY(selection.registration_index() < static_cast<i32>(registrations.size()));
    return registrations[selection.registration_index()].create(codec_id, sample_specification, codec_initialization_data);
}

}
