/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;

use ak::Utf16String;
use ak::math;
use libjs_rust::api::{
    ArrayBufferOrder, TypedArrayBase, is_typed_array_out_of_bounds, make_typed_array_with_buffer_witness_record,
    typed_array_length,
};
use libjs_rust::api::{Gc, Trace};
use libweb_rust::web_audio::control_message::FfiSharedCppObject;
use libweb_rust::web_audio::rendering::analyser_render_node::create_analyser_render_node;
use libweb_rust::web_audio::rendering::audio_frame_ring::{AudioFrameRing, AudioFrameRingCallbacks};
use libweb_rust::web_audio::rendering::fft::{FFT, FFTDirection};
use libweb_rust::web_audio::rendering::node_message::SharedCppObject;

use super::audio_array::overwrite_float32_array;
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

// https://webaudio.github.io/web-audio-api/#AnalyserNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AnalyserNode {
    node: AudioNode,
    fft_size: Cell<u32>,
    #[gc(untraced)]
    fft: RefCell<Option<FFT>>,
    max_decibels: Cell<f64>,
    min_decibels: Cell<f64>,
    smoothing_time_constant: Cell<f64>,

    // https://webaudio.github.io/web-audio-api/#previous-block
    #[gc(untraced)]
    previous_block: RefCell<Vec<f32>>,
    #[gc(untraced)]
    time_domain_ring: RefCell<Option<AudioFrameRing>>,
    #[gc(untraced)]
    history: RefCell<Vec<f32>>,
    history_write_index: Cell<usize>,
    history_valid_frames: Cell<usize>,

    // Frequency data computed within the current render quantum, per the spec's requirement
    // that repeated getter calls in one quantum return identical data.
    #[gc(untraced)]
    cached_frequency_data: RefCell<Vec<f32>>,
    frequency_data_cache_time: Cell<Option<f64>>,
}

impl Inherits<AudioNode> for AnalyserNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for AnalyserNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        node.node.finalize_node();
    }
}

impl GetTheParent for AnalyserNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

// https://webaudio.github.io/web-audio-api/#dom-analysernode-fftsize
// The maximum allowed fftSize; the node must keep this many past sample-frames around so
// that growing fftSize immediately exposes older data.
const MAX_FFT_SIZE: u32 = 32768;

pub(crate) struct AnalyserOptions {
    pub parent: AudioNodeOptions,
    pub fft_size: u32,
    pub max_decibels: f64,
    pub min_decibels: f64,
    pub smoothing_time_constant: f64,
}

unsafe extern "C" {
    fn web_audio_analyser_create_ring(callbacks: *mut AudioFrameRingCallbacks) -> FfiSharedCppObject;
}

impl AnalyserNode {
    pub(crate) fn create(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: &AnalyserOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
        // MUST initialize the AudioNode this, with context and options as arguments.
        let node = allocate(cx.vm(), |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            fft_size: Cell::new(options.fft_size),
            fft: RefCell::new(None),
            max_decibels: Cell::new(options.max_decibels),
            min_decibels: Cell::new(options.min_decibels),
            smoothing_time_constant: Cell::new(options.smoothing_time_constant),
            previous_block: RefCell::new(Vec::new()),
            time_domain_ring: RefCell::new(None),
            history: RefCell::new(Vec::new()),
            history_write_index: Cell::new(0),
            history_valid_frames: Cell::new(0),
            cached_frequency_data: RefCell::new(Vec::new()),
            frequency_data_cache_time: Cell::new(None),
        });
        node.node.set_this(AnyAudioNode::AnalyserNode(node));
        node.node.event_target().set_owner(node);
        node.set_fft_size_without_validation(options.fft_size);

