/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <LibGfx/LinuxDmaBuf.h>
#include <LibIPC/Forward.h>

#ifdef AK_OS_MACOS
#    include <LibCore/MachPort.h>
#else
#    include <LibGfx/ShareableBitmap.h>
#endif

namespace Gfx {

class SharedImageBuffer;

class SharedImage {
    AK_MAKE_NONCOPYABLE(SharedImage);

public:
    SharedImage(SharedImage&&) = default;
    SharedImage& operator=(SharedImage&&) = default;
    ~SharedImage() = default;

#ifndef AK_OS_MACOS
    explicit SharedImage(ShareableBitmap);
    explicit SharedImage(LinuxDmaBufBackingStore);

    bool is_shareable_bitmap() const { return m_shareable_bitmap.has_value(); }
    ShareableBitmap release_shareable_bitmap() { return m_shareable_bitmap.release_value(); }

    LinuxDmaBufBackingStore release_linux_dma_buf() { return m_linux_dma_buf.release_value(); }
#endif

private:
#ifdef AK_OS_MACOS
    explicit SharedImage(Core::MachPort&&);
    Core::MachPort m_port;
#else
    Optional<ShareableBitmap> m_shareable_bitmap;
    Optional<LinuxDmaBufBackingStore> m_linux_dma_buf;
#endif

    friend class SharedImageBuffer;

    template<typename U>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::SharedImage const&);

template<>
ErrorOr<Gfx::SharedImage> decode(Decoder&);

}
