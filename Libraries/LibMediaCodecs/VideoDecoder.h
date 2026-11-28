/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>

#include "DecoderError.h"
#include "DecoderLatency.h"

namespace Media {

enum class DecodeIntent : u8 {
    Reference,
    Output,
};

enum class DecodeCompletion : u8 {
    // A frame or a failure is ready for take_next_output().
    ProducedOutput,
    ProducedNothing,
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() { }

    // Both callbacks are installed before any coded data is received, and run on whichever thread frees the storage
    // or finishes the decode.
    // Called without the decoder's locks held.
    virtual void set_storage_freed_callback(Function<void()>) = 0;
    // Called when a decode finishes while no take_next_output() call is waiting for it, so that a caller blocked
    // elsewhere knows to come back. Decoders whose decodes are complete by the time receive_coded_data() returns
    // never call it. It may be called with the decoder's locks held, so it must not call into the decoder.
    virtual void set_decode_completed_callback(Function<void(DecodeCompletion)>) = 0;
    // Returns whether output is ready to be taken or may still arrive without more coded data. The decode completed
    // callback is called when a decode finishing makes this stop returning true.
    virtual bool has_any_decode_in_progress() const = 0;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) = 0;
    virtual void signal_end_of_stream() = 0;

    // Callers may pass the optional target parameter to indicate the timestamp past which they need output. This
    // allows reordered codecs to produce a seek-resolving frame without filling the reorder queue first.
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(ColorMetadata const& container_color_metadata, Optional<AK::Duration> target = {}) = 0;

    virtual void flush() = 0;
};

}