        // Default options for channel count and interpretation
        // https://webaudio.github.io/web-audio-api/#AnalyserNode
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };

        // FIXME: Set tail-time to no
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;
        node.history.borrow_mut().resize(MAX_FFT_SIZE as usize, 0.);
        let mut callbacks = std::mem::MaybeUninit::uninit();
        let ring = unsafe { web_audio_analyser_create_ring(callbacks.as_mut_ptr()) };
        let callbacks = unsafe { callbacks.assume_init() };
        *node.time_domain_ring.borrow_mut() = Some(AudioFrameRing {
            owner: SharedCppObject {
                pointer: ring.pointer,
                retain: ring.retain,
                release: ring.release,
            },
            callbacks,
        });
        node.node.queue_render_node_creation(unsafe {
            create_analyser_render_node(
                node.node.node_id(),
                BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
                ring,
                callbacks,
            )
        });
        Ok(node)
    }

    #[allow(clippy::needless_range_loop, reason = "preserve the C++ frame traversal")]
    fn drain_time_domain_ring(&self) {
        let ring = self.time_domain_ring.borrow();
        let Some(ring) = &*ring else {
            return;
        };

        // Ring pops are destructive, so everything the render thread has produced is folded into
        // the control-side history of the last MAX_FFT_SIZE sample-frames. Keeping the full
        // maximum window around means growing fftSize immediately exposes older data, as the
        // spec requires.
        let mut chunk = [0.; 1024];
        loop {
            let frame_count = ring.try_pop(&mut chunk);
            if frame_count == 0 {
                break;
            }
            let mut history = self.history.borrow_mut();
            for frame in 0..frame_count {
                history[self.history_write_index.get()] = chunk[frame];
                self.history_write_index
                    .set((self.history_write_index.get() + 1) % history.len());
            }
            self.history_valid_frames
                .set(history.len().min(self.history_valid_frames.get() + frame_count));
        }
    }

    // https://webaudio.github.io/web-audio-api/#current-time-domain-data
    fn current_time_domain_data(&self) -> Vec<f32> {
        // The input signal must be down-mixed to mono as if channelCount is 1, channelCountMode is "max" and
        // channelInterpretation is "speakers". This is independent of the settings for the AnalyserNode itself.
        // NB: The rendering thread down-mixes samples before writing them to the history ring.
        self.drain_time_domain_ring();

        // The most recent fftSize frames are used for the down-mixing operation.
        let mut result = vec![0.; self.fft_size.get() as usize];
        let frames_available = (self.fft_size.get() as usize).min(self.history_valid_frames.get());
        let history = self.history.borrow();
        for frame in 0..frames_available {
            let history_index =
                (self.history_write_index.get() + history.len() - frames_available + frame) % history.len();
            result[self.fft_size.get() as usize - frames_available + frame] = history[history_index];
        }
        result
    }

    // https://webaudio.github.io/web-audio-api/#blackman-window
    fn apply_a_blackman_window(&self, x: &[f32]) -> Vec<f32> {
        let a: f32 = 0.16;
        let a0 = 0.5f32 * (1. - a);
        let a1: f32 = 0.5;
        let a2 = a * 0.5f32;
        let n = self.fft_size.get();
        let w = |index: u32| {
            a0 - a1 * math::cos(2. * std::f32::consts::PI * index as f32 / n as f32)
                + a2 * math::cos(4. * std::f32::consts::PI * index as f32 / n as f32)
        };
        let mut x_hat = vec![0.; self.fft_size.get() as usize];

        // FIXME: Naive
        for i in 0..self.fft_size.get() as usize {
            x_hat[i] = x[i] * w(i as u32);
        }
        x_hat
    }

    // https://webaudio.github.io/web-audio-api/#smoothing-over-time
    fn smoothing_over_time(&self, current_block: &[f32]) -> Vec<f32> {
        // 1. Let X^−1[k] be the result of this operation on the previous block. The previous block is defined as being
        //    the buffer computed by the previous smoothing over time operation, or an array of N zeros if this is the
        //    first time we are smoothing over time.
        let mut previous_block = self.previous_block.borrow_mut();
        if previous_block.len() != current_block.len() {
            previous_block.clear();
            previous_block.resize(current_block.len(), 0.);
        }

        // 2. Let τ be the value of the smoothingTimeConstant attribute for this AnalyserNode.
        let tau = self.smoothing_time_constant.get() as f32;

        // 3. Let X[k] be the result of applying a Fourier transform of the current block.
        let mut result = vec![0.; current_block.len()];

        // 4. Then the smoothed value, X^[k], is computed by X^[k] = τ * X^−1[k] + (1 − τ) * |X[k]|
        //    If X^[k] is NaN, positive infinity or negative infinity, set X^[k] = 0.
        for i in 0..current_block.len() {
            let mut smoothed = tau * previous_block[i] + (1.0f32 - tau) * current_block[i].abs();
            if !smoothed.is_finite() {
                smoothed = 0.;
            }
            result[i] = smoothed;
        }
        *previous_block = result.clone();
        result
    }

    // https://webaudio.github.io/web-audio-api/#conversion-to-db
    fn conversion_to_db(&self, x_hat: &[f32]) -> Vec<f32> {
        // Y[k] = 20 log10 X^[k]   for k = 0,…,N−1
        let mut result = Vec::with_capacity(x_hat.len());
        for x in x_hat {
            // NB: 20 * log10(0) is -∞. The byte-domain conversion clamps this later.
            if *x > 0. {
                result.push(20.0f32 * math::log10(*x));
            } else {
                result.push(f32::NEG_INFINITY);
            }
        }
        result
    }

    // https://webaudio.github.io/web-audio-api/#current-frequency-data
    fn current_frequency_data(&self) -> Vec<f32> {
        // If another call to getFloatFrequencyData() or getByteFrequencyData() occurs within the same render quantum
        // as a previous call, the current frequency data is not updated with the same data. Instead, the previously
        // computed data is returned.
        // AD-HOC: The control thread cannot observe render quantum boundaries directly; the context's currentTime,
        //         which advances as quanta are rendered, stands in for the quantum stamp.
        let current_time = self.node.context_value().current_time_value();
        if self.frequency_data_cache_time.get() == Some(current_time) {
            return self.cached_frequency_data.borrow().clone();
        }

        // 1. Compute the current time-domain data.
        let time_domain_data = self.current_time_domain_data();

        // 2. Apply a Blackman window to the time domain input data.
        let windowed_input = self.apply_a_blackman_window(&time_domain_data);

        // 3. Apply a Fourier transform to the windowed time domain input data to get real and imaginary frequency data.
        let frequency_domain_data = apply_a_fourier_transform(
            self.fft.borrow().as_ref().unwrap(),
            &windowed_input,
            self.fft_size.get() as usize / 2,
        );

        // 4. Smooth over time the frequency domain data.
        let smoothed_data = self.smoothing_over_time(&frequency_domain_data);

        // 5. Convert to dB.
        let result = self.conversion_to_db(&smoothed_data);
        *self.cached_frequency_data.borrow_mut() = result.clone();
        self.frequency_data_cache_time.set(Some(current_time));
        result
    }

    // https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloatfrequencydata
    pub(crate) fn get_float_frequency_data(&self, _: &mut CallContext, array: Gc<TypedArrayBase>) -> WebIdlResult<()> {
        // Write the current frequency data into array. If array has fewer elements than the frequencyBinCount,
        // the excess elements will be dropped. If array has more elements than the frequencyBinCount, the
        // excess elements will be ignored. The most recent fftSize frames are used in computing the frequency data.
        let data = self.current_frequency_data();
        let record = make_typed_array_with_buffer_witness_record(&array, ArrayBufferOrder::SeqCst);
        if is_typed_array_out_of_bounds(&record) {
            return Ok(());
        }
        let count = typed_array_length(&record).min(self.fft_size.get() as usize / 2);
        overwrite_float32_array(&array, &data[..count]);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytefrequencydata
    pub(crate) fn get_byte_frequency_data(&self, _: &mut CallContext, array: Gc<TypedArrayBase>) -> WebIdlResult<()> {
        let data = self.current_frequency_data();
        let mut byte_data = Vec::with_capacity(data.len());

        // For getByteFrequencyData(), the 𝑌[𝑘] is clipped to lie between minDecibels and maxDecibels
        // and then scaled to fit in an unsigned byte such that minDecibels is represented by the
        // value 0 and maxDecibels is represented by the value 255.
        let delta_db = (self.max_decibels.get() - self.min_decibels.get()) as f32;
        for mut x in data {
            x = if f64::from(x) < self.min_decibels.get() {
                self.min_decibels.get() as f32
            } else {
                x
            };
            x = if self.max_decibels.get() < f64::from(x) {
                self.max_decibels.get() as f32
            } else {
                x
            };
            byte_data.push((255. * (f64::from(x) - self.min_decibels.get()) / f64::from(delta_db)) as u8);
        }

        // Write the current frequency data into array. If array’s byte length is less than frequencyBinCount,
        // the excess elements will be dropped. If array’s byte length is greater than the frequencyBinCount ,
        // the excess elements will be ignored. The most recent fftSize frames are used in computing the frequency data.
        let record = make_typed_array_with_buffer_witness_record(&array, ArrayBufferOrder::SeqCst);
        if is_typed_array_out_of_bounds(&record) {
            return Ok(());
        }
        let count = typed_array_length(&record).min(self.fft_size.get() as usize / 2);
        array
            .viewed_array_buffer()
            .overwrite(array.byte_offset(), &byte_data[..count]);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-analysernode-getfloattimedomaindata
    pub(crate) fn get_float_time_domain_data(
        &self,
        _: &mut CallContext,
        array: Gc<TypedArrayBase>,
    ) -> WebIdlResult<()> {
        // Write the current time-domain data (waveform data) into array. If array has fewer elements than the
        // value of fftSize, the excess elements will be dropped. If array has more elements than the value of
        // fftSize, the excess elements will be ignored. The most recent fftSize frames are written (after downmixing).
        let data = self.current_time_domain_data();
        let record = make_typed_array_with_buffer_witness_record(&array, ArrayBufferOrder::SeqCst);
        if is_typed_array_out_of_bounds(&record) {
            return Ok(());
        }
        let count = typed_array_length(&record).min(self.fft_size.get() as usize);
        overwrite_float32_array(&array, &data[..count]);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-analysernode-getbytetimedomaindata
    pub(crate) fn get_byte_time_domain_data(&self, _: &mut CallContext, array: Gc<TypedArrayBase>) -> WebIdlResult<()> {
        // Write the current time-domain data (waveform data) into array. If array’s byte length is less than
        // fftSize, the excess elements will be dropped. If array’s byte length is greater than the fftSize,
        // the excess elements will be ignored. The most recent fftSize frames are used in computing the byte data.
        let data = self.current_time_domain_data();
        let mut byte_data = Vec::with_capacity(data.len());
        assert_eq!(data.len(), self.fft_size.get() as usize);
        for sample in data {
            let mut x = 128. * (1. + sample);
            x = if x < 0. { 0. } else { x };
            x = if 255. < x { 255. } else { x };
            byte_data.push(x as u8);
        }
        let record = make_typed_array_with_buffer_witness_record(&array, ArrayBufferOrder::SeqCst);
        if is_typed_array_out_of_bounds(&record) {
            return Ok(());
        }
        let count = typed_array_length(&record).min(self.fft_size.get() as usize);
        array
            .viewed_array_buffer()
            .overwrite(array.byte_offset(), &byte_data[..count]);
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-analysernode-fftsize
    fn set_fft_size_without_validation(&self, fft_size: u32) {
        // reset previous block to 0s
        *self.previous_block.borrow_mut() = vec![0.; fft_size as usize / 2];
        self.frequency_data_cache_time.set(None);
        self.fft_size.set(fft_size);
        *self.fft.borrow_mut() = Some(FFT::new(fft_size as usize));

        // Note that increasing fftSize does mean that the current time-domain data must be expanded
        // to include past frames that it previously did not. This means that the AnalyserNode
        // effectively MUST keep around the last 32768 sample-frames and the current time-domain
        // data is the most recent fftSize sample-frames out of that.
        // NB: The history ring holds MAX_FFT_SIZE frames.
    }

    #[allow(clippy::manual_range_contains, reason = "preserve the C++ validation")]
    pub(crate) fn set_fft_size(&self, _: &mut CallContext, fft_size: u32) -> WebIdlResult<()> {
        if fft_size < 32 || fft_size > MAX_FFT_SIZE || !fft_size.is_power_of_two() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node fftSize not a power of 2 between 32 and 32768"),
            ));
        }
        self.set_fft_size_without_validation(fft_size);
        Ok(())
    }

    pub(crate) fn set_max_decibels(&self, _: &mut CallContext, max_decibels: f64) -> WebIdlResult<()> {
        if self.min_decibels.get() >= max_decibels {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node minDecibels greater than maxDecibels"),
            ));
        }
        self.max_decibels.set(max_decibels);
        Ok(())
    }

    pub(crate) fn set_min_decibels(&self, _: &mut CallContext, min_decibels: f64) -> WebIdlResult<()> {
        if min_decibels >= self.max_decibels.get() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node minDecibels greater than maxDecibels"),
            ));
        }
        self.min_decibels.set(min_decibels);
        Ok(())
    }

    pub(crate) fn set_smoothing_time_constant(
        &self,
        _: &mut CallContext,
        smoothing_time_constant: f64,
    ) -> WebIdlResult<()> {
        if smoothing_time_constant > 1. || smoothing_time_constant < 0. {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node smoothingTimeConstant not between 0.0 and 1.0"),
            ));
        }
        self.smoothing_time_constant.set(smoothing_time_constant);
        Ok(())
    }

    pub(crate) fn validate_options(options: &AnalyserOptions) -> WebIdlResult<()> {
        if options.min_decibels >= options.max_decibels {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node minDecibels greater than maxDecibels"),
            ));
        }
        if options.smoothing_time_constant > 1. || options.smoothing_time_constant < 0. {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node smoothingTimeConstant not between 0.0 and 1.0"),
            ));
        }
        if options.fft_size < 32 || options.fft_size > MAX_FFT_SIZE || !options.fft_size.is_power_of_two() {
            return Err(Exception::Dom(
                "IndexSizeError",
                Utf16String::from_ascii(b"Analyser node fftSize not a power of 2 between 32 and 32768"),
            ));
        }
        Ok(())
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        options: AnalyserOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::validate_options(&options)?;
        Self::create(cx, context, &options)
    }

    pub(crate) fn fft_size(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.fft_size.get())
    }

    pub(crate) fn max_decibels(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.max_decibels.get())
    }

    pub(crate) fn min_decibels(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.min_decibels.get())
    }

    pub(crate) fn smoothing_time_constant(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.smoothing_time_constant.get())
    }

    pub(crate) fn frequency_bin_count(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.fft_size.get() / 2)
    }
}

// https://webaudio.github.io/web-audio-api/#fourier-transform
//
// Returns `bin_count` (= fftSize / 2) magnitude values normalized by fftSize, per the spec's
// `(1/N) * |X[k]|` convention. The caller is expected to have applied a window already.
#[allow(clippy::manual_memcpy, reason = "preserve the C++ input copy loop")]
fn apply_a_fourier_transform(fft: &FFT, input: &[f32], bin_count: usize) -> Vec<f32> {
    let mut magnitudes = vec![0.; bin_count];
    if input.is_empty() {
        return magnitudes;
    }
    let n = input.len();
    let mut re = vec![0.; n];
    let mut im = vec![0.; n];
    for i in 0..n {
        re[i] = input[i];
    }
    fft.transform(&mut re, &mut im, FFTDirection::Forward);
    let inv_n = 1.0f32 / n as f32;
    for k in 0..bin_count.min(n) {
        magnitudes[k] = (re[k] * re[k] + im[k] * im[k]).sqrt() * inv_n;
    }
    magnitudes
}
