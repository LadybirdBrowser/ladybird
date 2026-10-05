/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;
use crate::render_state::BegunRead;

crate::stage::main_thread_ffi_entries!();

/// Runs the document's layout update to a fixed point: style, then the layout tree build, then
/// either a partial relayout of the registered boundaries or a full pass, until nothing is
/// pending. The document-side steps run through the registered layout update host.
///
/// # Safety
///
/// `host` must be a live document host with registered layout and layout update hosts, on its document's thread
/// between `document_host_begin_update_layout` and its end, and `inputs` must remain valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_update_layout(
    host: &DocumentHost,
    read: &BegunRead,
    inputs: *const FfiLayoutUpdateInputs,
) {
    assert!(!inputs.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    abort_on_panic(|| {
        // SAFETY: Guaranteed by the entry point's contract.
        unsafe { update_layout(&main_thread, host, read, &*inputs) };
        // The image resources the update's tree builds owe are attached once its layout is done.
        host.host_tables()
            .layout_update_host
            .get()
            .expect("the document has no layout update host")
            .attach_owed_image_resources(&main_thread, host, read);
    });
}

/// Pays what the layout round a frame ran owes the host, where the frame flew with one, landing it first with `read`:
/// the host's teardown of the layout tree pays it before the tree goes.
///
/// # Safety
///
/// `host` must be a live document host with registered layout and layout update hosts, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_pay_flown_round(host: &DocumentHost, read: &BegunRead) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    abort_on_panic(|| {
        host.take_frame_in_with(read);
        host.pay_clock_rounds(|mut answer| {
            let callbacks = host.host_tables().layout_update_host.get();
            // SAFETY: Guaranteed by the entry point's contract.
            unsafe {
                answer.pay(
                    &main_thread,
                    &callbacks.expect("the document has no layout update host"),
                    read,
                );
            }
        });
        if let Some(round) = host.take_flown_round() {
            // SAFETY: Guaranteed by the entry point's contract.
            unsafe { round.pay(&main_thread, host, read) };
        }
    });
}

/// Seals the plan of the clock lease of `host`'s document for the tasks after a rendering update, in `read`: the
/// elements whose running animations a tick samples, the monotonic time in milliseconds at which the document's
/// timestamps are zero, the timestamp of the next event of the animations, past which a tick samples nothing, and the
/// timestamp at which the sampled animations have all ended. A document whose layout is not up to date gets no plan.
///
/// # Safety
///
/// As for [`render_state_update_layout`], with no layout update running, and `elements` must hold `count` style nodes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_seal_clock_plan(
    host: &DocumentHost,
    read: &BegunRead,
    elements: *const u32,
    count: usize,
    time_origin: f64,
    deadline: f64,
    last_end: f64,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the caller.
    let elements = unsafe { crate::css::custom_properties::ffi_slice(elements, count) };
    abort_on_panic(|| {
        let elements: Vec<_> = elements
            .iter()
            .filter_map(|&element| StyleNodeID::from_raw(element))
            .collect();
        let round = seal_clock_round(&main_thread, host, read);
        host.seal_clock_plan(
            round.map(|round| crate::render_state::ClockPlan::new(elements, time_origin, deadline, last_end, round)),
        );
    });
}

/// Seals the first round of the layout of a rendering update whose style is about to fly, as of the document's facts
/// now, where the document needs one: the frame runs it after its style.
///
/// # Safety
///
/// As for [`render_state_update_layout`], with no layout update running.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_seal_first_layout_round(
    host: &DocumentHost,
    read: &BegunRead,
    inputs: *const FfiLayoutUpdateInputs,
) {
    assert!(!inputs.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    // SAFETY: Guaranteed by the entry point's contract.
    abort_on_panic(|| unsafe { seal_first_round(&main_thread, host, read, &*inputs) });
}

/// Lets the first round of the layout of a rendering update whose style is up to date fly beside the host, in `read`,
/// where the document's layout is not up to date and `blocker` is none: the host's next layout update pays it first.
/// Answers whether the round flies.
///
/// # Safety
///
/// As for [`render_state_update_layout`], with no layout update running and no frame of the document in flight.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_let_first_layout_round_fly(
    host: *const DocumentHost,
    read: &BegunRead,
    inputs: *const FfiLayoutUpdateInputs,
    blocker: crate::painting::ffi::FfiFlightBlocker,
) -> bool {
    assert!(!host.is_null(), "document host is null");
    assert!(!inputs.is_null());
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { main_thread(host) };
    let Some(license) = FlightLicense::for_blocker(blocker) else {
        return false;
    };
    // SAFETY: Guaranteed by the entry point's contract.
    abort_on_panic(|| unsafe { fly_first_round(&main_thread, host, read, &*inputs, &license) })
}
