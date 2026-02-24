/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/AnalyserNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AnalyserNode);

AnalyserNode::AnalyserNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
    : AudioNode(realm, context)
    , m_fft_size(options.fft_size)
    , m_max_decibels(options.max_decibels)
    , m_min_decibels(options.min_decibels)
    , m_smoothing_time_constant(options.smoothing_time_constant)
{
}

AnalyserNode::~AnalyserNode() = default;

WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> AnalyserNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#current-time-domain-data
Vector<f32> const& AnalyserNode::current_time_domain_data()
{
    u64 render_quantum_index = 0;
    if (try_update_time_domain_cache_from_context(render_quantum_index))
        return m_cached_time_domain_data;

    size_t quantum_index = current_render_quantum_index();
    if (!m_cached_render_quantum_index.has_value()
        || m_cached_render_quantum_index.value() != quantum_index
        || m_cached_time_domain_data.size() != m_fft_size) {
        m_cached_render_quantum_index = quantum_index;
        m_cached_time_domain_data = capture_time_domain_window();
        m_cached_frequency_data.clear();
    }
    return m_cached_time_domain_data;
}

// https://webaudio.github.io/web-audio-api/#current-frequency-data
Vector<f32> const& AnalyserNode::current_frequency_data()
{
    u64 render_quantum_index = 0;
    if (try_update_frequency_cache_from_context(render_quantum_index) && m_cached_frequency_data.size() == frequency_bin_count())
        return m_cached_frequency_data;

    // 1. Compute the current time-domain data.
    // NOTE: The spec requires that multiple calls to get*FrequencyData() within the same render quantum
    // return the same values.

    size_t quantum_index = current_render_quantum_index();
    if (!m_cached_render_quantum_index.has_value()
        || m_cached_render_quantum_index.value() != quantum_index
        || m_cached_time_domain_data.size() != m_fft_size) {
        m_cached_render_quantum_index = quantum_index;
        m_cached_time_domain_data = capture_time_domain_window();
        m_cached_frequency_data.clear();
    }

    if (m_cached_frequency_data.size() == frequency_bin_count())
        return m_cached_frequency_data;

    // FIXME: If we have neither offline injected analysis nor a realtime RenderGraph snapshot,
    // the only safe fallback is explicit silence. Do not do control-thread FFT work here.
    m_cached_frequency_data.clear();
    m_cached_frequency_data.resize(frequency_bin_count());
    for (size_t i = 0; i < m_cached_frequency_data.size(); ++i)
        m_cached_frequency_data[i] = -AK::Infinity<f32>;
    return m_cached_frequency_data;
}

bool AnalyserNode::try_update_time_domain_cache_from_context(u64& out_render_quantum_index)
{
    auto const& audio_context = this->context();

    Vector<f32> time_domain;
    time_domain.resize(m_fft_size);

    u64 render_quantum_index = 0;
    if (!audio_context->try_copy_realtime_analyser_data(node_id(), static_cast<u32>(m_fft_size), time_domain.span(), {}, render_quantum_index))
        return false;

    out_render_quantum_index = render_quantum_index;
    m_cached_render_quantum_index = static_cast<size_t>(render_quantum_index);
    m_cached_time_domain_data = move(time_domain);
    m_cached_frequency_data.clear();
    return true;
}

