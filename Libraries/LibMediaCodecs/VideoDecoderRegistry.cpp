/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>

namespace Media {

static Vector<VideoDecoderRegistration>& installed_video_decoders()
{
    static NeverDestroyed<Vector<VideoDecoderRegistration>> registrations;
    return *registrations;
}

void install_video_decoders(Vector<ReadonlySpan<VideoDecoderRegistration>> sets)
{
    auto& registrations = installed_video_decoders();
    registrations.clear();
    for (auto set : sets)
        registrations.append(set.data(), set.size());
}

Optional<DecoderCapabilities> video_decoder_capabilities(ParsedCodec const& codec)
{
    for (auto const& registration : installed_video_decoders()) {
        auto capabilities = registration.capabilities(codec);
        if (capabilities.has_value())
            return capabilities;
    }
    return {};
}

VideoDecoderSelection select_video_decoder(ParsedCodec const& codec, VideoDecoderSelection after)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video)
        return {};

    auto const& registrations = installed_video_decoders();
    auto registration_count = static_cast<i32>(registrations.size());
    for (auto index = after.registration_index() + 1; index < registration_count; index++) {
        if (registrations[index].capabilities(codec).has_value())
            return VideoDecoderSelection { index };
    }
    return {};
}

DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_video_decoder(VideoDecoderSelection selection, CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto const& registrations = installed_video_decoders();
    VERIFY(selection.has_value());
    VERIFY(selection.registration_index() < static_cast<i32>(registrations.size()));
    return registrations[selection.registration_index()].create(codec_id, codec_initialization_data);
}

}
