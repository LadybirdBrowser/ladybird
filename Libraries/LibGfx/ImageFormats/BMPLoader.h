/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {

struct BMPLoadingContext;
class ICOImageDecoderPlugin;

class BMPImageDecoderPlugin final : public ImageDecoderPlugin {
public:
    static bool sniff(NonnullRefPtr<ImageDecoderStream> stream);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(NonnullRefPtr<ImageDecoderStream> stream);
    static ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> create_as_included_in_ico(Badge<ICOImageDecoderPlugin>, NonnullRefPtr<ImageDecoderStream> stream);

    enum class IncludedInICO {
        Yes,
        No,
    };

    virtual ~BMPImageDecoderPlugin() override;

    virtual IntSize size() override;

    bool sniff_dib();
    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;
    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() override;

private:
    BMPImageDecoderPlugin(NonnullRefPtr<ImageDecoderStream> stream, IncludedInICO included_in_ico = IncludedInICO::No);
    static ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> create_impl(NonnullRefPtr<ImageDecoderStream> stream, IncludedInICO);

    OwnPtr<BMPLoadingContext> m_context;
    Optional<ByteBuffer> m_icc_data;
};

}