bool AnalyserNode::try_update_frequency_cache_from_context(u64& out_render_quantum_index)
{
    auto const& audio_context = this->context();

    Vector<f32> time_domain;
    time_domain.resize(m_fft_size);

    Vector<f32> frequency_db;
    frequency_db.resize(frequency_bin_count());

    u64 render_quantum_index = 0;
    if (!audio_context->try_copy_realtime_analyser_data(node_id(), static_cast<u32>(m_fft_size), time_domain.span(), frequency_db.span(), render_quantum_index))
        return false;

    out_render_quantum_index = render_quantum_index;
    m_cached_render_quantum_index = static_cast<size_t>(render_quantum_index);
    m_cached_time_domain_data = move(time_domain);
    m_cached_frequency_data = move(frequency_db);
    return true;
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloatfrequencydata
WebIDL::ExceptionOr<void> AnalyserNode::get_float_frequency_data(GC::Root<WebIDL::BufferSource> const& array)
{
    auto const& frequency_data = current_frequency_data();

    if (!is<JS::Float32Array>(*array->raw_object()))
        return vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");
    auto output_floats = static_cast<JS::Float32Array&>(*array->raw_object()).data();

    // Write the current frequency data into array. If array has fewer elements than the frequencyBinCount,
    // the excess elements will be dropped. If array has more elements than the frequencyBinCount, the
    // excess elements will be ignored.
    size_t floats_to_write = min(output_floats.size(), frequency_bin_count());
    if (floats_to_write == 0)
        return {};

    ReadonlySpan<f32> input { frequency_data.data(), floats_to_write };
    input.copy_to(output_floats.trim(floats_to_write));
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytefrequencydata
WebIDL::ExceptionOr<void> AnalyserNode::get_byte_frequency_data(GC::Root<WebIDL::BufferSource> const& array)
{
    auto const& dB_data = current_frequency_data();

    auto& vm = this->vm();
    if (!is<JS::Uint8Array>(*array->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Uint8Array");
    auto output_bytes = static_cast<JS::Uint8Array&>(*array->raw_object()).data();

    // Write the current frequency data into array. If array’s byte length is less than frequencyBinCount,
    // the excess elements will be dropped. If array’s byte length is greater than the frequencyBinCount ,
    // the excess elements will be ignored.
    size_t bytes_to_write = min(output_bytes.size(), frequency_bin_count());
    if (bytes_to_write == 0)
        return {};

    // For getByteFrequencyData(), the 𝑌[𝑘] is clipped to lie between minDecibels and maxDecibels
    // and then scaled to fit in an unsigned byte such that minDecibels is represented by the
    // value 0 and maxDecibels is represented by the value 255.
    f32 const min_decibels = static_cast<f32>(m_min_decibels);
    f32 const max_decibels = static_cast<f32>(m_max_decibels);
    f32 const delta_dB = max_decibels - min_decibels;

    for (size_t i = 0; i < bytes_to_write; ++i) {
        f32 clamped = AK::clamp(dB_data[i], min_decibels, max_decibels);
        output_bytes[i] = static_cast<u8>(255 * (clamped - min_decibels) / delta_dB);
    }
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloattimedomaindata
WebIDL::ExceptionOr<void> AnalyserNode::get_float_time_domain_data(GC::Root<WebIDL::BufferSource> const& array)
{
    auto const& time_domain_data = current_time_domain_data();

    auto& vm = this->vm();
    if (!is<JS::Float32Array>(*array->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");
    auto output_floats = static_cast<JS::Float32Array&>(*array->raw_object()).data();

    // Write the current time-domain data (waveform data) into array. If array has fewer elements than the
    // value of fftSize, the excess elements will be dropped. If array has more elements than the value of
    // fftSize, the excess elements will be ignored.
    size_t floats_to_write = min(output_floats.size(), fft_size());
    if (floats_to_write == 0)
        return {};

    ReadonlySpan<f32> input { time_domain_data.data(), floats_to_write };
    input.copy_to(output_floats.trim(floats_to_write));
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytetimedomaindata
WebIDL::ExceptionOr<void> AnalyserNode::get_byte_time_domain_data(GC::Root<WebIDL::BufferSource> const& array)
{
    auto const& time_domain_data = current_time_domain_data();
    VERIFY(time_domain_data.size() == m_fft_size);

    if (!is<JS::Uint8Array>(*array->raw_object()))
        return vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Uint8Array");
    auto output_bytes = static_cast<JS::Uint8Array&>(*array->raw_object()).data();

    // Write the current time-domain data (waveform data) into array. If array’s byte length is less than
    // fftSize, the excess elements will be dropped. If array’s byte length is greater than the fftSize,
    // the excess elements will be ignored.
    size_t bytes_to_write = min(output_bytes.size(), fft_size());
    if (bytes_to_write == 0)
        return {};

    for (size_t i = 0; i < bytes_to_write; ++i) {
        f32 clamped = AK::clamp(128 * (1 + time_domain_data[i]), 0.0f, 255.0f);
        output_bytes[i] = static_cast<u8>(clamped);
    }
    return {};
}

// FIXME: all of these setters, all of the js setters on all of the other audio nodes, need
// to enqueue a parameter update.

// https://webaudio.github.io/web-audio-api/#dom-analysernode-fftsize
WebIDL::ExceptionOr<void> AnalyserNode::set_fft_size(unsigned long fft_size)
{
    if (fft_size < 32 || fft_size > 32768 || !is_power_of_two(fft_size))
        return WebIDL::IndexSizeError::create(realm(), "Analyser node fftSize not a power of 2 between 32 and 32768"_utf16);
    m_fft_size = fft_size;
    m_cached_render_quantum_index.clear();
    m_cached_time_domain_data.clear();
    m_cached_frequency_data.clear();
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_max_decibels(double max_decibels)
{
    if (m_min_decibels >= max_decibels)
        return WebIDL::IndexSizeError::create(realm(), "Analyser node minDecibels greater than maxDecibels"_utf16);
    m_max_decibels = max_decibels;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_min_decibels(double min_decibels)
{
    if (min_decibels >= m_max_decibels)
        return WebIDL::IndexSizeError::create(realm(), "Analyser node minDecibels greater than maxDecibels"_utf16);

    m_min_decibels = min_decibels;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_smoothing_time_constant(double smoothing_time_constant)
{
    if (smoothing_time_constant > 1.0 || smoothing_time_constant < 0.0)
        return WebIDL::IndexSizeError::create(realm(), "Analyser node smoothingTimeConstant not between 0.0 and 1.0"_utf16);

    m_smoothing_time_constant = smoothing_time_constant;
    return {};
}

WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> AnalyserNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
{
    if (options.min_decibels >= options.max_decibels)
        return WebIDL::IndexSizeError::create(realm, "Analyser node minDecibels greater than maxDecibels"_utf16);

    if (options.smoothing_time_constant > 1.0 || options.smoothing_time_constant < 0.0)
        return WebIDL::IndexSizeError::create(realm, "Analyser node smoothingTimeConstant not between 0.0 and 1.0"_utf16);

    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.create<AnalyserNode>(realm, context, options);
    TRY(node->set_fft_size(options.fft_size));

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#AnalyserNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

void AnalyserNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnalyserNode);
    Base::initialize(realm);
}

size_t AnalyserNode::current_render_quantum_index() const
{
    // https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloatfrequencydata

    // If another call to getFloatFrequencyData() or getByteFrequencyData() occurs within the
    // same render quantum as a previous call, the current frequency data is not updated with
    // the same data. Instead, the previously computed data is returned.
    auto context = this->context();
    f32 sample_rate = context->sample_rate();
    if (sample_rate <= 0)
        return 0;

    size_t quantum_size = context->render_quantum_size();
    if (quantum_size == 0)
        return 0;

    double frames = context->current_time() * sample_rate;
    if (frames <= 0)
        return 0;

    return static_cast<size_t>(frames) / quantum_size;
}

Vector<f32> AnalyserNode::capture_time_domain_window() const
{
    if (m_rendered_time_domain_data.has_value() && is<OfflineAudioContext>(*context()))
        return m_rendered_time_domain_data.value();

    Vector<f32> time_domain_data;
    time_domain_data.resize(m_fft_size);
    return time_domain_data;
}

void AnalyserNode::set_time_domain_data_from_rendering(Badge<OfflineAudioContext>, ReadonlySpan<f32> time_domain)
{
    m_rendered_time_domain_data = Vector<f32> {};
    m_rendered_time_domain_data->resize(m_fft_size);

    // If there's too much, use the tail.
    size_t const to_copy = min(time_domain.size(), static_cast<size_t>(m_fft_size));
    time_domain.slice(time_domain.size() - to_copy)
        .copy_trimmed_to(m_rendered_time_domain_data->span().slice(m_fft_size - to_copy, to_copy));

    m_cached_render_quantum_index.clear();
    m_cached_time_domain_data.clear();
    m_cached_frequency_data.clear();
}

void AnalyserNode::set_analysis_data_from_rendering(Badge<OfflineAudioContext>, ReadonlySpan<f32> time_domain, ReadonlySpan<f32> frequency_data_db, size_t render_quantum_index)
{
    m_rendered_time_domain_data = Vector<f32> {};
    m_rendered_time_domain_data->resize(m_fft_size);

    size_t const to_copy = min(time_domain.size(), static_cast<size_t>(m_fft_size));
    time_domain.slice(time_domain.size() - to_copy)
        .copy_trimmed_to(m_rendered_time_domain_data->span().slice(m_fft_size - to_copy, to_copy));

    m_cached_render_quantum_index = render_quantum_index;
    m_cached_time_domain_data = m_rendered_time_domain_data.value();
    m_cached_frequency_data = Vector<f32> {};
    m_cached_frequency_data.ensure_capacity(frequency_bin_count());
    size_t const bins_to_copy = min(frequency_data_db.size(), frequency_bin_count());
    for (size_t i = 0; i < bins_to_copy; ++i)
        m_cached_frequency_data.unchecked_append(frequency_data_db[i]);
    for (size_t i = bins_to_copy; i < frequency_bin_count(); ++i)
        m_cached_frequency_data.unchecked_append(-AK::Infinity<f32>);
}

}
