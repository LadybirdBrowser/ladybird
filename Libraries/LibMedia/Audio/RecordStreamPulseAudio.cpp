/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibMedia/Audio/PulseAudioWrappers.h>
#include <LibMedia/Audio/RecordStream.h>

namespace Audio {

namespace {

class RecordStreamPulseAudio final : public RecordStream {
public:
    explicit RecordStreamPulseAudio(NonnullRefPtr<PulseAudioRecordStream> stream)
        : m_stream(move(stream))
    {
    }

    virtual SampleSpecification const& sample_specification() const override { return m_stream->sample_specification(); }

private:
    NonnullRefPtr<PulseAudioRecordStream> m_stream;
};

}

ErrorOr<NonnullRefPtr<RecordStream>> RecordStream::create(SampleSpecification const& specification, u32 fragment_size_bytes, StringView device_id, RecordCallback callback)
{
    auto context = TRY(PulseAudioContext::the());

    // AudioDevices encodes PulseAudio sources as "pulse:source:<name>". Unrecognized or
    // empty ids fall through to the server's default source.
    constexpr auto pulse_source_prefix = "pulse:source:"sv;
    ByteString device_name;
    if (device_id.starts_with(pulse_source_prefix))
        device_name = device_id.substring_view(pulse_source_prefix.length());

    auto pulse_stream = TRY(context->create_record_stream(specification, fragment_size_bytes,
        device_name.is_empty() ? nullptr : device_name.characters(), move(callback)));

    auto stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RecordStreamPulseAudio(move(pulse_stream))));
    return stream;
}

}
