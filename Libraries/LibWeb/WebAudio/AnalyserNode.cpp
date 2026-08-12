/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Rendering/AnalyserRenderNode.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <math.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AnalyserNode);

AnalyserNode::AnalyserNode(GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
    : AudioNode(context)
    , m_fft_size(options.fft_size)
    , m_max_decibels(options.max_decibels)
    , m_min_decibels(options.min_decibels)
    , m_smoothing_time_constant(options.smoothing_time_constant)
{
}

AnalyserNode::~AnalyserNode() = default;

WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> AnalyserNode::create(GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = GC::Heap::the().allocate<AnalyserNode>(context, options);
    node->set_fft_size_without_validation(options.fft_size);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#AnalyserNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    node->m_history.resize(MAX_FFT_SIZE);
    node->m_time_domain_ring = adopt_ref(*new Media::SpscAudioFrameRing(MAX_FFT_SIZE, 1));
    node->queue_render_node_creation(make<Rendering::AnalyserRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(), *node->m_time_domain_ring));

    return node;
}

void AnalyserNode::drain_time_domain_ring()
{
    if (!m_time_domain_ring)
        return;

    // Ring pops are destructive, so everything the render thread has produced is folded into
    // the control-side history of the last MAX_FFT_SIZE sample-frames. Keeping the full
    // maximum window around means growing fftSize immediately exposes older data, as the
    // spec requires.
    Array<float, 1024> chunk;
    while (true) {
        auto frame_count = m_time_domain_ring->try_pop(chunk.span());
        if (frame_count == 0)
            break;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            m_history[m_history_write_index] = chunk[frame];
            m_history_write_index = (m_history_write_index + 1) % m_history.size();
        }
        m_history_valid_frames = min(m_history.size(), m_history_valid_frames + frame_count);
    }
}

// https://webaudio.github.io/web-audio-api/#current-time-domain-data
Vector<f32> AnalyserNode::current_time_domain_data()
{
    // The input signal must be down-mixed to mono as if channelCount is 1, channelCountMode is "max" and
    // channelInterpretation is "speakers". This is independent of the settings for the AnalyserNode itself.
    // NB: The down-mix happens on the rendering thread in AnalyserRenderNode.
    drain_time_domain_ring();

    // The most recent fftSize frames are used for the down-mixing operation.
    Vector<f32> result;
    result.resize(m_fft_size);
    auto frames_available = min(static_cast<size_t>(m_fft_size), m_history_valid_frames);
    for (size_t frame = 0; frame < frames_available; ++frame) {
        auto history_index = (m_history_write_index + m_history.size() - frames_available + frame) % m_history.size();
        result[m_fft_size - frames_available + frame] = m_history[history_index];
    }
    return result;
}

// https://webaudio.github.io/web-audio-api/#blackman-window
Vector<f32> AnalyserNode::apply_a_blackman_window(Vector<f32> const& x) const
{
    f32 const a = 0.16;
    f32 const a0 = 0.5f * (1 - a);
    f32 const a1 = 0.5;
    f32 const a2 = a * 0.5f;
    unsigned long const N = m_fft_size;

    auto w = [&](unsigned long n) {
        return a0 - a1 * cos(2 * AK::Pi<f32> * (f32)n / (f32)N) + a2 * cos(4 * AK::Pi<f32> * (f32)n / (f32)N);
    };

    Vector<f32> x_hat;
    x_hat.resize(m_fft_size);

    // FIXME: Naive
    for (unsigned long i = 0; i < m_fft_size; i++) {
        x_hat[i] = x[i] * w(i);
    };

    return x_hat;
}

