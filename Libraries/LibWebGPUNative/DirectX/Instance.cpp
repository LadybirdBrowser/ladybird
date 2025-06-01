/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/Instance.h>
#include <d3d12sdklayers.h>
#include <wrl.h>

using namespace Microsoft::WRL;

namespace WebGPUNative {
namespace {

ComPtr<ID3D12Debug> debug_controller;

}

struct Instance::Impl {
    ErrorOr<void> initialize()
    {

#ifdef WEBGPUNATIVE_DEBUG
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
            debug_controller->EnableDebugLayer();
        }
#endif
        // NOTE: This is a no-op as DirectX doesn't have an instance abstraction, the main entry point
        // is IDXGIAdapter1 which will be added in the WebGPUNative::Adapter implementation
        return {};
    }
};

Instance::Instance()
    : m_impl(make<Impl>())
{
}

Instance::~Instance() = default;

ErrorOr<void> Instance::initialize()
{
    return m_impl->initialize();
}

Adapter Instance::adapter() const
{
    return Adapter(*this);
}

NonnullRefPtr<Core::Promise<Adapter>> Instance::request_adapter()
{
    return MUST(Core::Promise<Adapter>::try_create());
}

}
