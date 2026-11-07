/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Arc;

use ak::Span;

pub struct AudioBufferContents {
    pub channels: Vec<Vec<f32>>,
    pub sample_rate: f32,
}

impl AudioBufferContents {
    pub fn length(&self) -> usize {
        self.channels.first().map_or(0, Vec::len)
    }

    pub fn duration(&self) -> f64 {
        self.length() as f64 / f64::from(self.sample_rate)
    }
}

// An immutable snapshot of a PeriodicWave's Fourier coefficients.
// https://webaudio.github.io/web-audio-api/#PeriodicWave
pub struct PeriodicWaveData {
    pub real: Vec<f32>,
    pub imag: Vec<f32>,
    pub normalize: bool,
}

// An immutable copy of a WaveShaperNode's curve.
// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
pub struct WaveShaperCurve {
    pub values: Vec<f32>,
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_buffer_contents_create(
    channels: Span<'_, Span<'_, f32>>,
    sample_rate: f32,
) -> *const AudioBufferContents {
    Arc::into_raw(Arc::new(AudioBufferContents {
        channels: channels
            .as_slice()
            .iter()
            .map(|channel| channel.as_slice().to_vec())
            .collect(),
        sample_rate,
    }))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_buffer_contents_destroy(contents: *const AudioBufferContents) {
    drop(unsafe { Arc::from_raw(contents) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_buffer_contents_channel(
    contents: *const AudioBufferContents,
    channel: usize,
    output: *mut Span<'static, f32>,
) {
    unsafe {
        *output = Span::new(&(&*contents).channels[channel]);
    }
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_periodic_wave_data_create(
    real: Span<'_, f32>,
    imag: Span<'_, f32>,
    normalize: bool,
) -> *const PeriodicWaveData {
    Arc::into_raw(Arc::new(PeriodicWaveData {
        real: real.as_slice().to_vec(),
        imag: imag.as_slice().to_vec(),
        normalize,
    }))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_periodic_wave_data_destroy(data: *const PeriodicWaveData) {
    drop(unsafe { Arc::from_raw(data) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_periodic_wave_data_real(
    data: *const PeriodicWaveData,
    output: *mut Span<'static, f32>,
) {
    unsafe {
        *output = Span::new(&(&*data).real);
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_periodic_wave_data_imag(
    data: *const PeriodicWaveData,
    output: *mut Span<'static, f32>,
) {
    unsafe {
        *output = Span::new(&(&*data).imag);
    }
}

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_wave_shaper_curve_create(values: Span<'_, f32>) -> *const WaveShaperCurve {
    Arc::into_raw(Arc::new(WaveShaperCurve {
        values: values.as_slice().to_vec(),
    }))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_wave_shaper_curve_destroy(curve: *const WaveShaperCurve) {
    drop(unsafe { Arc::from_raw(curve) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_wave_shaper_curve_values(
    curve: *const WaveShaperCurve,
    output: *mut Span<'static, f32>,
) {
    unsafe {
        *output = Span::new(&(&*curve).values);
    }
}
