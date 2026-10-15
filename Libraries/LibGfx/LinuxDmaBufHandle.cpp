/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/LinuxDmaBufHandle.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::LinuxDmaBufHandle const& dma_buf)
{
    TRY(encoder.encode(dma_buf.size));
    TRY(encoder.encode(dma_buf.drm_format));
    TRY(encoder.encode(dma_buf.modifier));
    TRY(encoder.encode(dma_buf.planes.size()));
    for (auto const& plane : dma_buf.planes) {
        TRY(encoder.encode(plane.object_index));
        TRY(encoder.encode(plane.offset));
        TRY(encoder.encode(plane.pitch));
    }
    TRY(encoder.encode(dma_buf.objects.size()));
    for (auto const& object : dma_buf.objects)
        TRY(encoder.encode(TRY(IPC::File::clone_fd(object.fd()))));
    return {};
}

template<>
ErrorOr<Gfx::LinuxDmaBufHandle> decode(Decoder& decoder)
{
    Gfx::LinuxDmaBufHandle dma_buf;
    dma_buf.size = TRY(decoder.decode<Gfx::IntSize>());
    dma_buf.drm_format = TRY(decoder.decode<u32>());
    dma_buf.modifier = TRY(decoder.decode<u64>());

    auto plane_count = TRY(decoder.decode<size_t>());
    if (plane_count == 0 || plane_count > Gfx::LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT)
        return Error::from_string_literal("dma-buf image has an unusable number of planes");
    for (size_t plane_index = 0; plane_index < plane_count; plane_index++) {
        Gfx::LinuxDmaBufPlane plane;
        plane.object_index = TRY(decoder.decode<u32>());
        plane.offset = TRY(decoder.decode<u32>());
        plane.pitch = TRY(decoder.decode<u32>());
        dma_buf.planes.unchecked_append(plane);
    }

    auto object_count = TRY(decoder.decode<size_t>());
    if (object_count == 0 || object_count > Gfx::LinuxDmaBufHandle::MAXIMUM_PLANE_COUNT)
        return Error::from_string_literal("dma-buf image has an unusable number of objects");
    for (size_t object_index = 0; object_index < object_count; object_index++)
        dma_buf.objects.unchecked_append(TRY(decoder.decode<IPC::File>()));

    for (auto const& plane : dma_buf.planes) {
        if (plane.object_index >= dma_buf.objects.size())
            return Error::from_string_literal("dma-buf image plane refers to an object it does not have");
    }
    return dma_buf;
}

}
