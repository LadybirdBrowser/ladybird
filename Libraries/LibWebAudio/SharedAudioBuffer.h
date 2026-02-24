/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>

namespace Web::WebAudio::Render {

struct SharedAudioBufferBinding {
    u64 buffer_id { 0 };
    Core::AnonymousBuffer shared_memory;
};

class SharedAudioBuffer final : public RefCounted<SharedAudioBuffer> {
public:
    static NonnullRefPtr<SharedAudioBuffer> create(f32 sample_rate, size_t channel_count, size_t length_in_sample_frames, Vector<Vector<f32>>&& channels);
    static ErrorOr<NonnullRefPtr<SharedAudioBuffer>> create_from_buffer(Core::AnonymousBuffer shared_memory);

    f32 sample_rate() const;
    size_t channel_count() const;
    size_t length_in_sample_frames() const;

    ReadonlySpan<f32> channel(size_t index) const;

    Core::AnonymousBuffer to_buffer() const;

private:
    SharedAudioBuffer(f32 sample_rate, size_t channel_count, size_t length_in_sample_frames, Core::AnonymousBuffer data);

    f32 m_sample_rate { 0.0f };
    size_t m_channel_count { 0 };
    size_t m_length_in_sample_frames { 0 };
    Core::AnonymousBuffer m_data;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::SharedAudioBufferBinding const&);

template<>
ErrorOr<Web::WebAudio::Render::SharedAudioBufferBinding> decode(Decoder&);

}
