/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/Instance.h>

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
}
