/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibTest/TestCase.h>
#include <LibWebGPUNative/Adapter.h>
#include <LibWebGPUNative/CommandBuffer.h>
#include <LibWebGPUNative/CommandEncoder.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Instance.h>
#include <LibWebGPUNative/Queue.h>
#include <LibWebGPUNative/RenderPassEncoder.h>
#include <LibWebGPUNative/Texture.h>
#include <LibWebGPUNative/TextureView.h>

// Native implementation required for the following WebGPU sample:
/*
<!DOCTYPE html>
<html>
<head>
    <title>Ladybird WebGPU: Clear</title>
    <style>
        body {
            margin: 0;
            padding: 0;
            overflow: hidden;
        }
        canvas {
            display: block;
            width: 100vw;
            height: 100vh;
        }
    </style>
</head>
<body>
<canvas id="webgpuCanvas"></canvas>

<script>
    const ctx = webgpuCanvas.getContext("webgpu");
    let device;
    let greenValue = 0;

    function render() {
        greenValue += 0.01;
        if (greenValue > 1.0) {
            greenValue = 0;
        }

        const renderPassDescriptor = {
            colorAttachments: [
                {
                    view: ctx.getCurrentTexture().createView(),
                    clearValue: [1.0, greenValue, 0, 1.0],
                },
            ],
        };
        const commandEncoder = device.createCommandEncoder();
        const renderPassEncoder = commandEncoder.beginRenderPass(renderPassDescriptor);
        renderPassEncoder.end();
        device.queue.submit([commandEncoder.finish()]);

        requestAnimationFrame(render);
    }

    async function initWebGPU() {
        if (!navigator.gpu) {
            throw Error("WebGPU not supported");
        }
        const adapter = await navigator.gpu.requestAdapter();
        device = await adapter.requestDevice();
        ctx.configure({
            device: device,
        });
        requestAnimationFrame(render);
    }
    initWebGPU();
</script>
</body>
</html>
 */
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

    constexpr Gfx::IntSize texture_size = { 800, 600 };
    WebGPUNative::Texture texture = device.texture(texture_size);
    auto texture_result = texture.initialize();
    if (texture_result.is_error()) {
        FAIL("Texture initialization failed");
        return;
    }

    // Verify initial texture clear value is transparent black
    {
        auto mapped_buffer_result = texture.map_buffer();
        if (mapped_buffer_result.is_error()) {
            FAIL("Mapped buffer initialization failed");
            return;
        }
        auto const mapped_buffer = std::move(mapped_buffer_result.value());
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
    auto texture_view_result = texture_view.initialize();
    if (texture_view_result.is_error()) {
        FAIL("Texture view initialization failed");
        return;
    }

    Vector<WebGPUNative::RenderPassColorAttachment> render_pass_color_attachments;
    render_pass_color_attachments.append(WebGPUNative::RenderPassColorAttachment { .view = texture_view, .clear_value = WebGPUNative::Color { 1.0, 0.0, 0.0, 1.0 } });

    WebGPUNative::RenderPassDescriptor render_pass_descriptor;
    render_pass_descriptor.color_attachments = render_pass_color_attachments;

    auto render_pass_encoder_result = command_encoder.begin_render_pass(render_pass_descriptor);
    if (render_pass_encoder_result.is_error()) {
        FAIL("Render pass encoder initialization failed");
        return;
    }
    WebGPUNative::RenderPassEncoder render_pass_encoder = std::move(render_pass_encoder_result.value());
    render_pass_encoder.end();

    auto finish_result = command_encoder.finish();
    if (finish_result.is_error()) {
        FAIL("Command encoder finish failed");
        return;
    }

    WebGPUNative::CommandBuffer command_buffer = std::move(finish_result.value());

    WebGPUNative::Queue queue = device.queue();
    Vector<NonnullRawPtr<WebGPUNative::CommandBuffer>> command_buffers;
    command_buffers.append(command_buffer);
    auto submit_result = queue.submit(command_buffers);
    if (submit_result.is_error()) {
        FAIL("Submit command buffers initialization failed");
        return;
    }

    auto mapped_buffer_result = texture.map_buffer();
    if (mapped_buffer_result.is_error()) {
        FAIL("Mapped buffer initialization failed");
        return;
    }
    auto const mapped_buffer = std::move(mapped_buffer_result.value());
    constexpr Color expected_clear_value { 255, 0, 0, 255 };
    for (auto const& [pixel, x, y] : mapped_buffer->pixels()) {
        if (pixel != expected_clear_value) {
            auto const r = pixel.red();
            auto const g = pixel.green();
            auto const b = pixel.blue();
            auto const a = pixel.alpha();
            FAIL(String::formatted("Unexpected clear pixel colour ({}, {}, {}, {}) at ({}, {})", r, g, b, a, x, y));
            return;
        }
    }
}
