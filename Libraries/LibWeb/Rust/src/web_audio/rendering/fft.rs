/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::math;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FFTDirection {
    Forward,
    Inverse,
}

pub struct FFT {
    size: usize,
    twiddle_real: Vec<f32>,
    twiddle_imag: Vec<f32>,
}

impl FFT {
    // Radix-2 Cooley-Tukey FFT of a fixed power-of-two size. The twiddle factors depend only on that size, so they are
    // computed once here and shared by every transform.
    pub fn new(size: usize) -> Self {
        assert!(size != 0 && (size & (size - 1)) == 0);

        let mut twiddle_real = vec![0.; size / 2];
        let mut twiddle_imag = vec![0.; size / 2];
        for index in 0..size / 2 {
            let angle = -2. * std::f64::consts::PI * index as f64 / size as f64;
            twiddle_real[index] = math::cos(angle) as f32;
            twiddle_imag[index] = math::sin(angle) as f32;
        }
        Self {
            size,
            twiddle_real,
            twiddle_imag,
        }
    }

    pub fn size(&self) -> usize {
        self.size
    }

    // Both spans must hold exactly `size()` samples. The inverse transform includes the 1/N scaling.
    pub fn transform(&self, re: &mut [f32], im: &mut [f32], direction: FFTDirection) {
        assert!(re.len() == self.size && im.len() == self.size);

        let mut j = 0;
        for i in 1..self.size {
            let mut bit = self.size >> 1;
            while j & bit != 0 {
                j ^= bit;
                bit >>= 1;
            }
            j ^= bit;
            if i < j {
                re.swap(i, j);
                im.swap(i, j);
            }
        }

        let twiddle_sign = if direction == FFTDirection::Forward { 1. } else { -1. };
        let mut len = 2;
        while len <= self.size {
            let half = len >> 1;
            let twiddle_stride = self.size / len;
            for i in (0..self.size).step_by(len) {
                for k in 0..half {
                    let w_re = self.twiddle_real[k * twiddle_stride];
                    let w_im = self.twiddle_imag[k * twiddle_stride] * twiddle_sign;
                    let u_re = re[i + k];
                    let u_im = im[i + k];
                    let v_re = re[i + k + half] * w_re - im[i + k + half] * w_im;
                    let v_im = re[i + k + half] * w_im + im[i + k + half] * w_re;
                    re[i + k] = u_re + v_re;
                    im[i + k] = u_im + v_im;
                    re[i + k + half] = u_re - v_re;
                    im[i + k + half] = u_im - v_im;
                }
            }
            len <<= 1;
        }

        if direction == FFTDirection::Inverse {
            let inverse_size = 1. / self.size as f32;
            for i in 0..self.size {
                re[i] *= inverse_size;
                im[i] *= inverse_size;
            }
        }
    }
}
