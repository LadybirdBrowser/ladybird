/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/GenericShorthands.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Queue.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/Codecs/NALUnit.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameHandle.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationVideoDecoder.h>
#include <LibThreading/Thread.h>

namespace Media::WindowsMediaFoundation {

// What the transform's output stream currently produces, which can change between any two frames.
struct OutputFormat {
    GUID subtype { GUID_NULL };
    u8 bit_depth { 8 };
    // The buffers are padded out to the decoder's block size, and the aperture is the part that is displayed.
    Gfx::Size<u32> frame_size;
    Gfx::Point<u32> aperture_offset;
    Gfx::Size<u32> aperture_size;
    i32 default_stride { 0 };
};

struct WindowsMediaFoundationVideoDecoder::DecodedOutput {
    // None marks the end of the stream, after the last frame.
    ComPtr<IMFSample> sample;
    OutputFormat format;
    AK::Duration timestamp;
    AK::Duration duration;
    u64 generation { 0 };
};

struct WindowsMediaFoundationVideoDecoder::Transform {
    AK_ALLOC_WITH_KMALLOC;

    ComPtr<IMFTransform> transform;
    OutputFormat output_format;
    // Absent until the transform has accepted an output type.
    Optional<OutputStreamInfo> output_stream_info;
};

class WindowsMediaFoundationVideoDecoder::ThreadData final : public AtomicRefCounted<ThreadData> {
public:
    void set_output_arrived_callback(Function<void()>);

    void enqueue(DecodedOutput);
    void complete_outputs_until_exit();
    // Waits for output still being completed only while draining, when nothing else could wake the caller.
    DecoderErrorOr<DecodedOutput> take_completed_output(bool draining);
    void return_to_front(DecodedOutput);
    void record_failure(DecoderError);
    void discard_outputs();
    void exit();

private:
    Vector<DecodedOutput> take_all_outputs_while_locked();

    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    // Media Foundation hands frames back in presentation order, once nothing decoded later could precede them.
    Queue<DecodedOutput> m_awaiting_completion;
    Vector<DecodedOutput> m_completed;
    Optional<DecoderError> m_decode_failure;
    bool m_completed_end_of_stream { false };
    bool m_caller_is_waiting_for_output { false };
    bool m_should_exit { false };
    u64 m_generation { 0 };
    Function<void()> m_output_arrived_callback;
};

namespace {

Optional<GUID> input_subtype_for_codec(CodecID codec_id)
{
    switch (codec_id) {
    case CodecID::H264:
        return MFVideoFormat_H264;
    case CodecID::H265:
        return MFVideoFormat_HEVC;
    default:
        return {};
    }
}

ComPtr<IMFMediaType> create_input_type(GUID const& subtype, Optional<Gfx::Size<u32>> coded_size)
{
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type)))
        return nullptr;
    if (FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)))
        return nullptr;
    if (FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype)))
        return nullptr;
    if (FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive)))
        return nullptr;
    if (coded_size.has_value() && FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, coded_size->width(), coded_size->height())))
        return nullptr;
    return type;
}

// The HEVC decoder refuses to produce any output format, or to take input, until it is told the picture size.
Optional<Gfx::Size<u32>> coded_size_from_configuration(CodecID codec_id, ReadonlyBytes configuration)
{
    if (codec_id != CodecID::H265)
        return {};
    auto sets = Codecs::H265::parse_parameter_sets_from_configuration_record(configuration);
    if (!sets.has_value())
        return {};
    for (auto nal_unit : sets->sequence) {
        auto sequence_set = Codecs::H265::parse_sequence_parameter_set(nal_unit);
        if (sequence_set.has_value())
            return Gfx::Size<u32> { sequence_set->pic_width_in_luma_samples, sequence_set->pic_height_in_luma_samples };
    }
    return {};
}