// Radix-2 Cooley-Tukey FFT, operating in place on (re, im). `n` must be a power of two, which
// the fftSize validation guarantees.
static void radix2_fft(Span<float> re, Span<float> im)
{
    auto const n = re.size();
    VERIFY(n == im.size() && n != 0 && (n & (n - 1)) == 0);

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            AK::swap(re[i], re[j]);
            AK::swap(im[i], im[j]);
        }
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        auto half = len >> 1;
        auto angle = -2.0 * AK::Pi<double> / static_cast<double>(len);
        auto wlen_re = AK::cos(angle);
        auto wlen_im = AK::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            double w_re = 1.0;
            double w_im = 0.0;
            for (size_t k = 0; k < half; ++k) {
                auto u_re = re[i + k];
                auto u_im = im[i + k];
                auto v_re = re[i + k + half] * static_cast<float>(w_re) - im[i + k + half] * static_cast<float>(w_im);
                auto v_im = re[i + k + half] * static_cast<float>(w_im) + im[i + k + half] * static_cast<float>(w_re);
                re[i + k] = u_re + v_re;
                im[i + k] = u_im + v_im;
                re[i + k + half] = u_re - v_re;
                im[i + k + half] = u_im - v_im;
                auto next_w_re = w_re * wlen_re - w_im * wlen_im;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_w_re;
            }
        }
    }
}

// https://webaudio.github.io/web-audio-api/#fourier-transform
//
// Returns `bin_count` (= fftSize / 2) magnitude values normalized by fftSize, per the spec's
// `(1/N) * |X[k]|` convention. The caller is expected to have applied a window already.
static Vector<f32> apply_a_fourier_transform(Vector<f32> const& input, size_t bin_count)
{
    Vector<f32> magnitudes;
    magnitudes.resize(bin_count);
    if (input.is_empty())
        return magnitudes;

    auto n = input.size();
    Vector<float> re;
    Vector<float> im;
    re.resize(n);
    im.resize(n);
    for (size_t i = 0; i < n; ++i)
        re[i] = input[i];

    radix2_fft(re, im);

    auto inv_n = 1.0f / static_cast<float>(n);
    for (size_t k = 0; k < bin_count && k < n; ++k)
        magnitudes[k] = AK::sqrt(re[k] * re[k] + im[k] * im[k]) * inv_n;

    return magnitudes;
}

// https://webaudio.github.io/web-audio-api/#smoothing-over-time
Vector<f32> AnalyserNode::smoothing_over_time(Vector<f32> const& current_block)
{
    // 1. Let X^−1[k] be the result of this operation on the previous block. The previous block is defined as being
    //    the buffer computed by the previous smoothing over time operation, or an array of N zeros if this is the
    //    first time we are smoothing over time.
    if (m_previous_block.size() != current_block.size()) {
        m_previous_block.clear();
        m_previous_block.resize(current_block.size());
    }

    // 2. Let τ be the value of the smoothingTimeConstant attribute for this AnalyserNode.
    auto tau = static_cast<f32>(m_smoothing_time_constant);

    // 3. Let X[k] be the result of applying a Fourier transform of the current block.
    Vector<f32> result;
    result.resize(current_block.size());

    // 4. Then the smoothed value, X^[k], is computed by X^[k] = τ * X^−1[k] + (1 − τ) * |X[k]|
    //    If X^[k] is NaN, positive infinity or negative infinity, set X^[k] = 0.
    for (size_t i = 0; i < current_block.size(); ++i) {
        auto smoothed = tau * m_previous_block[i] + (1.0f - tau) * AK::fabs(current_block[i]);
        if (!isfinite(smoothed))
            smoothed = 0.0f;
        result[i] = smoothed;
    }

    m_previous_block = result;
    return result;
}

// https://webaudio.github.io/web-audio-api/#conversion-to-db
Vector<f32> AnalyserNode::conversion_to_dB(Vector<f32> const& X_hat) const
{
    // Y[k] = 20 log10 X^[k]   for k = 0,…,N−1
    Vector<f32> result;
    result.ensure_capacity(X_hat.size());
    for (auto x : X_hat) {
        // NB: 20 * log10(0) is -∞. The byte-domain conversion clamps this later.
        if (x > 0.0f)
            result.unchecked_append(20.0f * AK::log10(x));
        else
            result.unchecked_append(-AK::Infinity<f32>);
    }
    return result;
}

