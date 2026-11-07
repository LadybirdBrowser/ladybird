/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(
    clippy::unnecessary_unwrap,
    reason = "preserve the C++ constructor's explicit option cases"
)]

use std::cell::RefCell;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{ErrorKind, ErrorType, Trace, Vm};
use libweb_rust::web_audio::rendering::audio_data::PeriodicWaveData;

use super::base_audio_context::AnyBaseAudioContext;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) struct PeriodicWaveConstraints {
    pub(crate) disable_normalization: bool,
}

pub(crate) struct PeriodicWaveOptions {
    pub(crate) parent: PeriodicWaveConstraints,
    pub(crate) real: Option<Vec<f32>>,
    pub(crate) imag: Option<Vec<f32>>,
}

struct State {
    real: Vec<f32>,
    imag: Vec<f32>,
    normalize: bool,
}

// https://webaudio.github.io/web-audio-api/#PeriodicWave
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PeriodicWave {
    #[gc(untraced)]
    state: RefCell<State>,
}

impl Wrappable for PeriodicWave {
    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        let state = self.state.borrow();
        (state.real.capacity() * size_of::<f32>()).saturating_add(state.imag.capacity() * size_of::<f32>())
    }
}

fn copy_float_vector(vm: &Vm, source: &[f32]) -> WebIdlResult<Vec<f32>> {
    let mut vector = Vec::new();
    vector.try_reserve_exact(source.len()).map_err(|_| {
        Exception::from(
            vm.throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .unwrap_err(),
        )
    })?;
    vector.extend_from_slice(source);
    Ok(vector)
}

fn zeroed_float_vector(vm: &Vm, size: usize) -> WebIdlResult<Vec<f32>> {
    let mut vector = Vec::new();
    vector.try_reserve_exact(size).map_err(|_| {
        Exception::from(
            vm.throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .unwrap_err(),
        )
    })?;
    vector.resize(size, 0.);
    Ok(vector)
}

impl PeriodicWave {
    pub(crate) fn construct(
        cx: &mut CallContext,
        _context: AnyBaseAudioContext,
        options: PeriodicWaveOptions,
    ) -> WebIdlResult<Ref<Self>> {
        Self::create(cx, options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-periodicwave-periodicwave
    pub(crate) fn create(cx: &mut CallContext, options: PeriodicWaveOptions) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();

        // 1. Let p be a new PeriodicWave object. Let [[real]] and [[imag]] be two internal slots of type Float32Array, and let [[normalize]] be an internal slot.
        let p = allocate(vm, |_| Self {
            state: RefCell::new(State {
                real: Vec::new(),
                imag: Vec::new(),
                normalize: true,
            }),
        });
        let mut state = p.state.borrow_mut();

        // 2. Process options according to one of the following cases:
        {
            // 1. If both options.real and options.imag are present
            if options.real.is_some() && options.imag.is_some() {
                // 1. If the lengths of options.real and options.imag are different or if either length is less than 2, throw an IndexSizeError and abort this algorithm.
                if options.real.as_ref().unwrap().len() != options.imag.as_ref().unwrap().len()
                    || options.real.as_ref().unwrap().len() < 2
                {
                    return Err(Exception::index_size_error(Utf16String::from_ascii(
                        b"Real and imaginary arrays must have the same length and contain at least 2 elements",
                    )));
                }

                // 2. Set [[real]] and [[imag]] to new arrays with the same length as options.real.
                // 3. Copy all elements from options.real to [[real]] and options.imag to [[imag]].
                state.real = copy_float_vector(vm, options.real.as_ref().unwrap())?;
                state.imag = copy_float_vector(vm, options.imag.as_ref().unwrap())?;
            }
            // 2. If only options.real is present
            else if let Some(real) = &options.real {
                // 1. If length of options.real is less than 2, throw an IndexSizeError and abort this algorithm
                if real.len() < 2 {
                    return Err(Exception::index_size_error(Utf16String::from_ascii(
                        b"Real array must contain at least 2 elements",
                    )));
                }

                // 2. Set [[real]] and [[imag]] to arrays with the same length as options.real
                // 3. Copy options.real to [[real]] and set [[imag]] to all zeros.
                state.real = copy_float_vector(vm, real)?;
                state.imag = zeroed_float_vector(vm, real.len())?;
            }
            // 3. If only options.imag is present
            else if let Some(imag) = &options.imag {
                // 1. If length of options.imag is less than 2, throw an IndexSizeError and abort this algorithm.
                if imag.len() < 2 {
                    return Err(Exception::index_size_error(Utf16String::from_ascii(
                        b"Imaginary array must contain at least 2 elements",
                    )));
                }

                // 2. Set [[real]] and [[imag]] to arrays with the same length as options.imag.
                // 3. Copy options.imag to [[imag]] and set [[real]] to all zeros.
                state.real = zeroed_float_vector(vm, imag.len())?;
                state.imag = copy_float_vector(vm, imag)?;
            }
            // 4. Otherwise
            else {
                // 1. Set [[real]] and [[imag]] to zero-filled arrays of length 2.
                state.real = zeroed_float_vector(vm, 2)?;
                state.imag = zeroed_float_vector(vm, 2)?;

                // 2. Set element at index 1 of [[imag]] to 1.
                state.imag[1] = 1.;
            }
        }

        // 3. Set element at index 0 of both [[real]] and [[imag]] to 0. (This sets the DC component to 0.)
        state.real[0] = 0.;
        state.imag[0] = 0.;

        // 4. Initialize [[normalize]] to the inverse of the disableNormalization attribute of the PeriodicWaveConstraints on the PeriodicWaveOptions.
        state.normalize = !options.parent.disable_normalization;
        drop(state);

        // 5. Return p.
        Ok(p)
    }

    pub(crate) fn render_data(&self) -> Arc<PeriodicWaveData> {
        let state = self.state.borrow();
        Arc::new(PeriodicWaveData {
            real: state.real.clone(),
            imag: state.imag.clone(),
            normalize: state.normalize,
        })
    }
}