DecoderErrorOr<ComPtr<IMFTransform>> create_transform(CodecID codec_id, Optional<Gfx::Size<u32>> coded_size = {})
{
    auto subtype = input_subtype_for_codec(codec_id);
    if (!subtype.has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Media Foundation has no decoder for codec {}", codec_id);

    auto input_type = create_input_type(*subtype, coded_size);
    if (!input_type)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to create a Media Foundation media type"sv);
    return create_decoder_transform(codec_id, *input_type.Get());
}

bool profile_is_decodable(ParsedCodec const& codec)
{
    switch (codec.codec_id()) {
    case CodecID::H264: {
        auto parameters = codec.h264_parameters();
        if (!parameters.has_value())
            return true;
        auto profile = parameters->profile();
        if (!profile.has_value())
            return false;
        // The decoder Windows ships handles 8-bit 4:2:0 only.
        using Profile = Codecs::H264::Profile;
        return first_is_one_of(*profile, Profile::Baseline, Profile::Main, Profile::High);
    }
    case CodecID::H265: {
        auto parameters = codec.h265_parameters();
        if (!parameters.has_value())
            return true;
        auto profile = parameters->profile();
        if (!profile.has_value())
            return false;
        using Profile = Codecs::H265::Profile;
        return first_is_one_of(*profile, Profile::Main, Profile::Main10);
    }
    default:
        return false;
    }
}

bool output_subtype_is_usable(GUID const& subtype)
{
    return subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_P010;
}

DecoderErrorOr<void> wait_until_decoded(IMFSample&)
{
    // FIXME: Software decoding produces output synchronously from ProcessOutput. Hardware will require waiting for
    //        a signal that the output buffer is ready to consume.
    return {};
}

DecoderErrorOr<void> append_annex_b_nal_unit(ByteBuffer& buffer, ReadonlyBytes nal_unit)
{
    static constexpr Array<u8, 4> START_CODE { 0, 0, 0, 1 };
    DECODER_TRY_ALLOC(buffer.try_append(START_CODE.span()));
    DECODER_TRY_ALLOC(buffer.try_append(nal_unit));
    return {};
}

}

Optional<DecoderCapabilities> WindowsMediaFoundationVideoDecoder::capabilities(ParsedCodec const& codec)
{
    if (!input_subtype_for_codec(codec.codec_id()).has_value())
        return {};
    if (!profile_is_decodable(codec))
        return {};

    // Activating a transform to answer this costs enough that the answer is worth keeping.
    static NeverDestroyed<Mutex> support_mutex;
    static NeverDestroyed<HashMap<u32, bool>> support_by_codec;

    MutexLocker locker { *support_mutex };
    auto is_supported = support_by_codec->ensure(to_underlying(codec.codec_id()), [&] {
        return !create_transform(codec.codec_id()).is_error();
    });
    if (!is_supported)
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = false };
}

DecoderErrorOr<NonnullOwnPtr<WindowsMediaFoundationVideoDecoder>> WindowsMediaFoundationVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    auto transform = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) Transform));
    transform->transform = TRY(create_transform(codec_id, coded_size_from_configuration(codec_id, codec_initialization_data)));
    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<ThreadData>());

    auto frame_pool_result = VideoFramePool::create();
    if (frame_pool_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to create a video frame pool: {}", frame_pool_result.release_error());

    auto decoder = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) WindowsMediaFoundationVideoDecoder(codec_id, move(transform), thread_data, frame_pool_result.release_value())));
    TRY(decoder->apply_codec_configuration(codec_initialization_data));
    // Some decoders refuse every output format until they have seen input, so this is retried when they ask.
    (void)decoder->negotiate_output_format();

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create("MFDecode"sv, [thread_data] -> intptr_t {
        thread_data->complete_outputs_until_exit();
        return 0;
    }));
    thread->start();
    thread->detach();
    return decoder;
}

WindowsMediaFoundationVideoDecoder::WindowsMediaFoundationVideoDecoder(CodecID codec_id, NonnullOwnPtr<Transform> transform, NonnullRefPtr<ThreadData> thread_data, NonnullRefPtr<VideoFramePool> frame_pool)
    : m_codec_id(codec_id)
    , m_transform(move(transform))
    , m_thread_data(move(thread_data))
    , m_frame_pool(move(frame_pool))
{
}

WindowsMediaFoundationVideoDecoder::~WindowsMediaFoundationVideoDecoder()
{
    m_thread_data->exit();

    if (m_transform->output_stream_info.has_value())
        m_transform->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    m_frame_pool->shed_storage();
}

