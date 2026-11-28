/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCamera/DeviceInfo.h>
#include <LibCamera/StreamRequest.h>

namespace CameraServer {

// How the cameras lay out the pictures they give. All are 8 bits a sample with chroma at half the resolution across;
// I420 and NV12 halve it down as well.
enum class PixelFormat : u8 {
    // A plane of luma, a plane of blue chroma, a plane of red chroma.
    I420,
    // A plane of luma, then a plane of blue and red chroma interleaved.
    NV12,
    // Luma, blue chroma, luma, red chroma, for every two pixels.
    YUY2,
};

// A picture as a camera gave it, in one run of memory that is the camera's again once the callback returns.
struct RawFrame {
    PixelFormat format { PixelFormat::I420 };
    u32 width { 0 };
    u32 height { 0 };
    ReadonlyBytes data;
    // From the start of one row of the first plane to the start of the next. The chroma planes of I420 have half of it.
    size_t bytes_per_row { 0 };
    AK::Duration timestamp;
};

// What a stream's camera has to say, on a thread of the backend's.
class CameraStreamListener : public AtomicRefCounted<CameraStreamListener> {
public:
    virtual ~CameraStreamListener() = default;

    // The camera is open, and every picture from here on has this size. Said once, before any picture.
    virtual void format_decided(u32 width, u32 height, u32 frame_rate) = 0;
    virtual void frame_arrived(RawFrame const&) = 0;
    // The camera would not open, or is gone: nothing more arrives.
    virtual void ended() = 0;
};

// An open camera. Destroying it closes the camera, after which the listener hears nothing.
class CameraStream {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CameraStream() = default;
};

// A platform's cameras. This is all a platform has to provide: the server around it is the same everywhere.
class CameraBackend {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CameraBackend() = default;

    // Reports the cameras once a list is available and again on every change, on the event loop of the calling thread.
    virtual void watch_devices(Function<void(Vector<Camera::DeviceInfo>)>) = 0;

    // Opens the camera with the dom_device_id, or the default one if that is empty, as close to the request as it does.
    virtual ErrorOr<NonnullOwnPtr<CameraStream>> open(ByteString const& device_id, Camera::StreamRequest, NonnullRefPtr<CameraStreamListener>) = 0;
};

// The cameras of the platform's backend that is built in; a build without one has no cameras.
// Called before the process is sandboxed, so that a backend may load what it needs while it can.
NonnullOwnPtr<CameraBackend> create_platform_camera_backend();

// Two cameras that need no hardware. The first gives a moving test pattern at whatever size and rate it is asked for;
// the second gives it in blue, at one size and rate only. This is what headless instances have.
NonnullOwnPtr<CameraBackend> create_synthetic_camera_backend();

// The cameras with the preferred one as the default, if it is among them; they are as they were otherwise.
Vector<Camera::DeviceInfo> with_preferred_default(Vector<Camera::DeviceInfo>, ByteString const& preferred_device_id);

// What a stream gets when the request has no preference.
static constexpr u32 DEFAULT_WIDTH = 640;
static constexpr u32 DEFAULT_HEIGHT = 480;
static constexpr u32 DEFAULT_FRAME_RATE = 30;

}
