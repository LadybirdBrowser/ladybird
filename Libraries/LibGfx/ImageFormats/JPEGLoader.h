/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {

struct JPEGLoadingContext;

class JPEGImageDecoderPlugin : public ImageDecoderPlugin {
public:
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    virtual ~JPEGImageDecoderPlugin() override;
    virtual IntSize size() override;

    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;

    virtual Optional<Metadata const&> metadata() override;

    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() override;

    virtual NaturalFrameFormat natural_frame_format() const override;
    virtual ErrorOr<NonnullRefPtr<CMYKBitmap>> cmyk_frame() override;

private:
    explicit JPEGImageDecoderPlugin(NonnullOwnPtr<JPEGLoadingContext>);

    NonnullOwnPtr<JPEGLoadingContext> m_context;
};

}
