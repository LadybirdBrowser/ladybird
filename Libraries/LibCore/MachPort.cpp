/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <LibCore/MachPort.h>

#if defined(AK_OS_GNU_HURD)
extern "C" {
#    include <mach_error.h>
}
// This is in <mach/mach_error.h> on Darwin, and doesn't seem to be required.
#endif

#if defined(AK_OS_MACOS)
#    include <bootstrap.h>
#endif

namespace Core {

static constexpr MachPort::PortRight associated_port_right(MachPort::MessageRight right)
{
    switch (right) {
    case MachPort::MessageRight::MoveReceive:
        return MachPort::PortRight::Receive;
    case MachPort::MessageRight::MoveSend:
    case MachPort::MessageRight::CopySend:
    case MachPort::MessageRight::MakeSend:
        return MachPort::PortRight::Send;
    case MachPort::MessageRight::MoveSendOnce:
    case MachPort::MessageRight::MakeSendOnce:
        return MachPort::PortRight::SendOnce;

#if defined(AK_OS_MACOS)
    case MachPort::MessageRight::CopyReceive:
    case MachPort::MessageRight::DisposeReceive:
        return MachPort::PortRight::Receive;
    case MachPort::MessageRight::DisposeSend:
        return MachPort::PortRight::Send;
    case MachPort::MessageRight::DisposeSendOnce:
        return MachPort::PortRight::SendOnce;
#endif
    }
    VERIFY_NOT_REACHED();
}

Error mach_error_to_error(kern_return_t error)
{
    char const* err_string = mach_error_string(error);
    StringView const err_view(err_string, strlen(err_string));
    return Error::from_string_view(err_view);
}

#if defined(AK_OS_MACOS)
static Error bootstrap_error_to_error(kern_return_t error)
{
    char const* err_string = bootstrap_strerror(error);
    StringView const err_view(err_string, strlen(err_string));
    return Error::from_string_view(err_view);
}
#endif

MachPort::MachPort(PortRight right, mach_port_t port)
    : m_right(right)
    , m_port(port)
{
}

MachPort::~MachPort()
{
    unref_port();
}

MachPort::MachPort(MachPort&& other)
    : m_right(other.m_right)
    , m_port(other.release())
{
}

MachPort& MachPort::operator=(MachPort&& other)
{
    if (this != &other) {
        unref_port();
        m_right = other.m_right;
        m_port = other.release();
    }
    return *this;
}

void MachPort::unref_port()
{
    if (!MACH_PORT_VALID(m_port))
        return;

    kern_return_t res = KERN_FAILURE;
    switch (m_right) {
    case PortRight::Send:
    case PortRight::SendOnce:
    case PortRight::DeadName:
        res = mach_port_deallocate(mach_task_self(), m_port);
        break;
    case PortRight::Receive:
    case PortRight::PortSet:
        res = mach_port_mod_refs(mach_task_self(), m_port, to_underlying(m_right), -1);
        break;
    }
    VERIFY(res == KERN_SUCCESS);
}

ErrorOr<MachPort> MachPort::create_with_right(PortRight right)
{
    mach_port_t port = MACH_PORT_NULL;
    auto const ret = mach_port_allocate(mach_task_self(), to_underlying(right), &port);
    if (ret != KERN_SUCCESS) {
        dbgln("Unable to allocate port with right: {}", to_underlying(right));
        return mach_error_to_error(ret);
    }
    return MachPort(right, port);
}

MachPort MachPort::adopt_right(mach_port_t port, PortRight right)
{
    return MachPort(right, port);
}

mach_port_t MachPort::release()
{
    return exchange(m_port, MACH_PORT_NULL);
}

ErrorOr<MachPort> MachPort::insert_right(MessageRight right)
{
    auto const ret = mach_port_insert_right(mach_task_self(), m_port, m_port, to_underlying(right));
    if (ret != KERN_SUCCESS) {
        dbgln("Unable to insert message right: {}", to_underlying(right));
        return mach_error_to_error(ret);
    }
    return MachPort(associated_port_right(right), m_port);
}

#if defined(AK_OS_MACOS)

#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wdeprecated-declarations"
// bootstrap_register has been deprecated since macOS 10.5, but rules are more 'guidelines' than actual rules

ErrorOr<void> MachPort::register_with_bootstrap_server(ByteString const& service_name)
{
    if (service_name.length() > sizeof(name_t) - 1)
        return Error::from_errno(E2BIG);

    auto const ret = bootstrap_register(bootstrap_port, const_cast<char*>(service_name.characters()), m_port);
    if (ret != KERN_SUCCESS) {
        dbgln("Unable to register {} with bootstrap on port {:p}", service_name, m_port);
        return bootstrap_error_to_error(ret);
    }
    return {};
}

#    pragma GCC diagnostic pop

ErrorOr<MachPort> MachPort::look_up_from_bootstrap_server(ByteString const& service_name)
{
    if (service_name.length() > sizeof(name_t) - 1)
        return Error::from_errno(E2BIG);

    mach_port_t port = MACH_PORT_NULL;
    auto const ret = bootstrap_look_up(bootstrap_port, service_name.characters(), &port);
    if (ret != KERN_SUCCESS) {
        dbgln("Unable to look up service {} in bootstrap", service_name);
        return bootstrap_error_to_error(ret);
    }
    return MachPort(PortRight::Send, port);
}

#endif

}
