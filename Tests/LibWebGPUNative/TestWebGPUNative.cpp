/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
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

    Vector<WebGPUNative::RenderPassColorAttachment> render_pass_color_attachments;
    render_pass_color_attachments.append(WebGPUNative::RenderPassColorAttachment { .view = texture_view, .clear_value = WebGPUNative::Color { 1.0, 0.0, 0.0, 1.0 } });

    WebGPUNative::RenderPassDescriptor render_pass_descriptor;
    render_pass_descriptor.color_attachments = render_pass_color_attachments;

    WebGPUNative::RenderPassEncoder render_pass_encoder = TRY_OR_FAIL(command_encoder.begin_render_pass(render_pass_descriptor));
    render_pass_encoder.end();

    WebGPUNative::CommandBuffer command_buffer = TRY_OR_FAIL(command_encoder.finish());

    WebGPUNative::Queue queue = device.queue();
    Vector<NonnullRawPtr<WebGPUNative::CommandBuffer>> command_buffers;
    command_buffers.append(command_buffer);
    TRY_OR_FAIL(queue.submit(command_buffers));

    auto expected_png_file = TRY_OR_FAIL(Core::File::open("./clear.png"sv, Core::File::OpenMode::Read));
    auto expected_png_bytes = TRY_OR_FAIL(expected_png_file->read_until_eof());
    auto expected_decoded_frames = TRY_OR_FAIL(Gfx::PNGImageDecoderPlugin::create(expected_png_bytes));
    auto expected_frame = TRY_OR_FAIL(expected_decoded_frames->frame(0));
    auto expected_bitmap = expected_frame.image;

    auto const mapped_buffer = TRY_OR_FAIL(texture.map_buffer());
    auto actual_bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, texture_size));
    for (auto const& [pixel, x, y] : mapped_buffer->pixels()) {
        actual_bitmap->set_pixel(x, y, pixel);
        EXPECT_EQ(expected_bitmap->get_pixel(x, y), pixel);
    }
    EXPECT_EQ(expected_bitmap->size(), actual_bitmap->size());
    EXPECT_EQ(expected_bitmap->data_size(), actual_bitmap->data_size());
}
