/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {

struct GIFLoadingContext;

// Specified at: https://www.w3.org/Graphics/GIF/spec-gif89a.txt

class GIFImageDecoderPlugin final : public ImageDecoderPlugin {
public:
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    virtual ~GIFImageDecoderPlugin() override;

    virtual IntSize size() override;

    virtual bool is_animated() override;
    virtual size_t loop_count() override;
    virtual size_t frame_count() override;
    virtual size_t first_animated_frame_index() override;
    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;

private:
    GIFImageDecoderPlugin(FixedMemoryStream);

    OwnPtr<GIFLoadingContext> m_context;
};

}
