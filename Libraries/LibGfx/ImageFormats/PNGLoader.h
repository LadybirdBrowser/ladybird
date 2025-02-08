/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {

struct PNGLoadingContext;

class PNGImageDecoderPlugin final : public ImageDecoderPlugin {
public:
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    virtual ~PNGImageDecoderPlugin() override;

    virtual IntSize size() override;

    virtual bool is_animated() override;
    virtual size_t loop_count() override;
    virtual size_t frame_count() override;
    virtual size_t first_animated_frame_index() override;
    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;
    virtual Optional<Metadata const&> metadata() override;
    virtual ErrorOr<Optional<Media::CodingIndependentCodePoints>> cicp() override;
    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() override;

private:
    explicit PNGImageDecoderPlugin(ReadonlyBytes);

    ErrorOr<void> initialize();

    OwnPtr<PNGLoadingContext> m_context;
};

}
