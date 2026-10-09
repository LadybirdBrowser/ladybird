/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StringView.h>
#include <LibAudio/Export.h>
#include <LibAudio/SampleSpecification.h>
#include <LibCore/Promise.h>

namespace Audio {

// A cross-platform interface for capturing audio from an input device. Implementations
// deliver interleaved float32 samples matching the sample specification passed alongside
// them. The callback is invoked on a thread owned by the audio backend; it must hand the
// data off to whichever thread wants to consume it without blocking.
class AUDIO_API RecordStream : public AtomicRefCounted<RecordStream> {
public:
    using CreatePromise = Core::Promise<NonnullRefPtr<RecordStream>>;
    using RecordCallback = Function<void(ReadonlyBytes, SampleSpecification const&)>;

    // Begins creating a capture stream and returns a promise that is resolved when it is ready.
    // The device lives in the AudioServer, which delivers its own format; the specification
    // passed to the callback is authoritative. A process with no server to reach is refused,
    // since only the AudioServer opens devices. The device id is a dom_device_id produced by
    // AudioDevices, or an empty string to capture from the default input device.
    static NonnullRefPtr<CreatePromise> create(StringView device_id, RecordCallback);

    virtual ~RecordStream() = default;

    virtual SampleSpecification const& sample_specification() const = 0;

    // Runs on the thread that created the stream once the backend behind it is gone for good:
    // nothing more is delivered, and the owner opens a new stream to carry on.
    Function<void()> on_capture_lost;
};

}
