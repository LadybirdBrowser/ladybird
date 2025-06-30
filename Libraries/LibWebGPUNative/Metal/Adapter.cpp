/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Metal/AdapterImpl.h>

namespace WebGPUNative {

Adapter::Adapter(Instance const& gpu)
    : m_impl(make<Impl>(gpu))
{
}

Adapter::Adapter(Adapter&&) noexcept = default;
Adapter& Adapter::operator=(Adapter&&) noexcept = default;
Adapter::~Adapter() = default;

ErrorOr<void> Adapter::initialize()
{
    return m_impl->initialize();
}

Device Adapter::device() const
{
    return Device(*this);
}

NonnullRefPtr<Core::Promise<Device>> Adapter::request_device()
{
    return MUST(Core::Promise<Device>::try_create());
}

}
