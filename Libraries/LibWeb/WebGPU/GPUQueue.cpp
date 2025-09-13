/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGPU/GPUQueue.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUQueue);

wgpu::QueueDescriptor GPUQueueDescriptor::to_wgpu() const
{
    auto const label_view = label.bytes_as_string_view();
    auto const label_byte_string = label_view.to_byte_string();
    return wgpu::QueueDescriptor { .nextInChain = nullptr, .label = wgpu::StringView { label_byte_string.characters(), label_byte_string.length() } };
}

struct GPUQueue::Impl {
    wgpu::Queue queue { nullptr };
    String label;
};

GPUQueue::GPUQueue(JS::Realm& realm, Impl impl)
    : PlatformObject(realm)
    , m_impl(make<Impl>(move(impl)))
{
}

GPUQueue::~GPUQueue() = default;

JS::ThrowCompletionOr<GC::Ref<GPUQueue>> GPUQueue::create(JS::Realm& realm, wgpu::Queue queue)
{
    return realm.create<GPUQueue>(realm, Impl {
                                             .queue = move(queue),
                                             .label = ""_string,
                                         });
}

void GPUQueue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUQueue);
    Base::initialize(realm);
}

void GPUQueue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

String const& GPUQueue::label() const
{
    return m_impl->label;
}

void GPUQueue::set_label(String const& label)
{
    m_impl->label = label;
}

}
