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
Vector<f32> AnalyserNode::compute_time_domain_data() const
{
    dbgln("FIXME: Analyser node: implement current time domain data");
    // The input signal must be down-mixed to mono as if channelCount is 1, channelCountMode is "max" and channelInterpretation is "speakers".
    // This is independent of the settings for the AnalyserNode itself.
    // The most recent fftSize frames are used for the down-mixing operation.

    // FIXME: Remove this temporary Internals testing hook once it's no longer needed.
    if (m_testing_time_domain_data.has_value())
        return m_testing_time_domain_data.value();

    // FIXME: definition of "input signal" above unclear
    //        need to implement up/down mixing somewhere
    //        https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
    Vector<f32> result;
    result.resize(m_fft_size);
    return result;
}

// https://webaudio.github.io/web-audio-api/#blackman-window
Vector<f32> AnalyserNode::apply_a_blackman_window(Vector<f32> const& x) const
{
    f32 const a = 0.16;
    f32 const a0 = 0.5f * (1 - a);
    f32 const a1 = 0.5;
    f32 const a2 = a * 0.5f;
    f32 const N = static_cast<f32>(m_fft_size);

    auto w = [&](f32 n) {
        return a0 - (a1 * cos(2 * AK::Pi<f32> * n / N)) + (a2 * cos(4 * AK::Pi<f32> * n / N));
    };

    Vector<f32> x_hat;
    x_hat.resize(m_fft_size);

    // FIXME: Naive
    for (unsigned long i = 0; i < m_fft_size; i++) {
        x_hat[i] = x[i] * w(static_cast<f32>(i));
    }

    return x_hat;
}

// https://webaudio.github.io/web-audio-api/#fourier-transform
Vector<AnalyserNode::ComplexBin> AnalyserNode::apply_a_fourier_transform(Vector<f32> const& input) const
{
    // This function was adapted from pseudocode for the Cooley–Tukey algorithm at:
    // https://en.wikipedia.org/wiki/Cooley%E2%80%93Tukey_FFT_algorithm#Data_reordering,_bit_reversal,_and_in-place_algorithms
    size_t const N = input.size();
    if (N == 0)
        return {};

    VERIFY(is_power_of_two(N));

    Vector<f32> real;
    Vector<f32> imaginary;
    real.ensure_capacity(N);
    imaginary.ensure_capacity(N);
    for (size_t i = 0; i < N; ++i) {
        real.unchecked_append(input[i]);
        imaginary.unchecked_append(0.0f);
    }

    // Bit-reverse copy
    for (size_t i = 1, j = 0; i < N; ++i) {
        size_t bit = N >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;

        if (i < j) {
            AK::swap(real[i], real[j]);
            AK::swap(imaginary[i], imaginary[j]);
        }
    }

    // Iterative forward transform
    for (size_t n = N, s = 1; n > 1; n >>= 1, ++s) {
        size_t const m = 1u << s;
        size_t const half_m = m >> 1;

        // wm = exp(-2πi / m)
        f32 const angle = -2.0f * AK::Pi<f32> / static_cast<f32>(m);
        f32 const wm_cos = cos(angle);
        f32 const wm_sin = sin(angle);

        for (size_t k = 0; k < N; k += m) {
            // w = 1
            f32 w_cos = 1.0f;
            f32 w_sin = 0.0f;

            for (size_t j = 0; j < half_m; ++j) {
                size_t const u_index = k + j;
                size_t const v_index = u_index + half_m;

                // t = w * A[k + j + m/2]
                f32 const t_real = (real[v_index] * w_cos) - (imaginary[v_index] * w_sin);
                f32 const t_imaginary = (real[v_index] * w_sin) + (imaginary[v_index] * w_cos);

                // u = A[k + j]
                f32 const u_real = real[u_index];
                f32 const u_imaginary = imaginary[u_index];

                // A[k + j] = u + t
                real[u_index] = u_real + t_real;
                imaginary[u_index] = u_imaginary + t_imaginary;

                // A[k + j + m/2] = u - t
                real[v_index] = u_real - t_real;
                imaginary[v_index] = u_imaginary - t_imaginary;

                // w = w * wm
                f32 const next_w_cos = (w_cos * wm_cos) - (w_sin * wm_sin);
                f32 const next_w_sin = (w_cos * wm_sin) + (w_sin * wm_cos);
                w_cos = next_w_cos;
                w_sin = next_w_sin;
            }
        }
    }

    size_t const bin_count = min(frequency_bin_count(), N / 2);

    Vector<ComplexBin> zipped;
    zipped.ensure_capacity(bin_count);
    for (size_t i = 0; i < bin_count; ++i)
        zipped.unchecked_append(ComplexBin { .real = real[i], .imaginary = imaginary[i] });

    return zipped;
}

