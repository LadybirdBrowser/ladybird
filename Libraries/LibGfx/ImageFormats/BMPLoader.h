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
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> create_as_included_in_ico(Badge<ICOImageDecoderPlugin>, ReadonlyBytes);

    virtual ~BMPImageDecoderPlugin() override;

    virtual IntSize size() override;

    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;
    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() override;

private:
    enum class IncludedInICO : u8 {
        No,
        Yes,
    };

    explicit BMPImageDecoderPlugin(NonnullOwnPtr<BMPLoadingContext>);
    static ErrorOr<NonnullOwnPtr<BMPImageDecoderPlugin>> create_impl(ReadonlyBytes, IncludedInICO);

    NonnullOwnPtr<BMPLoadingContext> m_context;
};

}
