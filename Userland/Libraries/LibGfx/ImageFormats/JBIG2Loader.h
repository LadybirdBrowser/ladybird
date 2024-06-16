/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <AK/OwnPtr.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {

struct JBIG2LoadingContext;

class JBIG2ImageDecoderPlugin : public ImageDecoderPlugin {
public:
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    virtual ~JBIG2ImageDecoderPlugin() override;

    virtual IntSize size() override;

    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;

    static ErrorOr<ByteBuffer> decode_embedded(Vector<ReadonlyBytes>);

private:
    JBIG2ImageDecoderPlugin();

    OwnPtr<JBIG2LoadingContext> m_context;
};

}