// https://webaudio.github.io/web-audio-api/#smoothing-over-time
Vector<f32> AnalyserNode::smoothing_over_time(Vector<ComplexBin> const& current_block)
{
    Vector<f32> result;
    size_t const bin_count = frequency_bin_count();
    result.ensure_capacity(bin_count);
    m_previous_block.resize(bin_count);

    size_t const N = min(current_block.size(), bin_count);
    for (size_t i = 0; i < N; ++i) {
        f32 const magnitude = AK::sqrt((current_block[i].real * current_block[i].real) + (current_block[i].imaginary * current_block[i].imaginary));
        result.unchecked_append((m_smoothing_time_constant * m_previous_block[i]) + ((1.f - m_smoothing_time_constant) * magnitude));
    }
    for (size_t i = N; i < bin_count; ++i) {
        result.unchecked_append(0.0f);
    }
    m_previous_block = result;

    return result;
}

// https://webaudio.github.io/web-audio-api/#conversion-to-db
static Vector<f32> conversion_to_dB(Vector<f32> const& X_hat)
{
    Vector<f32> result;
    result.ensure_capacity(X_hat.size());
    // FIXME: Naive
    for (f32 x : X_hat) {
        if (x <= 0.0f || __builtin_isnan(x))
            result.unchecked_append(-AK::Infinity<f32>);
        else
            result.unchecked_append(20.0f * AK::log10(x));
    }

    return result;
}

// https://webaudio.github.io/web-audio-api/#current-frequency-data
Vector<f32> const& AnalyserNode::current_frequency_data()
{
    // 1. Compute the current time-domain data.
    // NOTE: The spec requires that multiple calls to get*FrequencyData() within the same render quantum
    // return the same values. We cache for this keyed on the audio context's currentTime.

    if (auto quantum_index = current_render_quantum_index();
        !m_cached_render_quantum_index.has_value()
        || m_cached_render_quantum_index.value() != quantum_index
        || m_cached_time_domain_data.size() != m_fft_size) {

        m_cached_render_quantum_index = quantum_index;
        m_cached_time_domain_data = compute_time_domain_data();
        m_cached_frequency_data.clear();
    }

    if (m_cached_frequency_data.size() == frequency_bin_count())
        return m_cached_frequency_data;

    auto const& current_time_domain_dat = current_time_domain_data();

    // 2. Apply a Blackman window to the time domain input data.
    auto blackman_windowed_input = apply_a_blackman_window(current_time_domain_dat);

    // 3. Apply a Fourier transform to the windowed time domain input data to get real and imaginary frequency data.
    auto frequency_domain_dat = apply_a_fourier_transform(blackman_windowed_input);

    // 4. Smooth over time the frequency domain data.
    auto smoothed_data = smoothing_over_time(frequency_domain_dat);

    // 5. Convert to dB.
    m_cached_frequency_data = conversion_to_dB(smoothed_data);
    return m_cached_frequency_data;
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

// https://webaudio.github.io/web-audio-api/#dom-analysernode-fftsize
WebIDL::ExceptionOr<void> AnalyserNode::set_fft_size(unsigned long fft_size)
{
    if (fft_size < 32 || fft_size > 32768 || !is_power_of_two(fft_size))
        return WebIDL::IndexSizeError::create(realm(), "Analyser node fftSize not a power of 2 between 32 and 32768"_utf16);

    // reset previous block to 0s
    m_previous_block = Vector<f32>();
    m_fft_size = fft_size;
    m_previous_block.resize(frequency_bin_count());
    m_cached_render_quantum_index.clear();
    m_cached_time_domain_data.clear();
    m_cached_frequency_data.clear();

    // FIXME: Check this:
    // Note that increasing fftSize does mean that the current time-domain data must be expanded
    // to include past frames that it previously did not. This means that the AnalyserNode
    // effectively MUST keep around the last 32768 sample-frames and the current time-domain
    // data is the most recent fftSize sample-frames out of that.
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

    size_t quantum_size = BaseAudioContext::render_quantum_size();
    if (quantum_size == 0)
        return 0;

    double frames = context->current_time() * sample_rate;
    if (frames <= 0)
        return 0;

    return static_cast<size_t>(frames) / quantum_size;
}

// Look up or compute the cached data for our current sample block.
Vector<f32> const& AnalyserNode::current_time_domain_data()
{
    size_t quantum_index = current_render_quantum_index();
    if (!m_cached_render_quantum_index.has_value() || m_cached_render_quantum_index.value() != quantum_index || m_cached_time_domain_data.size() != m_fft_size) {
        m_cached_render_quantum_index = quantum_index;
        m_cached_time_domain_data = compute_time_domain_data();
        m_cached_frequency_data.clear();
    }
    return m_cached_time_domain_data;
}

// Temporary for JS testing without a full audio pipeline.
void AnalyserNode::set_time_domain_data_for_testing(Badge<Internals::Internals>, ReadonlySpan<f32> input)
{
    m_testing_time_domain_data = Vector<f32> {};
    m_testing_time_domain_data->resize(m_fft_size);

    // If there's too much, use the tail.
    size_t to_copy = min(input.size(), m_fft_size);
    for (size_t i = 0; i < to_copy; ++i) {
        m_testing_time_domain_data->at(m_fft_size - to_copy + i) = input[input.size() - to_copy + i];
    }
    m_cached_render_quantum_index.clear();
    m_cached_time_domain_data.clear();
    m_cached_frequency_data.clear();
}

}
