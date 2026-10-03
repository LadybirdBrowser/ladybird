/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Gamepad/GamepadSharedState.h>
#include <LibWebCommon/Gamepad/GamepadStateBuffer.h>

#ifdef AK_OS_MACOS
#    include <mach/mach_vm.h>
#endif

namespace Web::Gamepad {

#ifdef AK_OS_MACOS
GamepadStateBuffer::GamepadStateBuffer(Core::MachPort memory_entry, SharedGamepadStateBuffer const* data)
    : m_memory_entry(move(memory_entry))
    , m_data(data)
{
}

GamepadStateBuffer::GamepadStateBuffer(GamepadStateBuffer&& other)
    : m_memory_entry(move(other.m_memory_entry))
    , m_data(exchange(other.m_data, nullptr))
{
}

GamepadStateBuffer& GamepadStateBuffer::operator=(GamepadStateBuffer&& other)
{
    if (this != &other) {
        unmap();
        m_memory_entry = move(other.m_memory_entry);
        m_data = exchange(other.m_data, nullptr);
    }
    return *this;
}

GamepadStateBuffer::~GamepadStateBuffer()
{
    unmap();
}

void GamepadStateBuffer::unmap()
{
    if (!m_data)
        return;
    auto result = mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(m_data), sizeof(SharedGamepadStateBuffer));
    VERIFY(result == KERN_SUCCESS);
    m_data = nullptr;
}

bool GamepadStateBuffer::is_valid() const
{
    return m_memory_entry.port() != MACH_PORT_NULL;
}

SharedGamepadStateBuffer const* GamepadStateBuffer::data() const
{
    return m_data;
}
#else
GamepadStateBuffer::GamepadStateBuffer(Core::AnonymousBuffer buffer)
    : m_buffer(move(buffer))
{
}

GamepadStateBuffer::GamepadStateBuffer(GamepadStateBuffer&&) = default;
GamepadStateBuffer& GamepadStateBuffer::operator=(GamepadStateBuffer&&) = default;
GamepadStateBuffer::~GamepadStateBuffer() = default;

bool GamepadStateBuffer::is_valid() const
{
    return m_buffer.is_valid();
}

SharedGamepadStateBuffer const* GamepadStateBuffer::data() const
{
    return reinterpret_cast<SharedGamepadStateBuffer const*>(m_buffer.data<void>());
}
#endif

ErrorOr<GamepadStateBuffer> GamepadStateBuffer::create(Core::AnonymousBuffer const& buffer)
{
    if (buffer.size() != sizeof(SharedGamepadStateBuffer))
        return Error::from_string_literal("Invalid gamepad state buffer size");
    TRY(buffer.validate_backing_size());

#ifdef AK_OS_MACOS
    memory_object_size_t size = sizeof(SharedGamepadStateBuffer);
    mach_port_t port = MACH_PORT_NULL;
    auto result = mach_make_memory_entry_64(mach_task_self(), &size,
        reinterpret_cast<memory_object_offset_t>(buffer.data<void>()), VM_PROT_READ, &port, MACH_PORT_NULL);
    if (result != KERN_SUCCESS)
        return Core::mach_error_to_error(result);
    auto memory_entry = Core::MachPort::adopt_right(port, Core::MachPort::PortRight::Send);
    if (size < sizeof(SharedGamepadStateBuffer))
        return Error::from_string_literal("Gamepad memory entry is too small");
    return GamepadStateBuffer { move(memory_entry), nullptr };
#else
    return GamepadStateBuffer { buffer };
#endif
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Gamepad::GamepadStateBuffer const& buffer)
{
#ifdef AK_OS_MACOS
    TRY(encoder.encode(buffer.is_valid()));
    if (!buffer.is_valid())
        return {};
    return encoder.append_attachment(Attachment::from_mach_port(TRY(buffer.m_memory_entry.copy_send_right()), Core::MachPort::MessageRight::MoveSend));
#else
    return encoder.encode(buffer.m_buffer);
#endif
}

template<>
ErrorOr<Web::Gamepad::GamepadStateBuffer> decode(Decoder& decoder)
{
    using Web::Gamepad::GamepadStateBuffer;
#ifdef AK_OS_MACOS
    using Web::Gamepad::SharedGamepadStateBuffer;

    if (!TRY(decoder.decode<bool>()))
        return GamepadStateBuffer {};
    if (decoder.attachments().is_empty())
        return Error::from_string_literal("Missing gamepad memory entry");
    auto attachment = decoder.attachments().dequeue();
    if (attachment.message_right() != Core::MachPort::MessageRight::MoveSend)
        return Error::from_string_literal("Invalid gamepad memory entry right");
    auto memory_entry = attachment.release_mach_port();

    mach_vm_address_t address = 0;
    auto result = mach_vm_map(mach_task_self(), &address, sizeof(SharedGamepadStateBuffer), 0, VM_FLAGS_ANYWHERE,
        memory_entry.port(), 0, false, VM_PROT_READ, VM_PROT_READ, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS)
        return Core::mach_error_to_error(result);
    return GamepadStateBuffer { move(memory_entry), reinterpret_cast<SharedGamepadStateBuffer const*>(address) };
#else
    auto buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    if (!buffer.is_valid())
        return GamepadStateBuffer {};
    return GamepadStateBuffer::create(buffer);
#endif
}

}
