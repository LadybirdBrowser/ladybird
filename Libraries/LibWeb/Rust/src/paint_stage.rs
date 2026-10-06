/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where frames are presented: on the Paint thread, and only there.
//!
//! Presenting a frame takes a [`Presenting`], which only the jobs this module hands the Paint thread are lent, on that
//! thread. The Paint thread runs its jobs one at a time and in the order they were handed to it, so the frames of a
//! compositor context reach the compositor in the order their jobs were handed out, and code that runs anywhere else
//! cannot present a frame: it has no [`Presenting`] to present with.

use crate::stage_thread::{InFlight, Riding, StopWord, paint_thread};
use std::marker::PhantomData;

/// What a job on the Paint thread presents with. Only this module makes one, on the Paint thread, for one job at a time.
/// The raw pointer marker makes it neither [`Send`] nor [`Sync`], so it cannot leave the job it is lent to.
pub(crate) struct Presenting {
    not_send_or_sync: PhantomData<*const ()>,
}

impl Presenting {
    /// Lends `job` what it presents with. Only a job running on the Paint thread may call this.
    fn lend<R>(job: impl FnOnce(&mut Presenting) -> R) -> R {
        debug_assert!(paint_thread().is_current(), "only the Paint thread presents");
        job(&mut Presenting {
            not_send_or_sync: PhantomData,
        })
    }
}

/// Hands `job` to the Paint thread, which runs it after the jobs handed to it before, beside whoever holds the ride, and
/// lends it what it presents with.
pub(crate) fn ride_presenting<R: Send + 'static>(job: impl FnOnce(&mut Presenting) -> R + Send + 'static) -> Riding<R> {
    paint_thread().ride(move || Presenting::lend(job))
}

/// Submits `job` to the Paint thread, which runs it after the jobs handed to it before, beside the host, and lends it
/// what it presents with. The host takes what it answers in from the flight.
pub(crate) fn submit_presenting<R: Send + 'static>(
    job: impl FnOnce(&mut Presenting, &StopWord) -> R + Send + 'static,
) -> InFlight<R> {
    paint_thread().submit(move |stop| Presenting::lend(|presenting| job(presenting, stop)))
}
