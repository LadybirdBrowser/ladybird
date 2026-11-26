/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/GBMAllocation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef AK_OS_LINUX
#    include <sys/sysmacros.h>
#endif
#include <unistd.h>

namespace Gfx {

namespace {

// NB: libgbm is loaded at runtime, so that a system without it falls back to the allocations that need none. These
//     are the parts of its stable ABI that we use, from <gbm.h>.
struct GBMDevice;
struct GBMBufferObject;

constexpr u32 GBM_BO_USE_SCANOUT = 1 << 0;
constexpr u32 GBM_BO_USE_RENDERING = 1 << 2;

struct GBMFunctions {
    GBMDevice* (*gbm_create_device)(int fd);
    GBMBufferObject* (*gbm_bo_create_with_modifiers2)(GBMDevice*, u32 width, u32 height, u32 format, u64 const* modifiers, unsigned count, u32 flags);
    void (*gbm_bo_destroy)(GBMBufferObject*);
    int (*gbm_bo_get_plane_count)(GBMBufferObject*);
    int (*gbm_bo_get_fd)(GBMBufferObject*);
    u32 (*gbm_bo_get_stride_for_plane)(GBMBufferObject*, int plane);
    u32 (*gbm_bo_get_offset)(GBMBufferObject*, int plane);
    u64 (*gbm_bo_get_modifier)(GBMBufferObject*);
};

struct OpenGBMDevice {
    GBMFunctions const& functions;
    dev_t render_node;
    GBMDevice* device;
};

}

static ErrorOr<GBMFunctions> load_gbm_functions()
{
    auto* library = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return Error::from_string_literal("libgbm is unavailable");

    GBMFunctions functions {};
#define RESOLVE_FUNCTION(name)                                                          \
    functions.name = reinterpret_cast<decltype(functions.name)>(dlsym(library, #name)); \
    if (!functions.name)                                                                \
        return Error::from_string_literal("libgbm lacks " #name);
    RESOLVE_FUNCTION(gbm_create_device)
    RESOLVE_FUNCTION(gbm_bo_create_with_modifiers2)
    RESOLVE_FUNCTION(gbm_bo_destroy)
    RESOLVE_FUNCTION(gbm_bo_get_plane_count)
    RESOLVE_FUNCTION(gbm_bo_get_fd)
    RESOLVE_FUNCTION(gbm_bo_get_stride_for_plane)
    RESOLVE_FUNCTION(gbm_bo_get_offset)
    RESOLVE_FUNCTION(gbm_bo_get_modifier)
#undef RESOLVE_FUNCTION
    return functions;
}

static ErrorOr<OpenGBMDevice> open_gbm_device(dev_t render_node)
{
    static auto const functions = load_gbm_functions();
    if (functions.is_error())
        return Error::copy(functions.error());

    auto path = ByteString::formatted("/dev/dri/renderD{}", minor(render_node));
    int fd = open(path.characters(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    struct stat render_node_stat {};
    if (fstat(fd, &render_node_stat) < 0 || render_node_stat.st_rdev != render_node) {
        close(fd);
        return Error::from_string_literal("The render node is not at its usual path");
    }

    // NB: The device keeps using the descriptor for as long as it lives, which is as long as the process.
    auto* device = functions.value().gbm_create_device(fd);
    if (!device) {
        close(fd);
        return Error::from_string_literal("GBM cannot open the render node");
    }
    return OpenGBMDevice { .functions = functions.value(), .render_node = render_node, .device = device };
}

ErrorOr<LinuxDmaBufHandle> allocate_scanout_capable_linux_dmabuf(dev_t render_node, IntSize size, u32 drm_format, ReadonlySpan<u64> modifiers)
{
    // NB: A process allocates from the one GPU it renders on.
    static auto const gbm = open_gbm_device(render_node);
    if (gbm.is_error())
        return Error::copy(gbm.error());
    if (gbm.value().render_node != render_node)
        return Error::from_string_literal("GBM is open on another render node");
    auto const& functions = gbm.value().functions;

    auto* buffer_object = functions.gbm_bo_create_with_modifiers2(gbm.value().device, size.width(), size.height(), drm_format,
        modifiers.data(), modifiers.size(), GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!buffer_object)
        return Error::from_string_literal("GBM cannot allocate a scanout buffer in any of the layouts");
    // NB: The dma-buf keeps the memory alive once the buffer object is destroyed.
    ScopeGuard destroy_buffer_object = [&] { functions.gbm_bo_destroy(buffer_object); };

    if (functions.gbm_bo_get_plane_count(buffer_object) != 1)
        return Error::from_string_literal("GBM allocated more than one plane");
    int fd = functions.gbm_bo_get_fd(buffer_object);
    if (fd < 0)
        return Error::from_string_literal("GBM cannot export the buffer as a dma-buf");

    LinuxDmaBufHandle dmabuf;
    dmabuf.size = size;
    dmabuf.drm_format = drm_format;
    dmabuf.modifier = functions.gbm_bo_get_modifier(buffer_object);
    dmabuf.objects.append(IPC::File::adopt_fd(fd));
    dmabuf.planes.append({ .object_index = 0, .offset = functions.gbm_bo_get_offset(buffer_object, 0), .pitch = functions.gbm_bo_get_stride_for_plane(buffer_object, 0) });
    return dmabuf;
}

}
