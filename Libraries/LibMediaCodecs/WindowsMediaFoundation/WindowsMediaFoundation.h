/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Windows.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/Export.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace Media::WindowsMediaFoundation {

using Microsoft::WRL::ComPtr;

MEDIACODECS_API i64 sample_time_from_duration(AK::Duration);
MEDIACODECS_API AK::Duration duration_from_sample_time(i64 sample_time);

// Activates the first decoder the system ranks for the input type's format that accepts the type.
MEDIACODECS_API DecoderErrorOr<ComPtr<IMFTransform>> create_decoder_transform(CodecID, IMFMediaType& input_type);

// Sets the first output type the transform offers whose subtype is accepted, and returns it.
MEDIACODECS_API DecoderErrorOr<ComPtr<IMFMediaType>> set_first_acceptable_output_type(IMFTransform&, Function<bool(GUID const& subtype)> const& subtype_is_acceptable);

MEDIACODECS_API DecoderErrorOr<ComPtr<IMFSample>> create_input_sample(ReadonlyBytes, AK::Duration timestamp, AK::Duration duration);

// What the transform's output stream needs from us, which changes along with its output type.
struct OutputStreamInfo {
    bool transform_provides_samples { false };
    u32 sample_byte_count { 0 };
    u32 sample_alignment { 0 };
};
MEDIACODECS_API DecoderErrorOr<OutputStreamInfo> output_stream_info(IMFTransform&);

struct NeedsMoreInput { };
// The output type has to be chosen again before any more output can be taken.
struct OutputTypeNeeded { };
using ProcessedOutput = Variant<ComPtr<IMFSample>, NeedsMoreInput, OutputTypeNeeded>;

// Takes the next output from the transform. Without stream info, which is only known once an output type is set, no
// sample is offered for the transform to fill.
MEDIACODECS_API DecoderErrorOr<ProcessedOutput> process_output(IMFTransform&, Optional<OutputStreamInfo> const&);

}
