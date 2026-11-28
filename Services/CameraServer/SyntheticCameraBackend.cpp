/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <CameraServer/CameraBackend.h>
#include <LibCore/System.h>
#include <LibThreading/Thread.h>

namespace CameraServer {

namespace {

constexpr auto SYNTHETIC_DEVICE_ID = "synthetic:camera"sv;
// Distinguishable by its blue frames; ignores requests for other sizes or rates.
constexpr auto SECOND_SYNTHETIC_DEVICE_ID = "synthetic:second-camera"sv;
constexpr u32 SECOND_CAMERA_WIDTH = 640;
constexpr u32 SECOND_CAMERA_HEIGHT = 480;
constexpr u32 SECOND_CAMERA_FRAME_RATE = 30;

enum class Pattern : u8 {
    Colors,
    Blue,
};

// A band of brightness that slides across a field of slowly changing color, so that any two frames differ and a picture
// shown the wrong way up or with its colors swapped looks it.
void draw_test_pattern(Bytes i420, u32 width, u32 height, u64 frame_number, Pattern pattern)
{
    auto chroma_width = (width + 1) / 2;
    auto chroma_height = (height + 1) / 2;
    auto* y = i420.data();
    auto* u = y + static_cast<size_t>(width) * height;
    auto* v = u + static_cast<size_t>(chroma_width) * chroma_height;

    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column)
            y[static_cast<size_t>(row) * width + column] = static_cast<u8>(16 + ((column * 255ull / width + frame_number * 4) % 220));
    }
    for (u32 row = 0; row < chroma_height; ++row) {
        for (u32 column = 0; column < chroma_width; ++column) {
            auto index = static_cast<size_t>(row) * chroma_width + column;
            u[index] = pattern == Pattern::Blue ? 220 : static_cast<u8>(16 + column * 224ull / chroma_width);
            v[index] = pattern == Pattern::Blue ? 100 : static_cast<u8>(16 + row * 224ull / chroma_height);
        }
    }
}

class SyntheticStream final : public CameraStream {
public:
    SyntheticStream(u32 width, u32 height, u32 frame_rate, Pattern pattern, NonnullRefPtr<CameraStreamListener> listener)
    {
        m_thread = Threading::Thread::construct("Synthetic Camera"sv, [this, width, height, frame_rate, pattern, listener = move(listener)]() -> intptr_t {
            // NB: A camera that can't give its pictures doesn't open, rather than open and end.
            auto picture = ByteBuffer::create_uninitialized(Camera::i420_size(width, height));
            if (picture.is_error()) {
                listener->ended();
                return 0;
            }
            listener->format_decided(width, height, frame_rate);
            auto frame_duration = AK::Duration::from_nanoseconds(1'000'000'000 / frame_rate);
            auto next_frame_time = MonotonicTime::now();
            for (u64 frame_number = 0; !m_should_stop.load(); ++frame_number) {
                draw_test_pattern(picture.value().bytes(), width, height, frame_number, pattern);
                listener->frame_arrived(RawFrame {
                    .format = PixelFormat::I420,
                    .width = width,
                    .height = height,
                    .data = picture.value().bytes(),
                    .bytes_per_row = width,
                    .timestamp = AK::Duration::from_nanoseconds(frame_duration.to_nanoseconds() * static_cast<i64>(frame_number)),
                });

                next_frame_time += frame_duration;
                auto wait = next_frame_time - MonotonicTime::now();
                if (wait > AK::Duration::zero())
                    (void)Core::System::sleep_ms(static_cast<u32>(wait.to_milliseconds()));
            }
            return 0;
        });
        m_thread->start();
    }

    virtual ~SyntheticStream() override
    {
        m_should_stop.store(true);
        (void)m_thread->join();
    }

private:
    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_should_stop { false };
};

class SyntheticCameras final : public CameraBackend {
public:
    virtual void watch_devices(Function<void(Vector<Camera::DeviceInfo>)> on_device_list) override
    {
        on_device_list({
            Camera::DeviceInfo {
                .dom_device_id = SYNTHETIC_DEVICE_ID,
                .label = "Synthetic camera",
                .group_id = {},
                .is_default = true,
            },
            Camera::DeviceInfo {
                .dom_device_id = SECOND_SYNTHETIC_DEVICE_ID,
                .label = "Second synthetic camera",
                .group_id = {},
                .is_default = false,
            },
        });
    }

    virtual ErrorOr<NonnullOwnPtr<CameraStream>> open(ByteString const& device_id, Camera::StreamRequest request, NonnullRefPtr<CameraStreamListener> listener) override
    {
        if (device_id == SECOND_SYNTHETIC_DEVICE_ID)
            return make<SyntheticStream>(SECOND_CAMERA_WIDTH, SECOND_CAMERA_HEIGHT, SECOND_CAMERA_FRAME_RATE, Pattern::Blue, move(listener));
        if (!device_id.is_empty() && device_id != SYNTHETIC_DEVICE_ID)
            return Error::from_string_literal("No such camera");

        auto or_default = [](u32 value, u32 fallback, u32 maximum) { return value == 0 ? fallback : min(value, maximum); };
        auto width = or_default(request.width, DEFAULT_WIDTH, 1920);
        auto height = or_default(request.height, DEFAULT_HEIGHT, 1080);
        auto frame_rate = or_default(request.frame_rate, DEFAULT_FRAME_RATE, 60);
        return make<SyntheticStream>(width, height, frame_rate, Pattern::Colors, move(listener));
    }
};

}

NonnullOwnPtr<CameraBackend> create_synthetic_camera_backend()
{
    return make<SyntheticCameras>();
}

}
