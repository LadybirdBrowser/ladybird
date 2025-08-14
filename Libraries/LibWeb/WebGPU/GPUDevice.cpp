/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPU.h>
#include <LibWeb/WebGPU/GPUDevice.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUDevice);

struct GPUDevice::Impl {
    wgpu::Device device { nullptr };
    String label;
    GC::Ref<GPU> instance;
    GC::Ref<GPUQueue> queue;
};

GPUDevice::GPUDevice(JS::Realm& realm, Impl impl)
    : EventTarget(realm)
    , m_impl(make<Impl>(move(impl)))
{
}

GPUDevice::~GPUDevice() = default;

JS::ThrowCompletionOr<GC::Ref<GPUDevice>> GPUDevice::create(JS::Realm& realm, GPU& instance, wgpu::Device device, String const& label)
{
    auto queue = device.GetQueue();
    return realm.create<GPUDevice>(realm, Impl { .device = move(device), .label = label, .instance = instance, .queue = TRY(GPUQueue::create(realm, instance, move(queue))) });
}

void GPUDevice::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUDevice);
    EventTarget::initialize(realm);
}

void GPUDevice::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_impl->instance);
    visitor.visit(m_impl->queue);
}

// NOTE: wgpu::Device does not have a GetLabel() method exposed

String const& GPUDevice::label() const
{
    return m_impl->label;
}

void GPUDevice::set_label(String const& label)
{
    m_impl->label = label;
    auto label_view = label.bytes_as_string_view();
    m_impl->device.SetLabel(wgpu::StringView { label_view.characters_without_null_termination(), label_view.length() });
}

GC::Ref<GPUQueue> GPUDevice::queue() const
{
    return m_impl->queue;
}

}
