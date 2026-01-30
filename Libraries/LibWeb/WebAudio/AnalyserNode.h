/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

class OfflineAudioContext;
class RenderGraph;

// https://webaudio.github.io/web-audio-api/#AnalyserOptions
struct AnalyserOptions : AudioNodeOptions {
    unsigned long fft_size { 2048 };
    double max_decibels { -30 };
    double min_decibels { -100 };
    double smoothing_time_constant { 0.8 };
};

// https://webaudio.github.io/web-audio-api/#AnalyserNode
class AnalyserNode : public AudioNode {
    WEB_PLATFORM_OBJECT(AnalyserNode, AudioNode);
    GC_DECLARE_ALLOCATOR(AnalyserNode);

public:
    virtual ~AnalyserNode() override;

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> get_float_frequency_data(GC::Root<WebIDL::BufferSource> const& array);   // Float32Array
    WebIDL::ExceptionOr<void> get_byte_frequency_data(GC::Root<WebIDL::BufferSource> const& array);    // Uint8Array
    WebIDL::ExceptionOr<void> get_float_time_domain_data(GC::Root<WebIDL::BufferSource> const& array); // Float32Array
    WebIDL::ExceptionOr<void> get_byte_time_domain_data(GC::Root<WebIDL::BufferSource> const& array);  // Uint8Array

    unsigned long fft_size() const { return m_fft_size; }
    unsigned long frequency_bin_count() const { return m_fft_size / 2; }
    double max_decibels() const { return m_max_decibels; }
    double min_decibels() const { return m_min_decibels; }
    double smoothing_time_constant() const { return m_smoothing_time_constant; }

    WebIDL::ExceptionOr<void> set_fft_size(unsigned long);
    WebIDL::ExceptionOr<void> set_max_decibels(double);
    WebIDL::ExceptionOr<void> set_min_decibels(double);
    WebIDL::ExceptionOr<void> set_smoothing_time_constant(double);

    static WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, AnalyserOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, AnalyserOptions const& = {});

protected:
    AnalyserNode(JS::Realm&, GC::Ref<BaseAudioContext>, AnalyserOptions const& = {});

    virtual void initialize(JS::Realm&) override;

private:
    unsigned long m_fft_size;
    double m_max_decibels;
    double m_min_decibels;
    double m_smoothing_time_constant;

    // https://webaudio.github.io/web-audio-api/#current-frequency-data
    Vector<f32> const& current_frequency_data();

    // https://webaudio.github.io/web-audio-api/#current-time-domain-data
    Vector<f32> const& current_time_domain_data();
    Vector<f32> capture_time_domain_window() const;

    bool try_update_time_domain_cache_from_context(u64& out_render_quantum_index);
    bool try_update_frequency_cache_from_context(u64& out_render_quantum_index);

    // Cached results for the current render quantum (single block of sample frames)
    size_t current_render_quantum_index() const;
    Optional<size_t> m_cached_render_quantum_index;
    Vector<f32> m_cached_time_domain_data;
    Vector<f32> m_cached_frequency_data;
    Optional<size_t> m_realtime_analyser_index;

    friend class RenderGraph;
    void set_analyser_index(Badge<RenderGraph>, size_t index) { m_realtime_analyser_index = index; }
    friend class OfflineAudioContext;

    // Rendered input signal produced by the normal WebAudio graph rendering path.
    // This is the control-thread copy of the render-thread ring buffer, containing the most recent fftSize frames.
    void set_time_domain_data_from_rendering(Badge<OfflineAudioContext>, ReadonlySpan<f32>);

    // Rendered analyser output produced by the render thread.
    // This sets cached values so repeated get*FrequencyData() calls within the same render quantum are stable,
    // and so smoothing reflects render-thread state progression.
    void set_analysis_data_from_rendering(Badge<OfflineAudioContext>, ReadonlySpan<f32> time_domain, ReadonlySpan<f32> frequency_data_db, size_t render_quantum_index);
    Optional<Vector<f32>> m_rendered_time_domain_data;
};

}
