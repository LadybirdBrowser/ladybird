/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop)]

use std::cell::UnsafeCell;
use std::sync::Arc;

use ak::Span;

use super::fft::{FFT, FFTDirection};

pub struct AudioBufferContents<'a> {
    pub channels: &'a [Span<'a, f32>],
    pub sample_rate: f32,
}

impl AudioBufferContents<'_> {
    pub fn length(&self) -> usize {
        self.channels.first().map_or(0, |channel| channel.as_slice().len())
    }
}

struct Channel {
    head: Vec<f32>,
    real: Vec<f32>,
    imag: Vec<f32>,
}

// An impulse response prepared for convolution on the rendering thread, built on the control thread when a
// ConvolverNode's buffer is set so the rendering thread never has to transform a whole impulse response in a single
// quantum.
//
// The first partition is kept in the time domain: it is the one that carries the quietest part of the output, where the
// round-off of a transform round trip is most audible. Everything after it is cut into equally sized partitions and
// transformed. Only the first half of each partition's spectrum plus the Nyquist bin is stored; the remaining bins
// follow from the Hermitian symmetry of the transform of a real signal.
// https://webaudio.github.io/web-audio-api/#ConvolverNode
pub struct ConvolverKernel {
    channels: Vec<Channel>,
    partition_count: usize,
    bin_count: usize,
}

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-normalize
fn calculate_normalization_scale(contents: &AudioBufferContents<'_>) -> f32 {
    const GAIN_CALIBRATION: f64 = 0.00125;
    const GAIN_CALIBRATION_SAMPLE_RATE: f64 = 44100.;
    const MIN_POWER: f64 = 0.000125;

    // Normalize by RMS power.
    let mut power = 0.;
    for samples in contents.channels {
        for &sample in samples.as_slice() {
            power += f64::from(sample) * f64::from(sample);
        }
    }
    power = (power / (contents.channels.len() * contents.length()) as f64).sqrt();

    // Protect against accidental overload.
    if !power.is_finite() || power < MIN_POWER {
        power = MIN_POWER;
    }

    let mut scale = 1. / power;

    // Calibrate to make perceived volume same as unprocessed.
    scale *= GAIN_CALIBRATION;

    // Scale depends on sample-rate.
    if contents.sample_rate != 0. {
        scale *= GAIN_CALIBRATION_SAMPLE_RATE / f64::from(contents.sample_rate);
    }

    // True-stereo compensation.
    if contents.channels.len() == 4 {
        scale *= 0.5;
    }

    scale as f32
}

impl ConvolverKernel {
    pub fn create(contents: &AudioBufferContents<'_>, partition_size: usize, normalize: bool) -> Option<Arc<Self>> {
        let length = contents.length();
        if length == 0 {
            return None;
        }

        let scale = if normalize {
            calculate_normalization_scale(contents)
        } else {
            1.
        };

        // Each partition is zero-padded to twice its length so that the cyclic convolution of the transform produces the
        // linear convolution the rendering thread is after.
        let transform_size = 2 * partition_size;
        let bin_count = partition_size + 1;
        let partition_count = if length > partition_size {
            (length - partition_size).div_ceil(partition_size)
        } else {
            0
        };

        let fft = FFT::new(transform_size);
        let mut real = vec![0.; transform_size];
        let mut imag = vec![0.; transform_size];

        let mut channels = Vec::with_capacity(contents.channels.len());
        for samples in contents.channels {
            let samples = samples.as_slice();
            let mut channel = Channel {
                head: vec![0.; partition_size],
                real: vec![0.; partition_count * bin_count],
                imag: vec![0.; partition_count * bin_count],
            };

            for frame in 0..partition_size.min(length) {
                channel.head[frame] = samples[frame] * scale;
            }

            for partition in 0..partition_count {
                let offset = (partition + 1) * partition_size;
                let frames_in_partition = partition_size.min(length - offset);
                for frame in 0..frames_in_partition {
                    real[frame] = samples[offset + frame] * scale;
                }
                for frame in frames_in_partition..transform_size {
                    real[frame] = 0.;
                }
                for frame in 0..transform_size {
                    imag[frame] = 0.;
                }

                fft.transform(&mut real, &mut imag, FFTDirection::Forward);

                for bin in 0..bin_count {
                    channel.real[partition * bin_count + bin] = real[bin];
                    channel.imag[partition * bin_count + bin] = imag[bin];
                }
            }

            channels.push(channel);
        }

        Some(Arc::new(Self {
            channels,
            partition_count,
            bin_count,
        }))
    }

    pub fn channel_count(&self) -> usize {
        self.channels.len()
    }

    // The number of partitions that are held in the frequency domain, which excludes the time-domain head.
    pub fn partition_count(&self) -> usize {
        self.partition_count
    }

    pub fn bin_count(&self) -> usize {
        self.bin_count
    }

    pub fn head(&self, channel: usize) -> &[f32] {
        &self.channels[channel].head
    }

    pub fn partition_real(&self, channel: usize, partition: usize) -> &[f32] {
        &self.channels[channel].real[partition * self.bin_count..(partition + 1) * self.bin_count]
    }

    pub fn partition_imag(&self, channel: usize, partition: usize) -> &[f32] {
        &self.channels[channel].imag[partition * self.bin_count..(partition + 1) * self.bin_count]
    }
}

// The ring of input spectra a ConvolverRenderNode multiplies with the kernel's partitions, one slot per partition plus
// one for the window written in the current quantum. It is sized after the kernel, so it is allocated here on the
// control thread and handed over together with the kernel, keeping allocation off the rendering thread.
pub struct ConvolverDelayLine {
    length: usize,
    real: [UnsafeCell<Vec<f32>>; Self::MAX_CHANNEL_COUNT],
    imag: [UnsafeCell<Vec<f32>>; Self::MAX_CHANNEL_COUNT],
}

// SAFETY: Delay-line samples are only accessed by the rendering thread after transfer.
unsafe impl Send for ConvolverDelayLine {}
unsafe impl Sync for ConvolverDelayLine {}
impl ConvolverDelayLine {
    // The input to a ConvolverNode is mono or stereo and cannot be increased.
    // https://webaudio.github.io/web-audio-api/#ConvolverNode
    pub const MAX_CHANNEL_COUNT: usize = 2;

    pub fn create(kernel: &ConvolverKernel) -> Self {
        let length = kernel.partition_count() + 1;
        let real = std::array::from_fn(|_| UnsafeCell::new(vec![0.; length * kernel.bin_count()]));
        let imag = std::array::from_fn(|_| UnsafeCell::new(vec![0.; length * kernel.bin_count()]));
        Self { length, real, imag }
    }

    #[allow(clippy::mut_from_ref)]
    pub(crate) unsafe fn real_shared(&self, channel: usize) -> &mut [f32] {
        unsafe { &mut *self.real[channel].get() }
    }

    #[allow(clippy::mut_from_ref)]
    pub(crate) unsafe fn imag_shared(&self, channel: usize) -> &mut [f32] {
        unsafe { &mut *self.imag[channel].get() }
    }

    pub fn length(&self) -> usize {
        self.length
    }

    pub fn real(&mut self, channel: usize) -> &mut [f32] {
        self.real[channel].get_mut()
    }

    pub fn imag(&mut self, channel: usize) -> &mut [f32] {
        self.imag[channel].get_mut()
    }
}
