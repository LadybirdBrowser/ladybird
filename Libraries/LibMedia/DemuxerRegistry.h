/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

namespace Media {

MEDIA_API bool is_supported_file_container(ContainerMimeType);
MEDIA_API bool is_codec_supported_in_file_container(ContainerMimeType, CodecID);
MEDIA_API DecoderErrorOr<NonnullRefPtr<Demuxer>> create_demuxer(NonnullRefPtr<MediaStream> const&);

}
