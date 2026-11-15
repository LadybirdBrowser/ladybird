/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/RegisteredImageFrames.h>

namespace Compositing {

void RegisteredImageFrames::register_image_frame(DisplayListImageFrameResource resource)
{
    m_image_frames_by_id.set(resource.id.value(), move(resource.frame));
}

void RegisteredImageFrames::unregister_image_frame(ImageFrameResourceId id)
{
    m_image_frames_by_id.remove(id.value());
}

Gfx::DecodedImageFrame const* RegisteredImageFrames::image_frame(ImageFrameResourceId id) const
{
    auto image_frame = m_image_frames_by_id.find(id.value());
    if (image_frame == m_image_frames_by_id.end())
        return nullptr;
    return &image_frame->value;
}

}
