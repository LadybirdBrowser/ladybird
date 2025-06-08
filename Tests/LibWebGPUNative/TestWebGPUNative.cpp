/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Instance.h>
#include <LibWebGPUNative/Queue.h>

// FIXME: Complete enough of the implementation to test a "clear value" render pass into to a Gfx::Bitmap for headless/offscreen verification
TEST_CASE(clear)
{
    WebGPUNative::Instance instance;
    if (auto instance_result = instance.initialize(); instance_result.is_error()) {
        FAIL("Failed to initialize Instance");
        return;
    }

    Core::EventLoop loop;
    WebGPUNative::Adapter adapter = instance.adapter();
    NonnullRefPtr const adapter_promise = instance.request_adapter();
    loop.deferred_invoke([=, &adapter] {
        MUST(adapter.initialize());
        adapter_promise->resolve(std::move(adapter));
    });
    auto adapter_result = adapter_promise->await();
    if (adapter_result.is_error()) {
        FAIL("Adapter initialization failed");
        return;
    }
    adapter = std::move(adapter_result.value());

    WebGPUNative::Device device = adapter.device();
    NonnullRefPtr const device_promise = adapter.request_device();
    loop.deferred_invoke([=, &device] {
        MUST(device.initialize());
        device_promise->resolve(std::move(device));
    });
    auto device_result = device_promise->await();
    if (device_result.is_error()) {
        FAIL("Device initialization failed");
        return;
    }
    device = std::move(device_result.value());

    WebGPUNative::CommandEncoder command_encoder = device.command_encoder();
    auto command_encoder_result = command_encoder.initialize();
    if (command_encoder_result.is_error()) {
        FAIL("Command encoder initialization failed");
        return;
    }

}
