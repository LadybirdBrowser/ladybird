/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

#ifdef AK_OS_MACOS
#    include <LibCore/MachPort.h>
#endif

namespace Web::Gamepad {

struct SharedGamepadStateBuffer;

class WEBCOMMON_API GamepadStateBuffer {
    AK_MAKE_NONCOPYABLE(GamepadStateBuffer);

public:
    GamepadStateBuffer() = default;
    GamepadStateBuffer(GamepadStateBuffer&&);
    GamepadStateBuffer& operator=(GamepadStateBuffer&&);
    ~GamepadStateBuffer();

    static ErrorOr<GamepadStateBuffer> create(Core::AnonymousBuffer const&);

    bool is_valid() const;
    SharedGamepadStateBuffer const* data() const;

private:
#ifdef AK_OS_MACOS
    GamepadStateBuffer(Core::MachPort memory_entry, SharedGamepadStateBuffer const* data);

    void unmap();

    Core::MachPort m_memory_entry;
    SharedGamepadStateBuffer const* m_data { nullptr };
#else
    explicit GamepadStateBuffer(Core::AnonymousBuffer);

    Core::AnonymousBuffer m_buffer;
#endif

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);

    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadStateBuffer const&);

template<>
WEBCOMMON_API ErrorOr<Web::Gamepad::GamepadStateBuffer> decode(Decoder&);

}
