/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/Export.h>

namespace Media {

struct MimeTypeView {
    StringView type;
    StringView subtype;
    OrderedHashMap<String, String> const& parameters;
};

enum class MediaSupport : u8 {
    NotSupported,
    Maybe,
    Probably,
};

struct MediaSupportInfo {
    MediaSupport support { MediaSupport::NotSupported };
    DecoderCapabilities capabilities { DecoderCapabilities() };

    bool operator==(MediaSupportInfo const&) const = default;
};

MEDIA_API Optional<CodecID> codec_implied_by_file_container(ContainerMimeType);
MEDIA_API MediaSupportInfo file_media_support(MimeTypeView const&);

}
