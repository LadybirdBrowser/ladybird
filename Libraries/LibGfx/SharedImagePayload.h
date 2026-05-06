/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/BitmapInfo.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

#ifdef AK_OS_MACOS
#    include <LibCore/MachPort.h>
#endif

namespace Gfx {

class SharedImage;
class DecodedImageFrame;
struct VulkanImage;

struct LinuxDmaBufPayload {
    u32 drm_format { 0 };
    u32 stride { 0 };
    u32 offset { 0 };
    IPC::File file;
};

class SharedImagePayload {
    AK_MAKE_NONCOPYABLE(SharedImagePayload);

public:
    SharedImagePayload(SharedImagePayload&&) = default;
    SharedImagePayload& operator=(SharedImagePayload&&) = default;
    ~SharedImagePayload() = default;

#ifdef AK_OS_MACOS
    SharedImagePayload(BitmapInfo, ShareableBitmap, ColorSpace = {});
    SharedImagePayload(BitmapInfo, Core::MachPort&&, ColorSpace = {});
    bool is_shareable_bitmap() const { return m_data.has<ShareableBitmap>(); }
    ShareableBitmap* shareable_bitmap() { return m_data.has<ShareableBitmap>() ? &m_data.get<ShareableBitmap>() : nullptr; }
    ShareableBitmap const* shareable_bitmap() const { return m_data.has<ShareableBitmap>() ? &m_data.get<ShareableBitmap>() : nullptr; }
    Core::MachPort* mach_port() { return m_data.has<Core::MachPort>() ? &m_data.get<Core::MachPort>() : nullptr; }
    Core::MachPort const* mach_port() const { return m_data.has<Core::MachPort>() ? &m_data.get<Core::MachPort>() : nullptr; }
#else
    SharedImagePayload(BitmapInfo, ShareableBitmap, ColorSpace = {});
    SharedImagePayload(BitmapInfo, LinuxDmaBufPayload&&, ColorSpace = {});
    bool is_shareable_bitmap() const { return m_data.has<ShareableBitmap>(); }
    ShareableBitmap* shareable_bitmap() { return m_data.has<ShareableBitmap>() ? &m_data.get<ShareableBitmap>() : nullptr; }
    ShareableBitmap const* shareable_bitmap() const { return m_data.has<ShareableBitmap>() ? &m_data.get<ShareableBitmap>() : nullptr; }
    LinuxDmaBufPayload* linux_dma_buf_payload() { return m_data.has<LinuxDmaBufPayload>() ? &m_data.get<LinuxDmaBufPayload>() : nullptr; }
    LinuxDmaBufPayload const* linux_dma_buf_payload() const { return m_data.has<LinuxDmaBufPayload>() ? &m_data.get<LinuxDmaBufPayload>() : nullptr; }
#endif

    BitmapInfo const& info() const { return m_info; }
    ColorSpace const& color_space() const { return m_color_space; }
    u32 row_bytes() const;

private:
    BitmapInfo m_info;
    ColorSpace m_color_space;

#ifdef AK_OS_MACOS
    Variant<ShareableBitmap, Core::MachPort> m_data;
#else
    Variant<ShareableBitmap, LinuxDmaBufPayload> m_data;
#endif

    friend class SharedImage;

    template<typename U>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> IPC::decode(IPC::Decoder&);
};

ErrorOr<void> upload_decoded_image_frame_to_shared_image(DecodedImageFrame const&, SharedImagePayload&);
ErrorOr<void> upload_decoded_image_frame_to_bitmap(DecodedImageFrame const&, Bitmap&, BitmapInfo const&, ColorSpace const&);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::LinuxDmaBufPayload const&);

template<>
ErrorOr<Gfx::LinuxDmaBufPayload> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapInfo const&);

template<>
ErrorOr<Gfx::BitmapInfo> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::SharedImagePayload const&);

template<>
ErrorOr<Gfx::SharedImagePayload> decode(Decoder&);

}
