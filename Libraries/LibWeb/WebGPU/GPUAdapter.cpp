/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUAdapter.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUAdapter);

struct GPUAdapter::Impl {
    wgpu::Adapter adapter { nullptr };

    void dump_info() const
    {
        wgpu::AdapterInfo info {};
        adapter.GetInfo(&info);
        dbgln("Vendor ID: {}", info.vendorID);
        dbgln("Vendor: {}", StringView { info.vendor.data, info.vendor.length });
        dbgln("Architecture: {}", StringView { info.architecture.data, info.architecture.length });
        dbgln("Device ID: {}", info.deviceID);
        dbgln("Name: {}", StringView { info.device.data, info.device.length });
        dbgln("Driver description: {}", StringView { info.description.data, info.description.length });
    }
};

GPUAdapter::GPUAdapter(JS::Realm& realm, wgpu::Adapter adapter)
    : PlatformObject(realm)
    , m_impl(make<Impl>(move(adapter)))
{
    m_impl->dump_info();
}

JS::ThrowCompletionOr<GC::Ref<GPUAdapter>> GPUAdapter::create(JS::Realm& realm, wgpu::Adapter adapter)
{
    return realm.create<GPUAdapter>(realm, move(adapter));
}

void GPUAdapter::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUAdapter);
    Base::initialize(realm);
}

void GPUAdapter::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