// https://webaudio.github.io/web-audio-api/#current-frequency-data
Vector<f32> AnalyserNode::current_frequency_data()
{
    // If another call to getFloatFrequencyData() or getByteFrequencyData() occurs within the same render quantum
    // as a previous call, the current frequency data is not updated with the same data. Instead, the previously
    // computed data is returned.
    // AD-HOC: The control thread cannot observe render quantum boundaries directly; the context's currentTime,
    //         which advances as quanta are rendered, stands in for the quantum stamp.
    auto current_time = context()->current_time();
    if (m_frequency_data_cache_time == current_time)
        return m_cached_frequency_data;

    // 1. Compute the current time-domain data.
    auto time_domain_data = current_time_domain_data();

    // 2. Apply a Blackman window to the time domain input data.
    auto windowed_input = apply_a_blackman_window(time_domain_data);

    // 3. Apply a Fourier transform to the windowed time domain input data to get real and imaginary frequency data.
    auto frequency_domain_data = apply_a_fourier_transform(windowed_input, frequency_bin_count());

    // 4. Smooth over time the frequency domain data.
    auto smoothed_data = smoothing_over_time(frequency_domain_data);

    // 5. Convert to dB.
    auto result = conversion_to_dB(smoothed_data);

    m_cached_frequency_data = result;
    m_frequency_data_cache_time = current_time;
    return result;
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloatfrequencydata
WebIDL::ExceptionOr<void> AnalyserNode::get_float_frequency_data(GC::Ref<JS::Float32Array> array)
{
    // Write the current frequency data into array. If array has fewer elements than the frequencyBinCount,
    // the excess elements will be dropped. If array has more elements than the frequencyBinCount, the
    // excess elements will be ignored. The most recent fftSize frames are used in computing the frequency data.
    auto const frequency_data = current_frequency_data();

    auto record = JS::make_typed_array_with_buffer_witness_record(*array, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(record))
        return {};
    auto floats_to_write = min(static_cast<size_t>(JS::typed_array_length(record)), static_cast<size_t>(frequency_bin_count()));
    array->viewed_array_buffer()->overwrite(array->byte_offset(), frequency_data.data(), floats_to_write * sizeof(float));

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytefrequencydata
WebIDL::ExceptionOr<void> AnalyserNode::get_byte_frequency_data(GC::Ref<JS::Uint8Array> array)
{
    Vector<f32> dB_data = current_frequency_data();
    Vector<u8> byte_data;
    byte_data.ensure_capacity(dB_data.size());

    // For getByteFrequencyData(), the 𝑌[𝑘] is clipped to lie between minDecibels and maxDecibels
    // and then scaled to fit in an unsigned byte such that minDecibels is represented by the
    // value 0 and maxDecibels is represented by the value 255.
    f32 delta_dB = m_max_decibels - m_min_decibels;
    for (auto x : dB_data) {
        x = max(x, m_min_decibels);
        x = min(x, m_max_decibels);

        byte_data.unchecked_append(static_cast<u8>(255 * (x - m_min_decibels) / delta_dB));
    }

    // Write the current frequency data into array. If array’s byte length is less than frequencyBinCount,
    // the excess elements will be dropped. If array’s byte length is greater than the frequencyBinCount ,
    // the excess elements will be ignored. The most recent fftSize frames are used in computing the frequency data.
    auto record = JS::make_typed_array_with_buffer_witness_record(*array, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(record))
        return {};
    auto bytes_to_write = min(static_cast<size_t>(JS::typed_array_length(record)), static_cast<size_t>(frequency_bin_count()));
    array->viewed_array_buffer()->overwrite(array->byte_offset(), byte_data.data(), bytes_to_write);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloattimedomaindata
WebIDL::ExceptionOr<void> AnalyserNode::get_float_time_domain_data(GC::Ref<JS::Float32Array> array)
{
    // Write the current time-domain data (waveform data) into array. If array has fewer elements than the
    // value of fftSize, the excess elements will be dropped. If array has more elements than the value of
    // fftSize, the excess elements will be ignored. The most recent fftSize frames are written (after downmixing).

    Vector<f32> time_domain_data = current_time_domain_data();

    auto record = JS::make_typed_array_with_buffer_witness_record(*array, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(record))
        return {};
    auto floats_to_write = min(static_cast<size_t>(JS::typed_array_length(record)), static_cast<size_t>(fft_size()));
    array->viewed_array_buffer()->overwrite(array->byte_offset(), time_domain_data.data(), floats_to_write * sizeof(float));

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytetimedomaindata
WebIDL::ExceptionOr<void> AnalyserNode::get_byte_time_domain_data(GC::Ref<JS::Uint8Array> array)
{
    // Write the current time-domain data (waveform data) into array. If array’s byte length is less than
    // fftSize, the excess elements will be dropped. If array’s byte length is greater than the fftSize,
    // the excess elements will be ignored. The most recent fftSize frames are used in computing the byte data.

    Vector<f32> time_domain_data = current_time_domain_data();
    VERIFY(time_domain_data.size() == m_fft_size);

    Vector<u8> byte_data;
    byte_data.ensure_capacity(m_fft_size);

    for (size_t i = 0; i < m_fft_size; i++) {
        auto x = 128 * (1 + time_domain_data[i]);
        x = max(x, 0);
        x = min(x, 255);
        byte_data.unchecked_append(static_cast<u8>(x));
    }

    auto record = JS::make_typed_array_with_buffer_witness_record(*array, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(record))
        return {};
    auto bytes_to_write = min(static_cast<size_t>(JS::typed_array_length(record)), static_cast<size_t>(fft_size()));
    array->viewed_array_buffer()->overwrite(array->byte_offset(), byte_data.data(), bytes_to_write);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-fftsize
void AnalyserNode::set_fft_size_without_validation(unsigned long fft_size)
{
    // reset previous block to 0s
    m_previous_block = Vector<f32>();
    m_previous_block.resize(fft_size / 2);

    m_frequency_data_cache_time = {};

    m_fft_size = fft_size;

    // Note that increasing fftSize does mean that the current time-domain data must be expanded
    // to include past frames that it previously did not. This means that the AnalyserNode
    // effectively MUST keep around the last 32768 sample-frames and the current time-domain
    // data is the most recent fftSize sample-frames out of that.
    // NB: m_history always spans MAX_FFT_SIZE frames, so this holds by construction.
}

WebIDL::ExceptionOr<void> AnalyserNode::set_fft_size(unsigned long fft_size)
{
    if (fft_size < 32 || fft_size > MAX_FFT_SIZE || !is_power_of_two(fft_size))
        return WebIDL::IndexSizeError::create("Analyser node fftSize not a power of 2 between 32 and 32768"_utf16);

    set_fft_size_without_validation(fft_size);

    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_max_decibels(double max_decibels)
{
    if (m_min_decibels >= max_decibels)
        return WebIDL::IndexSizeError::create("Analyser node minDecibels greater than maxDecibels"_utf16);
    m_max_decibels = max_decibels;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_min_decibels(double min_decibels)
{
    if (min_decibels >= m_max_decibels)
        return WebIDL::IndexSizeError::create("Analyser node minDecibels greater than maxDecibels"_utf16);

    m_min_decibels = min_decibels;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_smoothing_time_constant(double smoothing_time_constant)
{
    if (smoothing_time_constant > 1.0 || smoothing_time_constant < 0.0)
        return WebIDL::IndexSizeError::create("Analyser node smoothingTimeConstant not between 0.0 and 1.0"_utf16);

    m_smoothing_time_constant = smoothing_time_constant;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::validate_options(AnalyserOptions const& options)
{
    if (options.min_decibels >= options.max_decibels)
        return WebIDL::IndexSizeError::create("Analyser node minDecibels greater than maxDecibels"_utf16);

    if (options.smoothing_time_constant > 1.0 || options.smoothing_time_constant < 0.0)
        return WebIDL::IndexSizeError::create("Analyser node smoothingTimeConstant not between 0.0 and 1.0"_utf16);

    if (options.fft_size < 32 || options.fft_size > MAX_FFT_SIZE || !is_power_of_two(options.fft_size))
        return WebIDL::IndexSizeError::create("Analyser node fftSize not a power of 2 between 32 and 32768"_utf16);

    return {};
}

WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> AnalyserNode::create_for_constructor(GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
{
    TRY(validate_options(options));
    return create(context, options);
}

}
