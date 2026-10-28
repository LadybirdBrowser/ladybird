/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WaylandGpu.h>
#include <UI/Qt/WaylandRegistry.h>

#include <libdrm/drm_fourcc.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

// NB: The generated listener functions cast the const away from the listeners they pass to libwayland.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#include <linux-dmabuf-v1-client-protocol.h>
#pragma GCC diagnostic pop

namespace Ladybird {

namespace {

struct DmaBufFeedback {
    Optional<dev_t> main_device;
    ReadonlyBytes format_table;
    Optional<dev_t> tranche_device;
    Vector<u64> tranche_modifiers;
    Vector<u64> xrgb8888_modifiers;
};

Optional<dev_t> device_from_array(wl_array const& array)
{
    dev_t device;
    if (array.size != sizeof(device))
        return {};
    memcpy(&device, array.data, sizeof(device));
    return device;
}

}

Optional<WaylandCompositorDevice> wayland_compositor_main_device()
{
    auto registry = WaylandRegistry::create();
    if (!registry)
        return {};

    // The default feedback is new in version 4.
    auto* dmabuf = registry->bind<zwp_linux_dmabuf_v1>(zwp_linux_dmabuf_v1_interface, 4);
    if (!dmabuf)
        return {};

    DmaBufFeedback feedback;
    static zwp_linux_dmabuf_feedback_v1_listener const feedback_listener {
        .done = [](void*, zwp_linux_dmabuf_feedback_v1*) { },
        .format_table = [](void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t size) {
                auto& feedback = *static_cast<DmaBufFeedback*>(data);
                auto* table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                ::close(fd);
                if (table != MAP_FAILED)
                    feedback.format_table = { static_cast<u8 const*>(table), size }; },
        .main_device = [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) { static_cast<DmaBufFeedback*>(data)->main_device = device_from_array(*device); },
        .tranche_done = [](void* data, zwp_linux_dmabuf_feedback_v1*) {
                auto& feedback = *static_cast<DmaBufFeedback*>(data);
                if (feedback.tranche_device.has_value() && feedback.tranche_device == feedback.main_device) {
                    for (auto modifier : feedback.tranche_modifiers) {
                        if (!feedback.xrgb8888_modifiers.contains_slow(modifier))
                            feedback.xrgb8888_modifiers.append(modifier);
                    }
                }
                feedback.tranche_device.clear();
                feedback.tranche_modifiers.clear(); },
        .tranche_target_device = [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) { static_cast<DmaBufFeedback*>(data)->tranche_device = device_from_array(*device); },
        .tranche_formats = [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
                // Each entry of the format table is a 32-bit format, 32 bits of padding and a 64-bit modifier.
                static constexpr size_t entry_size = 16;
                auto& feedback = *static_cast<DmaBufFeedback*>(data);
                ReadonlySpan<u16> entry_indices { static_cast<u16 const*>(indices->data), indices->size / sizeof(u16) };
                for (auto index : entry_indices) {
                    if ((index + 1) * entry_size > feedback.format_table.size())
                        continue;
                    u32 format;
                    u64 modifier;
                    memcpy(&format, feedback.format_table.data() + index * entry_size, sizeof(format));
                    memcpy(&modifier, feedback.format_table.data() + index * entry_size + 8, sizeof(modifier));
                    if (format == DRM_FORMAT_XRGB8888)
                        feedback.tranche_modifiers.append(modifier);
                } },
        .tranche_flags = [](void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) { },
    };
    auto* default_feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(default_feedback, &feedback_listener, &feedback);
    // The compositor sends the whole feedback as soon as it is asked for, so it all arrives within one roundtrip.
    registry->roundtrip();
    zwp_linux_dmabuf_feedback_v1_destroy(default_feedback);
    zwp_linux_dmabuf_v1_destroy(dmabuf);
    if (!feedback.format_table.is_empty())
        munmap(const_cast<u8*>(feedback.format_table.data()), feedback.format_table.size());

    if (!feedback.main_device.has_value())
        return {};
    return WaylandCompositorDevice { .device = *feedback.main_device, .xrgb8888_modifiers = move(feedback.xrgb8888_modifiers) };
}

}
