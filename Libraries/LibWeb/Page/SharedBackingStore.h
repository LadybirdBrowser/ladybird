/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>

#if defined(AK_OS_MACOS)
#    include <LibCore/MachPort.h>
#else
#    include <LibGfx/ShareableBitmap.h>
#endif

namespace Web {

class WEB_API SharedBackingStore {
    AK_MAKE_NONCOPYABLE(SharedBackingStore);

public:
#if defined(AK_OS_MACOS)
    explicit SharedBackingStore(Core::MachPort&&);
    Core::MachPort release_iosurface_port() { return move(m_port); }
#else
    explicit SharedBackingStore(Gfx::ShareableBitmap);
    Gfx::ShareableBitmap const& bitmap() const { return m_bitmap; }
#endif
    SharedBackingStore(SharedBackingStore&&) = default;
    SharedBackingStore& operator=(SharedBackingStore&&) = default;
    ~SharedBackingStore() = default;

private:
#if defined(AK_OS_MACOS)
    Core::MachPort m_port;
#else
    Gfx::ShareableBitmap m_bitmap;
#endif

    template<typename U>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::SharedBackingStore const&);

template<>
WEB_API ErrorOr<Web::SharedBackingStore> decode(Decoder&);

}
