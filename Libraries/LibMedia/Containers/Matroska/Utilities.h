/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/CodecID.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Containers/Matroska/Document.h>

namespace Media::Matroska {

constexpr bool supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    if (container_id == ContainerID::WebM) {
        switch (codec_id) {
        case CodecID::VP8:
        case CodecID::VP9:
        case CodecID::AV1:
        case CodecID::Vorbis:
        case CodecID::Opus:
            return true;
        default:
            return false;
        }
    }

    if (container_id != ContainerID::Matroska)
        return false;

    switch (codec_id) {
    case CodecID::VP8:
    case CodecID::VP9:
    case CodecID::H264:
    case CodecID::H265:
    case CodecID::MP3:
    case CodecID::AAC:
    case CodecID::AV1:
    case CodecID::Theora:
    case CodecID::Vorbis:
    case CodecID::Opus:
    case CodecID::FLAC:
    case CodecID::U8:
    case CodecID::S16LE:
    case CodecID::S24LE:
    case CodecID::S32LE:
    case CodecID::F32LE:
        return true;
    default:
        return false;
    }
}

}
