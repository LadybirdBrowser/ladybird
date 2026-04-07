/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Variant.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

#ifdef AK_OS_MACOS
#    include <LibCore/MachPort.h>
#endif

namespace Gfx {

class SharedImageBuffer;

#ifndef AK_OS_MACOS
struct LinuxDmaBufHandle {
    BitmapFormat bitmap_format;
    AlphaType alpha_type;
    IntSize size;
    u32 drm_format;
    size_t pitch;
    u64 modifier;
    IPC::File file;
};
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
struct VulkanImage;
SharedImage duplicate_shared_image(VulkanImage const&);
LinuxDmaBufHandle duplicate_linux_dmabuf_handle(VulkanImage const&);
#endif

class SharedImage {
    AK_MAKE_NONCOPYABLE(SharedImage);

public:
    SharedImage(SharedImage&&) = default;
    SharedImage& operator=(SharedImage&&) = default;
    ~SharedImage() = default;

#ifdef AK_OS_MACOS
    explicit SharedImage(Core::MachPort&&);
#else
    explicit SharedImage(ShareableBitmap);
    explicit SharedImage(LinuxDmaBufHandle&&);
#endif

private:
#ifdef AK_OS_MACOS
    Core::MachPort m_port;
#else
    Variant<ShareableBitmap, LinuxDmaBufHandle> m_data;
#endif

    friend class SharedImageBuffer;

    template<typename U>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

#ifndef AK_OS_MACOS
template<>
ErrorOr<void> encode(Encoder&, Gfx::LinuxDmaBufHandle const&);

template<>
ErrorOr<Gfx::LinuxDmaBufHandle> decode(Decoder&);
#endif

template<>
ErrorOr<void> encode(Encoder&, Gfx::SharedImage const&);

template<>
ErrorOr<Gfx::SharedImage> decode(Decoder&);

}
