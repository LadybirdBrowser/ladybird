/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Instance.h>
#include <LibWebGPUNative/Queue.h>

// FIXME: Complete enough of the implementation to test a "clear value" render pass into to a Gfx::Bitmap for headless/offscreen verification
TEST_CASE(clear)
{
    WebGPUNative::Instance instance;
    TRY_OR_FAIL(instance.initialize());

    Core::EventLoop loop;
    WebGPUNative::Adapter adapter = instance.adapter();
    NonnullRefPtr const adapter_promise = instance.request_adapter();
    loop.deferred_invoke([=, &adapter] {
        TRY_OR_FAIL(adapter.initialize());
        adapter_promise->resolve(std::move(adapter));
    });
    adapter = TRY_OR_FAIL(adapter_promise->await());

    WebGPUNative::Device device = adapter.device();
    NonnullRefPtr const device_promise = adapter.request_device();
    loop.deferred_invoke([=, &device] {
        TRY_OR_FAIL(device.initialize());
        device_promise->resolve(std::move(device));
    });
    device = TRY_OR_FAIL(device_promise->await());
    [[maybe_unused]] WebGPUNative::Queue queue = device.queue();
}
