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
Vector<f32> AnalyserNode::current_time_domain_data()
{
    dbgln("FIXME: Analyser node: implement current time domain data");
    // The input signal must be down-mixed to mono as if channelCount is 1, channelCountMode is "max" and channelInterpretation is "speakers".
    // This is independent of the settings for the AnalyserNode itself.
    // The most recent fftSize frames are used for the down-mixing operation.

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

// https://webaudio.github.io/web-audio-api/#fourier-transform
static Vector<f32> apply_a_fourier_transform(Vector<f32> const& input)
{
    dbgln("FIXME: Analyser node: implement apply a fourier transform");
    auto result = Vector<f32>();
    result.resize(input.size());
    return result;
}

// https://webaudio.github.io/web-audio-api/#smoothing-over-time
Vector<f32> AnalyserNode::smoothing_over_time(Vector<f32> const& current_block)
{
    auto X = apply_a_fourier_transform(current_block);

    // FIXME: Naive
    Vector<f32> result;
    result.ensure_capacity(m_fft_size);
    for (unsigned long i = 0; i < m_fft_size; i++) {
        // FIMXE: Complex modulus on X[i]
        result.unchecked_append(m_smoothing_time_constant * m_previous_block[i] + (1.f - m_smoothing_time_constant) * abs(X[i]));
    }

    m_previous_block = result;

    return result;
}

// https://webaudio.github.io/web-audio-api/#conversion-to-db
Vector<f32> AnalyserNode::conversion_to_dB(Vector<f32> const& X_hat) const
{
    Vector<f32> result;
    result.ensure_capacity(X_hat.size());
    // FIXME: Naive
    for (auto x : X_hat)
        result.unchecked_append(20.0f * AK::log(x));

    return result;
}

// https://webaudio.github.io/web-audio-api/#current-frequency-data
Vector<f32> AnalyserNode::current_frequency_data()
{
    // 1. Compute the current time-domain data.
    auto current_time_domain_dat = current_time_domain_data();

    // 2. Apply a Blackman window to the time domain input data.
    auto blackman_windowed_input = apply_a_blackman_window(current_time_domain_dat);

    // 3. Apply a Fourier transform to the windowed time domain input data to get real and imaginary frequency data.
    auto frequency_domain_dat = apply_a_fourier_transform(blackman_windowed_input);

    // 4. Smooth over time the frequency domain data.
    auto smoothed_data = smoothing_over_time(frequency_domain_dat);

    // 5. Convert to dB.
    return conversion_to_dB(smoothed_data);
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloatfrequencydata
WebIDL::ExceptionOr<void> AnalyserNode::get_float_frequency_data(GC::Root<WebIDL::BufferSource> const& array)
{

    // Write the current frequency data into array. If array has fewer elements than the frequencyBinCount,
    // the excess elements will be dropped. If array has more elements than the frequencyBinCount, the
    // excess elements will be ignored. The most recent fftSize frames are used in computing the frequency data.
    auto const frequency_data = current_frequency_data();

    // FIXME: If another call to getFloatFrequencyData() or getByteFrequencyData() occurs within the same render
    // quantum as a previous call, the current frequency data is not updated with the same data. Instead, the
    // previously computed data is returned.

    auto& vm = this->vm();

    if (!is<JS::Float32Array>(*array->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");
    auto& output_array = static_cast<JS::Float32Array&>(*array->raw_object());

    size_t floats_to_write = min(output_array.data().size(), frequency_bin_count());
    for (size_t i = 0; i < floats_to_write; i++) {
        output_array.data()[i] = frequency_data[i];
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytefrequencydata
WebIDL::ExceptionOr<void> AnalyserNode::get_byte_frequency_data(GC::Root<WebIDL::BufferSource> const& array)
{
    // FIXME: If another call to getByteFrequencyData() or getFloatFrequencyData() occurs within the same render
    // quantum as a previous call, the current frequency data is not updated with the same data. Instead,
    // the previously computed data is returned.
    //      Need to implement some kind of blocking mechanism, I guess
    //      Might be more obvious how to handle this when render quantua have some
    //      more scaffolding
    //

    // current_frequency_data returns a vector of size m_fftSize
    // FIXME: Ensure sizes are correct after the fourier transform is implemented
    //        Spec says to write frequencyBinCount bytes, not fftSize
    Vector<f32> dB_data = current_frequency_data();
    Vector<u8> byte_data;
    byte_data.ensure_capacity(dB_data.size());

    // For getByteFrequencyData(), the 𝑌[𝑘] is clipped to lie between minDecibels and maxDecibels
    // and then scaled to fit in an unsigned byte such that minDecibels is represented by the
    // value 0 and maxDecibels is represented by the value 255.
    // FIXME: Naive
    f32 delta_dB = m_max_decibels - m_min_decibels;
    for (auto x : dB_data) {
        x = max(x, m_min_decibels);
        x = min(x, m_max_decibels);

        byte_data.unchecked_append(static_cast<u8>(255 * (x - m_min_decibels) / delta_dB));
    }

    // Write the current frequency data into array. If array’s byte length is less than frequencyBinCount,
    // the excess elements will be dropped. If array’s byte length is greater than the frequencyBinCount ,
    // the excess elements will be ignored. The most recent fftSize frames are used in computing the frequency data.
    auto& output_buffer = array->viewed_array_buffer()->buffer();
    size_t bytes_to_write = min(array->byte_length(), frequency_bin_count());

    for (size_t i = 0; i < bytes_to_write; i++)
        output_buffer[i] = byte_data[i];

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloattimedomaindata
WebIDL::ExceptionOr<void> AnalyserNode::get_float_time_domain_data(GC::Root<WebIDL::BufferSource> const& array)
{
    // Write the current time-domain data (waveform data) into array. If array has fewer elements than the
    // value of fftSize, the excess elements will be dropped. If array has more elements than the value of
    // fftSize, the excess elements will be ignored. The most recent fftSize frames are written (after downmixing).

    Vector<f32> time_domain_data = current_time_domain_data();

    auto& vm = this->vm();

    if (!is<JS::Float32Array>(*array->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");
    auto& output_array = static_cast<JS::Float32Array&>(*array->raw_object());

    size_t floats_to_write = min(output_array.data().size(), frequency_bin_count());
    for (size_t i = 0; i < floats_to_write; i++) {
        output_array.data()[i] = time_domain_data[i];
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytetimedomaindata
WebIDL::ExceptionOr<void> AnalyserNode::get_byte_time_domain_data(GC::Root<WebIDL::BufferSource> const& array)
{
    // Write the current time-domain data (waveform data) into array. If array’s byte length is less than
    // fftSize, the excess elements will be dropped. If array’s byte length is greater than the fftSize,
    // the excess elements will be ignored. The most recent fftSize frames are used in computing the byte data.

    Vector<f32> time_domain_data = current_time_domain_data();
    VERIFY(time_domain_data.size() == m_fft_size);

    Vector<u8> byte_data;
    byte_data.ensure_capacity(m_fft_size);

    // FIXME: Naive
    for (size_t i = 0; i < m_fft_size; i++) {
        auto x = 128 * (1 + time_domain_data[i]);
        x = max(x, 0);
        x = min(x, 255);
        byte_data.unchecked_append(static_cast<u8>(x));
    }

    auto& output_buffer = array->viewed_array_buffer()->buffer();
    size_t bytes_to_write = min(array->byte_length(), fft_size());

    for (size_t i = 0; i < bytes_to_write; i++)
        output_buffer[i] = byte_data[i];

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-fftsize
WebIDL::ExceptionOr<void> AnalyserNode::set_fft_size(unsigned long fft_size)
{
    if (fft_size < 32 || fft_size > 32768 || !is_power_of_two(fft_size))
        return WebIDL::IndexSizeError::create(realm(), "Analyser node fftSize not a power of 2 between 32 and 32768"_string);

    // reset previous block to 0s
    m_previous_block = Vector<f32>();
    m_previous_block.resize(fft_size);

    m_fft_size = fft_size;

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
        return WebIDL::IndexSizeError::create(realm(), "Analyser node minDecibels greater than maxDecibels"_string);
    m_max_decibels = max_decibels;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_min_decibels(double min_decibels)
{
    if (min_decibels >= m_max_decibels)
        return WebIDL::IndexSizeError::create(realm(), "Analyser node minDecibels greater than maxDecibels"_string);

    m_min_decibels = min_decibels;
    return {};
}

WebIDL::ExceptionOr<void> AnalyserNode::set_smoothing_time_constant(double smoothing_time_constant)
{
    if (smoothing_time_constant > 1.0 || smoothing_time_constant < 0.0)
        return WebIDL::IndexSizeError::create(realm(), "Analyser node smoothingTimeConstant not between 0.0 and 1.0"_string);

    m_smoothing_time_constant = smoothing_time_constant;
    return {};
}

WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> AnalyserNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AnalyserOptions const& options)
{
    if (options.min_decibels >= options.max_decibels)
        return WebIDL::IndexSizeError::create(realm, "Analyser node minDecibels greater than maxDecibels"_string);

    if (options.smoothing_time_constant > 1.0 || options.smoothing_time_constant < 0.0)
        return WebIDL::IndexSizeError::create(realm, "Analyser node smoothingTimeConstant not between 0.0 and 1.0"_string);

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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnalyserNode);
}

}
