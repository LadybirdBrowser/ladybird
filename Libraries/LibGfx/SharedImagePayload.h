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

class SharedImageInstance;

class SharedImagePayload {
    AK_MAKE_NONCOPYABLE(SharedImagePayload);

public:
    SharedImagePayload(SharedImagePayload&&) = default;
    SharedImagePayload& operator=(SharedImagePayload&&) = default;
    ~SharedImagePayload() = default;

#ifndef AK_OS_MACOS
    explicit SharedImagePayload(ShareableBitmap);
    explicit SharedImagePayload(LinuxDmaBufBackingStore);

    bool is_shareable_bitmap() const { return m_shareable_bitmap.has_value(); }
    ShareableBitmap release_shareable_bitmap() { return m_shareable_bitmap.release_value(); }

    LinuxDmaBufBackingStore release_linux_dma_buf() { return m_linux_dma_buf.release_value(); }
#endif

private:
#ifdef AK_OS_MACOS
    explicit SharedImagePayload(Core::MachPort&&);
    Core::MachPort m_port;
#else
    Optional<ShareableBitmap> m_shareable_bitmap;
    Optional<LinuxDmaBufBackingStore> m_linux_dma_buf;
#endif

    friend class SharedImageInstance;

    template<typename U>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::SharedImagePayload const&);

template<>
ErrorOr<Gfx::SharedImagePayload> decode(Decoder&);

}
