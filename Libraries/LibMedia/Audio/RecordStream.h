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
#include <LibCore/Promise.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Export.h>

namespace Audio {

// A cross-platform interface for capturing audio from an input device. Implementations
// deliver interleaved float32 samples matching the sample specification passed alongside
// them. The callback is invoked on a thread owned by the audio backend; it must hand the
// data off to whichever thread wants to consume it without blocking.
class MEDIA_API RecordStream : public AtomicRefCounted<RecordStream> {
public:
    using CreatePromise = Core::Promise<NonnullRefPtr<RecordStream>>;
    using RecordCallback = Function<void(ReadonlyBytes, SampleSpecification const&)>;

    // Begins creating a capture stream on the platform audio backend and returns a promise
    // that is resolved when it is ready. The device id is a dom_device_id produced by
    // Media::AudioDevices, or an empty string to capture from the default input device.
    // Backends honor the requested sample specification by converting where they can; the
    // specification passed to the callback is authoritative.
    static NonnullRefPtr<CreatePromise> create(SampleSpecification const&, u32 fragment_size_bytes, StringView device_id, RecordCallback);

    virtual ~RecordStream() = default;

    virtual SampleSpecification const& sample_specification() const = 0;
};

}