DecoderErrorOr<void> WindowsMediaFoundationVideoDecoder::apply_codec_configuration(ReadonlyBytes configuration)
{
    if (configuration.is_empty())
        return {};

    m_annex_b_parameter_sets.clear();
    auto append_all = [&](auto const& nal_units) -> DecoderErrorOr<void> {
        for (auto nal_unit : nal_units)
            TRY(append_annex_b_nal_unit(m_annex_b_parameter_sets, nal_unit));
        return {};
    };

    switch (m_codec_id) {
    case CodecID::H264: {
        auto sets = Codecs::H264::parse_parameter_sets_from_configuration_record(configuration);
        if (!sets.has_value())
            return DecoderError::corrupted("Invalid H.264 configuration record"sv);
        m_nal_unit_length_size = sets->nal_unit_length_size;
        TRY(append_all(sets->sequence));
        TRY(append_all(sets->picture));
        return {};
    }
    case CodecID::H265: {
        auto sets = Codecs::H265::parse_parameter_sets_from_configuration_record(configuration);
        if (!sets.has_value())
            return DecoderError::corrupted("Invalid H.265 configuration record"sv);
        m_nal_unit_length_size = sets->nal_unit_length_size;
        TRY(append_all(sets->video));
        TRY(append_all(sets->sequence));
        TRY(append_all(sets->picture));
        return {};
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

DecoderErrorOr<ByteBuffer> WindowsMediaFoundationVideoDecoder::annex_b_access_unit_for(CodedFrame const& coded_frame)
{
    ByteBuffer access_unit;
    if (coded_frame.is_keyframe())
        DECODER_TRY_ALLOC(access_unit.try_append(m_annex_b_parameter_sets));
    DECODER_TRY_ALLOC(access_unit.try_ensure_capacity(access_unit.size() + coded_frame.data().size()));

    Codecs::NALUnitIterator iterator { coded_frame.data(), m_nal_unit_length_size };
    for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next())
        TRY(append_annex_b_nal_unit(access_unit, *nal_unit));
    if (iterator.has_error())
        return DecoderError::corrupted("Invalid NAL unit length"sv);
    return access_unit;
}

DecoderErrorOr<void> WindowsMediaFoundationVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    if (m_draining)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Media Foundation decoder has been drained"sv);

    if (auto configuration = coded_frame.new_codec_configuration(); configuration.has_value())
        TRY(apply_codec_configuration(*configuration));

    auto access_unit = TRY(annex_b_access_unit_for(coded_frame));

    auto sample = TRY(create_input_sample(access_unit, coded_frame.presentation_timestamp(), coded_frame.duration()));
    auto sample_time = sample_time_from_duration(coded_frame.presentation_timestamp());
    if (coded_frame.is_keyframe())
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (m_next_input_is_discontinuous)
        sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);

    auto submitted_frame = SubmittedFrame { sample_time, coded_frame.presentation_timestamp(), coded_frame.duration(), intent };
    auto insertion_index = m_submitted_frames.size();
    while (insertion_index > 0 && m_submitted_frames[insertion_index - 1].sample_time > sample_time)
        insertion_index--;
    DECODER_TRY_ALLOC(m_submitted_frames.try_insert(insertion_index, submitted_frame));

    auto& transform = *m_transform->transform.Get();
    auto result = transform.ProcessInput(0, sample.Get(), 0);
    if (result == MF_E_NOTACCEPTING) {
        // The transform takes no more input until the output it is holding has been taken.
        TRY(collect_available_outputs());
        result = transform.ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(result)) {
        // Collecting output may have retired frames ahead of this one.
        if (auto index = index_of_submitted_frame(sample_time); index.has_value())
            m_submitted_frames.remove(*index);
        return DecoderError::format(DecoderErrorCategory::Corrupted, "Media Foundation decoder rejected a frame with result {:#x}", static_cast<u32>(result));
    }
    m_next_input_is_discontinuous = false;

    // Frames start completing as soon as the transform hands them out.
    return collect_available_outputs();
}

void WindowsMediaFoundationVideoDecoder::signal_end_of_stream()
{
    auto& transform = *m_transform->transform.Get();
    transform.ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform.ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    m_draining = true;

    if (auto collected = collect_available_outputs(); collected.is_error())
        m_thread_data->record_failure(collected.release_error());
}

