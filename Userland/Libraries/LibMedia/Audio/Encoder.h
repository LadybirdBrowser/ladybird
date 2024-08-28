/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Forward.h"
#include "Sample.h"
#include <AK/Error.h>
#include <AK/Span.h>

namespace Audio {

class Encoder {
public:
    virtual ~Encoder() = default;

    // Encodes the given samples and writes them to the output stream.
    // Note that due to format restrictions, not all samples might be written immediately, this is only guaranteed after a call to finalize().
    virtual ErrorOr<void> write_samples(ReadonlySpan<Sample> samples) = 0;

    // Finalizes the stream, future calls to write_samples() will cause an error.
    // This method makes sure that all samples are encoded and written out.
    // This method is called in the destructor, but since this can error, you should call this function yourself before disposing of the decoder.
    virtual ErrorOr<void> finalize() = 0;

    // Sets the metadata for this audio file.
    // Not all encoders support this, and metadata may not be writeable after starting to write samples.
    virtual ErrorOr<void> set_metadata([[maybe_unused]] Metadata const& metadata) { return {}; }

    // Provides a hint about the total number of samples to the encoder, improving some encoder's performance in various aspects.
    // Note that the hint does not have to be fully correct; wrong hints never cause errors, not even indirectly.
    virtual void sample_count_hint([[maybe_unused]] size_t sample_count) { }
};

}
