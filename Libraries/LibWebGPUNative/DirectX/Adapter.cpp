/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/DirectX/AdapterImpl.h>

namespace WebGPUNative {

Adapter::Adapter(Instance const&)
    : m_impl(make<Impl>())
{
}

Adapter::Adapter(Adapter&&) noexcept = default;
Adapter& Adapter::operator=(Adapter&&) noexcept = default;
Adapter::~Adapter() = default;

ErrorOr<void> Adapter::initialize()
{
    return m_impl->initialize();
}

}
