/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibTest/TestCase.h>
#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Instance.h>
#include <LibWebGPUNative/Queue.h>
#include <LibWebGPUNative/Texture.h>
#include <LibWebGPUNative/TextureView.h>

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

    WebGPUNative::CommandEncoder command_encoder = device.command_encoder();

    TRY_OR_FAIL(command_encoder.initialize());

    constexpr Gfx::IntSize texture_size = { 800, 600 };
    WebGPUNative::Texture texture = device.texture(texture_size);
    TRY_OR_FAIL(texture.initialize());

    // Verify initial texture clear value is transparent black
    {
        auto const mapped_buffer = TRY_OR_FAIL(texture.map_buffer());
        for (auto const& [pixel, x, y] : mapped_buffer->pixels()) {
            if (pixel != Color::Transparent) {
                auto const r = pixel.red();
                auto const g = pixel.green();
                auto const b = pixel.blue();
                auto const a = pixel.alpha();
                FAIL(String::formatted("Unexpected clear pixel colour ({}, {}, {}, {}) at ({}, {})", r, g, b, a, x, y));
                return;
            }
        }
    }

    WebGPUNative::TextureView texture_view = texture.texture_view();
    TRY_OR_FAIL(texture_view.initialize());
}
