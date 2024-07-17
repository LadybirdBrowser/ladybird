/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <LibAudio/SampleFormats.h>
#include <LibCore/Forward.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/Thread.h>

namespace Audio {

enum class OutputState {
    Playing,
    Suspended,
};

// This class implements high-level audio playback behavior. It is primarily intended as an abstract cross-platform
// interface to be used by Ladybird (and its dependent libraries) for playback.
//
// The interface is designed to be simple and robust. All control functions can be called safely from any thread.
// Timing information provided by the class should allow audio timestamps to be tracked with the best accuracy possible.
class PlaybackStream : public AtomicRefCounted<PlaybackStream> {
public:
    using AudioDataRequestCallback = Function<ReadonlyBytes(Bytes buffer, PcmSampleFormat format, size_t sample_count)>;

    // Creates a new audio Output class.
    //
    // The initial_output_state parameter determines whether it will begin playback immediately.
    //
    // The AudioDataRequestCallback will be called when the Output needs more audio data to fill
    // its buffers and continue playback.
    static ErrorOr<NonnullRefPtr<PlaybackStream>> create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&&);

    virtual ~PlaybackStream() = default;

    // Sets the callback function that will be fired whenever the server consumes more data than is made available
    // by the data request callback. It will fire when either the data request runs too long, or the data request
    // returns no data. If all the input data has been exhausted and this event fires, that means that playback
    // has ended.
    virtual void set_underrun_callback(Function<void()>) = 0;

    // Resume playback from the suspended state, requesting new data for audio buffers as soon as possible.
    //
    // The value provided to the promise resolution will match the `total_time_played()` at the exact moment that
    // the stream was resumed.
    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() = 0;
    // Completes playback of any buffered audio data and then suspends playback and buffering.
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() = 0;
    // Drops any buffered audio data and then suspends playback and buffering. This can used be to stop playback
    // as soon as possible instead of waiting for remaining audio to play.
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() = 0;

    // Returns a accurate monotonically-increasing time duration that is based on the number of samples that have
    // been played by the output device. The value is interpolated and takes into account latency to the speakers
    // whenever possible.
    //
    // This function should be able to run from any thread safely.
    virtual ErrorOr<AK::Duration> total_time_played() = 0;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double volume) = 0;
};

}
