/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

using AnalyserOptions = Bindings::AnalyserOptions;

// https://webaudio.github.io/web-audio-api/#AnalyserNode
class AnalyserNode : public AudioNode {
    WEB_WRAPPABLE(AnalyserNode, AudioNode);
    GC_DECLARE_ALLOCATOR(AnalyserNode);

public:
    virtual ~AnalyserNode() override;

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> get_float_frequency_data(GC::Ref<JS::Float32Array>);
    WebIDL::ExceptionOr<void> get_byte_frequency_data(GC::Ref<JS::Uint8Array>);
    WebIDL::ExceptionOr<void> get_float_time_domain_data(GC::Ref<JS::Float32Array>);
    WebIDL::ExceptionOr<void> get_byte_time_domain_data(GC::Ref<JS::Uint8Array>);

    unsigned long fft_size() const { return m_fft_size; }
    unsigned long frequency_bin_count() const { return m_fft_size / 2; }
    double max_decibels() const { return m_max_decibels; }
    double min_decibels() const { return m_min_decibels; }
    double smoothing_time_constant() const { return m_smoothing_time_constant; }

    WebIDL::ExceptionOr<void> set_fft_size(unsigned long);
    WebIDL::ExceptionOr<void> set_max_decibels(double);
    WebIDL::ExceptionOr<void> set_min_decibels(double);
    WebIDL::ExceptionOr<void> set_smoothing_time_constant(double);

    static WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> create(GC::Ref<BaseAudioContext>, AnalyserOptions const& = {});
    static WebIDL::ExceptionOr<void> validate_options(AnalyserOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> create_for_constructor(GC::Ref<BaseAudioContext>, AnalyserOptions const& = {});

protected:
    AnalyserNode(GC::Ref<BaseAudioContext>, AnalyserOptions const& = {});

private:
    unsigned long m_fft_size;
    double m_max_decibels;
    double m_min_decibels;
    double m_smoothing_time_constant;

    void set_fft_size_without_validation(unsigned long);

    // https://webaudio.github.io/web-audio-api/#current-frequency-data
    Vector<f32> current_frequency_data();

    // https://webaudio.github.io/web-audio-api/#current-time-domain-data
    Vector<f32> current_time_domain_data();

    // https://webaudio.github.io/web-audio-api/#blackman-window
    Vector<f32> apply_a_blackman_window(Vector<f32> const& x) const;

    // https://webaudio.github.io/web-audio-api/#smoothing-over-time
    Vector<f32> smoothing_over_time(Vector<f32> const& current_block);

    // https://webaudio.github.io/web-audio-api/#previous-block
    Vector<f32> m_previous_block;

    // https://webaudio.github.io/web-audio-api/#conversion-to-db
    Vector<f32> conversion_to_dB(Vector<f32> const& X_hat) const;
};

}
