/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>

namespace Media::WindowsMediaFoundation {

// Media Foundation counts time in units of 100 nanoseconds.
static constexpr i64 NANOSECONDS_PER_SAMPLE_TIME_UNIT = 100;

i64 sample_time_from_duration(AK::Duration duration)
{
    return duration.to_nanoseconds() / NANOSECONDS_PER_SAMPLE_TIME_UNIT;
}

AK::Duration duration_from_sample_time(i64 sample_time)
{
    return AK::Duration::from_time_units(sample_time, NANOSECONDS_PER_SAMPLE_TIME_UNIT, 1'000'000'000);
}

static bool media_foundation_is_started()
{
    static bool const started = [] {
        // Decoder threads never enter an apartment, so they rely on the process-wide one staying alive.
        CO_MTA_USAGE_COOKIE cookie;
        if (FAILED(CoIncrementMTAUsage(&cookie)))
            return false;
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }();
    return started;
}

DecoderErrorOr<ComPtr<IMFTransform>> create_decoder_transform(CodecID codec_id, IMFMediaType& input_type)
{
    if (!media_foundation_is_started())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation could not be started"sv);

    MFT_REGISTER_TYPE_INFO input_type_info {};
    if (FAILED(input_type.GetGUID(MF_MT_MAJOR_TYPE, &input_type_info.guidMajorType)) || FAILED(input_type.GetGUID(MF_MT_SUBTYPE, &input_type_info.guidSubtype)))
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Media Foundation input type has no format"sv);
    auto category = input_type_info.guidMajorType == MFMediaType_Audio ? MFT_CATEGORY_AUDIO_DECODER : MFT_CATEGORY_VIDEO_DECODER;

    IMFActivate** activates = nullptr;
    UINT32 activate_count = 0;
    auto flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MFTEnumEx(category, flags, &input_type_info, nullptr, &activates, &activate_count)))
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation could not enumerate its decoders"sv);
    ScopeGuard release_activates = [&] {
        for (UINT32 i = 0; i < activate_count; i++)
            activates[i]->Release();
        CoTaskMemFree(static_cast<void*>(activates));
    };

    for (UINT32 i = 0; i < activate_count; i++) {
        ComPtr<IMFTransform> transform;
        if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform))))
            continue;
        if (FAILED(transform->SetInputType(0, &input_type, 0))) {
            activates[i]->ShutdownObject();
            continue;
        }
        return transform;
    }

    return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation has no decoder for codec {}", codec_id);
}

DecoderErrorOr<ComPtr<IMFMediaType>> set_first_acceptable_output_type(IMFTransform& transform, Function<bool(GUID const& subtype)> const& subtype_is_acceptable)
{
    for (DWORD type_index = 0;; type_index++) {
        ComPtr<IMFMediaType> available_type;
        auto result = transform.GetOutputAvailableType(0, type_index, &available_type);
        if (result == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(result))
            return DecoderError::format(DecoderErrorCategory::Unknown, "Media Foundation decoder could not list its output types: {:#x}", static_cast<u32>(result));

        GUID subtype = GUID_NULL;
        if (FAILED(available_type->GetGUID(MF_MT_SUBTYPE, &subtype)) || !subtype_is_acceptable(subtype))
            continue;
        if (FAILED(transform.SetOutputType(0, available_type.Get(), 0)))
            return DecoderError::with_description(DecoderErrorCategory::Unknown, "Media Foundation decoder refused the output format it offered"sv);
        return available_type;
    }
    return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation decoder offers no output format we can read"sv);
}

DecoderErrorOr<ComPtr<IMFSample>> create_input_sample(ReadonlyBytes data, AK::Duration timestamp, AK::Duration duration)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), &buffer)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a Media Foundation buffer"sv);
    BYTE* buffer_data = nullptr;
    if (FAILED(buffer->Lock(&buffer_data, nullptr, nullptr)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to lock a Media Foundation buffer"sv);
    data.copy_to({ buffer_data, data.size() });
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(data.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get())))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a Media Foundation sample"sv);
    sample->SetSampleTime(sample_time_from_duration(timestamp));
    sample->SetSampleDuration(sample_time_from_duration(duration));
    return sample;
}

DecoderErrorOr<OutputStreamInfo> output_stream_info(IMFTransform& transform)
{
    MFT_OUTPUT_STREAM_INFO stream_info {};
    if (FAILED(transform.GetOutputStreamInfo(0, &stream_info)))
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "Media Foundation decoder did not describe its output stream"sv);
    return OutputStreamInfo {
        .transform_provides_samples = (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0,
        .sample_byte_count = stream_info.cbSize,
        .sample_alignment = stream_info.cbAlignment,
    };
}

DecoderErrorOr<ProcessedOutput> process_output(IMFTransform& transform, Optional<OutputStreamInfo> const& stream_info)
{
    while (true) {
        ComPtr<IMFSample> allocated_sample;
        if (stream_info.has_value() && !stream_info->transform_provides_samples) {
            ComPtr<IMFMediaBuffer> buffer;
            auto buffer_result = stream_info->sample_alignment > 1
                ? MFCreateAlignedMemoryBuffer(stream_info->sample_byte_count, stream_info->sample_alignment - 1, &buffer)
                : MFCreateMemoryBuffer(stream_info->sample_byte_count, &buffer);
            if (FAILED(buffer_result) || FAILED(MFCreateSample(&allocated_sample)) || FAILED(allocated_sample->AddBuffer(buffer.Get())))
                return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a Media Foundation output sample"sv);
        }

        MFT_OUTPUT_DATA_BUFFER output_buffer {};
        output_buffer.dwStreamID = 0;
        output_buffer.pSample = allocated_sample.Get();
        DWORD status = 0;
        auto result = transform.ProcessOutput(0, 1, &output_buffer, &status);
        if (output_buffer.pEvents != nullptr)
            output_buffer.pEvents->Release();

        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return NeedsMoreInput {};
        if (result == MF_E_TRANSFORM_STREAM_CHANGE || result == MF_E_TRANSFORM_TYPE_NOT_SET)
            return OutputTypeNeeded {};
        if (FAILED(result))
            return DecoderError::format(DecoderErrorCategory::Corrupted, "Media Foundation decoder failed to produce output with result {:#x}", static_cast<u32>(result));

        ComPtr<IMFSample> sample;
        if (allocated_sample)
            sample = move(allocated_sample);
        else
            sample.Attach(output_buffer.pSample);
        if (sample)
            return sample;
    }
}

}
