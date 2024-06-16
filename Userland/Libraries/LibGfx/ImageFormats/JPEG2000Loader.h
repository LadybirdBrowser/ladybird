/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {

namespace JPEG2000 {

struct TagTreeNode;
class TagTree {
public:
    TagTree(TagTree&&);
    ~TagTree();

    static ErrorOr<TagTree> create(u32 x_count, u32 y_count);

    ErrorOr<u32> read_value(u32 x, u32 y, Function<ErrorOr<bool>()> const& read_bit, Optional<u32> stop_at = {}) const;

private:
    TagTree(NonnullOwnPtr<TagTreeNode>);

    NonnullOwnPtr<TagTreeNode> m_root;
};

}

struct JPEG2000LoadingContext;

class JPEG2000ImageDecoderPlugin : public ImageDecoderPlugin {
public:
    static bool sniff(ReadonlyBytes);
    static ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> create(ReadonlyBytes);

    virtual ~JPEG2000ImageDecoderPlugin() override;

    virtual IntSize size() override;

    virtual ErrorOr<ImageFrameDescriptor> frame(size_t index, Optional<IntSize> ideal_size = {}) override;

    virtual ErrorOr<Optional<ReadonlyBytes>> icc_data() override;

private:
    JPEG2000ImageDecoderPlugin();

    OwnPtr<JPEG2000LoadingContext> m_context;
};

}
