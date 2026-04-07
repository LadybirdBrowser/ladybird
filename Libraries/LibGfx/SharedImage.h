/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
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

private:
#ifdef AK_OS_MACOS
    explicit SharedImage(Core::MachPort&&);
    Core::MachPort m_port;
#else
    explicit SharedImage(ShareableBitmap);
    ShareableBitmap m_shareable_bitmap;
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