DecoderErrorOr<void> WindowsMediaFoundationVideoDecoder::negotiate_output_format()
{
    auto& transform = *m_transform->transform.Get();
    auto output_type = TRY(set_first_acceptable_output_type(transform, output_subtype_is_usable));
    GUID subtype = GUID_NULL;
    output_type->GetGUID(MF_MT_SUBTYPE, &subtype);

    OutputFormat format;
    format.subtype = subtype;
    format.bit_depth = subtype == MFVideoFormat_P010 ? 10 : 8;

    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(MFGetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, &width, &height)))
        format.frame_size = { width, height };
    format.aperture_size = format.frame_size;

    MFVideoArea aperture {};
    if (SUCCEEDED(output_type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), nullptr))) {
        auto offset_x = max(aperture.OffsetX.value, 0);
        auto offset_y = max(aperture.OffsetY.value, 0);
        auto aperture_width = min<u32>(aperture.Area.cx, format.frame_size.width() - min<u32>(offset_x, format.frame_size.width()));
        auto aperture_height = min<u32>(aperture.Area.cy, format.frame_size.height() - min<u32>(offset_y, format.frame_size.height()));
        format.aperture_offset = { static_cast<u32>(offset_x), static_cast<u32>(offset_y) };
        format.aperture_size = { aperture_width, aperture_height };
    }

    UINT32 default_stride = 0;
    if (SUCCEEDED(output_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride)))
        format.default_stride = static_cast<i32>(default_stride);

    auto stream_info = TRY(output_stream_info(transform));
    m_transform->output_format = format;

    // The HEVC decoder crashes if told that streaming begins before it has an output format.
    auto was_streaming = m_transform->output_stream_info.has_value();
    m_transform->output_stream_info = stream_info;
    if (!was_streaming) {
        if (FAILED(transform.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) || FAILED(transform.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
            return DecoderError::with_description(DecoderErrorCategory::Unknown, "Media Foundation decoder refused to start streaming"sv);
    }
    return {};
}

DecoderErrorOr<void> WindowsMediaFoundationVideoDecoder::collect_available_outputs()
{
    while (true) {
        auto output = TRY(process_output(*m_transform->transform.Get(), m_transform->output_stream_info));
        if (output.has<NeedsMoreInput>()) {
            // A draining transform that wants input has handed out everything it had.
            if (m_draining && !m_drained) {
                m_drained = true;
                m_thread_data->enqueue({});
            }
            return {};
        }
        if (output.has<OutputTypeNeeded>()) {
            TRY(negotiate_output_format());
            continue;
        }

        auto sample = move(output.get<ComPtr<IMFSample>>());
        LONGLONG sample_time = 0;
        LONGLONG sample_duration = 0;
        sample->GetSampleTime(&sample_time);
        sample->GetSampleDuration(&sample_duration);

        auto submitted_frame_index = index_of_submitted_frame(sample_time);
        auto submitted_frame = SubmittedFrame {
            .sample_time = sample_time,
            .timestamp = duration_from_sample_time(sample_time),
            .duration = duration_from_sample_time(sample_duration),
        };
        if (submitted_frame_index.has_value())
            submitted_frame = m_submitted_frames[*submitted_frame_index];
        retire_submitted_frames_through(submitted_frame_index);
        if (submitted_frame.intent == DecodeIntent::Reference)
            continue;

        DecodedOutput decoded_output;
        decoded_output.sample = move(sample);
        decoded_output.format = m_transform->output_format;
        decoded_output.timestamp = submitted_frame.timestamp;
        decoded_output.duration = submitted_frame.duration;
        m_thread_data->enqueue(move(decoded_output));
    }
}

void WindowsMediaFoundationVideoDecoder::set_output_arrived_callback(Function<void()> callback)
{
    m_thread_data->set_output_arrived_callback(move(callback));
}

void WindowsMediaFoundationVideoDecoder::ThreadData::set_output_arrived_callback(Function<void()> callback)
{
    MutexLocker locker { m_mutex };
    m_output_arrived_callback = move(callback);
}

void WindowsMediaFoundationVideoDecoder::ThreadData::enqueue(DecodedOutput output)
{
    MutexLocker locker { m_mutex };
    output.generation = m_generation;
    m_awaiting_completion.enqueue(move(output));
    m_condition.broadcast();
}

void WindowsMediaFoundationVideoDecoder::ThreadData::complete_outputs_until_exit()
{
    while (true) {
        DecodedOutput output;
        {
            MutexLocker locker { m_mutex };
            while (m_awaiting_completion.is_empty() && !m_should_exit)
                m_condition.wait();
            if (m_should_exit)
                return;
            output = m_awaiting_completion.dequeue();
        }

        DecoderErrorOr<void> decoded = {};
        if (output.sample)
            decoded = wait_until_decoded(*output.sample.Get());

        MutexLocker locker { m_mutex };
        if (m_should_exit)
            return;
        if (output.generation == m_generation) {
            if (!output.sample)
                m_completed_end_of_stream = true;
            else if (decoded.is_error())
                m_decode_failure = decoded.release_error();
            else
                m_completed.append(move(output));
            if (!m_caller_is_waiting_for_output && m_output_arrived_callback)
                m_output_arrived_callback();
        }
        m_condition.broadcast();
    }
}

DecoderErrorOr<WindowsMediaFoundationVideoDecoder::DecodedOutput> WindowsMediaFoundationVideoDecoder::ThreadData::take_completed_output(bool draining)
{
    MutexLocker locker { m_mutex };
    while (m_completed.is_empty()) {
        if (m_decode_failure.has_value())
            return m_decode_failure.release_value();
        if (m_completed_end_of_stream)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Media Foundation decoder has been drained"sv);
        if (!draining)
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Media Foundation decoder has no finished frame to hand out"sv);

        m_caller_is_waiting_for_output = true;
        m_condition.wait();
        m_caller_is_waiting_for_output = false;
    }
    return m_completed.take_first();
}

void WindowsMediaFoundationVideoDecoder::ThreadData::return_to_front(DecodedOutput output)
{
    MutexLocker locker { m_mutex };
    m_completed.prepend(move(output));
}

void WindowsMediaFoundationVideoDecoder::ThreadData::record_failure(DecoderError error)
{
    MutexLocker locker { m_mutex };
    if (!m_decode_failure.has_value())
        m_decode_failure = move(error);
}

Vector<WindowsMediaFoundationVideoDecoder::DecodedOutput> WindowsMediaFoundationVideoDecoder::ThreadData::take_all_outputs_while_locked()
{
    auto outputs = move(m_completed);
    while (!m_awaiting_completion.is_empty())
        outputs.append(m_awaiting_completion.dequeue());
    return outputs;
}

void WindowsMediaFoundationVideoDecoder::ThreadData::discard_outputs()
{
    // The stale samples are released after the lock, which the completion thread waits on.
    Vector<DecodedOutput> stale_outputs;
    {
        MutexLocker locker { m_mutex };
        m_generation++;
        stale_outputs = take_all_outputs_while_locked();
        m_decode_failure.clear();
        m_completed_end_of_stream = false;
    }
}

void WindowsMediaFoundationVideoDecoder::ThreadData::exit()
{
    Vector<DecodedOutput> stale_outputs;
    {
        MutexLocker locker { m_mutex };
        m_should_exit = true;
        stale_outputs = take_all_outputs_while_locked();
        m_condition.broadcast();
    }
}

Optional<size_t> WindowsMediaFoundationVideoDecoder::index_of_submitted_frame(i64 sample_time) const
{
    for (size_t index = 0; index < m_submitted_frames.size(); index++) {
        if (m_submitted_frames[index].sample_time == sample_time)
            return index;
    }
    return {};
}

void WindowsMediaFoundationVideoDecoder::retire_submitted_frames_through(Optional<size_t> index)
{
    // Frames come out in presentation order, so any earlier one still waiting was dropped by the decoder.
    if (index.has_value())
        m_submitted_frames.remove(0, *index + 1);
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> WindowsMediaFoundationVideoDecoder::take_next_output(CodingIndependentCodePoints const& container_cicp, [[maybe_unused]] Optional<AK::Duration> target)
{
    TRY(collect_available_outputs());

    auto output = TRY(m_thread_data->take_completed_output(m_draining));

    // Only this thread takes outputs or flushes, so a frame that cannot go out yet can go back to the front.
    auto frame = copy_output_into_frame_pool(output, container_cicp);
    if (frame.is_error())
        m_thread_data->return_to_front(move(output));
    return frame;
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> WindowsMediaFoundationVideoDecoder::copy_output_into_frame_pool(DecodedOutput const& output, CodingIndependentCodePoints const& container_cicp)
{
    auto const& format = output.format;
    auto subsampling = Subsampling::yuv420();
    auto size = format.aperture_size.to_type<int>();

    auto layout_result = frame_plane_layout(size, format.bit_depth, subsampling);
    if (layout_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "Failed to compute video frame plane layout: {}", layout_result.release_error());
    auto layout = layout_result.release_value();

    auto acquired_slot = m_frame_pool->try_acquire(layout.total_byte_count);
    if (!acquired_slot.has_value())
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video frame slot is held elsewhere"sv);
    auto pool_slot_result = m_frame_pool->try_adopt_acquired_slot(*acquired_slot);
    if (pool_slot_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a pooled frame slot reference"sv);

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(output.sample->ConvertToContiguousBuffer(&buffer)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to read a Media Foundation output sample"sv);

    BYTE* scanline_zero = nullptr;
    LONG pitch = 0;
    DWORD buffer_length = 0;
    ComPtr<IMF2DBuffer> buffer_2d;
    bool locked_2d = SUCCEEDED(buffer.As(&buffer_2d)) && SUCCEEDED(buffer_2d->Lock2D(&scanline_zero, &pitch));
    if (!locked_2d) {
        if (FAILED(buffer->Lock(&scanline_zero, nullptr, &buffer_length)))
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to lock a Media Foundation output buffer"sv);
        auto component_size = format.bit_depth > 8 ? 2 : 1;
        pitch = format.default_stride != 0 ? format.default_stride : static_cast<LONG>(format.frame_size.width() * component_size);
    }
    ScopeGuard unlock_buffer = [&] {
        if (locked_2d)
            buffer_2d->Unlock2D();
        else
            buffer->Unlock();
    };

    if (pitch <= 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Bottom-up Media Foundation output is not supported"sv);

    // Each line holds one luma sample, or one interleaved pair of chroma samples, for every column.
    size_t const component_size = format.bit_depth > 8 ? 2 : 1;
    size_t const source_pitch = pitch;
    size_t const chroma_plane_offset = source_pitch * format.frame_size.height();
    auto const chroma_size = subsampling.subsampled_size(format.aperture_size);
    auto const chroma_offset = Gfx::Point<u32> { format.aperture_offset.x() / 2, format.aperture_offset.y() / 2 };
    size_t const required_length = chroma_plane_offset + source_pitch * (chroma_offset.y() + chroma_size.height());
    if (!locked_2d && buffer_length < required_length)
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Media Foundation output buffer is too small for its format"sv);

    auto bytes = acquired_slot->bytes;
    auto y_plane = bytes.slice(0, layout.y_size);
    auto u_plane = bytes.slice(layout.u_offset, layout.u_size);
    auto v_plane = bytes.slice(layout.v_offset, layout.v_size);

    // P010 keeps its 10 bits in the high end of each 16-bit sample, while our frames keep them in the low end.
    auto const sample_shift = format.bit_depth > 8 ? 16 - format.bit_depth : 0;
    auto copy_sample = [&](u8 const* source, u8* destination) {
        if (component_size == 1) {
            *destination = *source;
            return;
        }
        u16 value;
        memcpy(&value, source, sizeof(value));
        value >>= sample_shift;
        memcpy(destination, &value, sizeof(value));
    };

    auto const luma_line_size = format.aperture_size.width() * component_size;
    for (u32 row = 0; row < format.aperture_size.height(); row++) {
        auto const* source = scanline_zero + source_pitch * (format.aperture_offset.y() + row) + format.aperture_offset.x() * component_size;
        auto* destination = y_plane.offset_pointer(row * luma_line_size);
        if (sample_shift == 0) {
            memcpy(destination, source, luma_line_size);
            continue;
        }
        for (u32 column = 0; column < format.aperture_size.width(); column++)
            copy_sample(source + column * component_size, destination + column * component_size);
    }

    auto const chroma_line_size = chroma_size.width() * component_size;
    for (u32 row = 0; row < chroma_size.height(); row++) {
        auto const* source = scanline_zero + chroma_plane_offset + source_pitch * (chroma_offset.y() + row) + chroma_offset.x() * 2 * component_size;
        auto* u_destination = u_plane.offset_pointer(row * chroma_line_size);
        auto* v_destination = v_plane.offset_pointer(row * chroma_line_size);
        for (u32 column = 0; column < chroma_size.width(); column++) {
            copy_sample(source + (column * 2) * component_size, u_destination + column * component_size);
            copy_sample(source + (column * 2 + 1) * component_size, v_destination + column * component_size);
        }
    }

    return DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(output.timestamp, output.duration, format.aperture_size, format.bit_depth, subsampling, container_cicp, pool_slot_result.release_value()));
}

void WindowsMediaFoundationVideoDecoder::flush()
{
    m_transform->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_submitted_frames.clear();
    m_draining = false;
    m_drained = false;
    m_next_input_is_discontinuous = true;
    m_thread_data->discard_outputs();
}

}
