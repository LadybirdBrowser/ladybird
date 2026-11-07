/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// An immutable snapshot of an AudioBuffer's sample data, created on the control thread when the buffer's content is
// acquired and shared with the rendering thread.
// https://webaudio.github.io/web-audio-api/#acquire-the-content
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
