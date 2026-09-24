/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>

namespace Media::FFmpeg {

// A libavcodec loaded from the system, for the codecs the bundled FFmpeg is built without.
class MEDIA_API SystemFFmpeg {
public:
    AK_ALLOC_WITH_KMALLOC;

    static SystemFFmpeg const* the();
    static DecoderErrorOr<NonnullOwnPtr<SystemFFmpeg>> try_load(ByteString const& library_path);

    SystemFFmpeg(void* library_handle, NonnullOwnPtr<FFmpegFunctions>, unsigned major);
    ~SystemFFmpeg();

    FFmpegFunctions const& functions() const { return *m_functions; }
    unsigned major() const { return m_major; }
    bool has_decoder(CodecID) const;
    bool provides_decoder_missing_from_bundle() const;

private:
    void* m_library_handle;
    NonnullOwnPtr<FFmpegFunctions> m_functions;
    unsigned m_major;
};

}
