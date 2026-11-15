/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Export.h>
#include <LibGfx/DecodedImageFrame.h>

namespace Compositing {

class COMPOSITING_API RegisteredImageFrames {
public:
    void register_image_frame(DisplayListImageFrameResource);
    void unregister_image_frame(ImageFrameResourceId);

    Gfx::DecodedImageFrame const* image_frame(ImageFrameResourceId) const;

private:
    HashMap<u64, Gfx::DecodedImageFrame> m_image_frames_by_id;
};

}
