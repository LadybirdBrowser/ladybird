/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which loops keep their interrupt check: only a loop that ends soon or that a loop around it stops may lose it.

#![cfg(any(target_arch = "x86_64", all(target_arch = "aarch64", not(windows))))]

use libwasm_jit::{CompileContext, CompileRequest, Options, RuntimeInfo};
use std::sync::Arc;

const LOOP: [u8; 2] = [0x03, 0x40];
const END: u8 = 0x0b;
const BR_IF_0: [u8; 2] = [0x0d, 0x00];
const I32_ADD: u8 = 0x6a;
const I32_SUB: u8 = 0x6b;
const I32_NE: u8 = 0x47;
const I32_LT_S: u8 = 0x48;
const I32_LE_S: u8 = 0x4c;
const I32_GT_U: u8 = 0x4b;
const CALL_0: [u8; 2] = [0x10, 0x00];
const DROP: u8 = 0x1a;

/// The bound parameter, and the two locals the loops count with.
const BOUND: u8 = 0;
const I: u8 = 1;
const J: u8 = 2;

fn constant(mut value: i32) -> Vec<u8> {
    let mut bytes = vec![0x41];
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if (value == 0 && byte & 0x40 == 0) || (value == -1 && byte & 0x40 != 0) {
            bytes.push(byte);
            return bytes;
        }
        bytes.push(byte | 0x80);
    }
}

fn get(local: u8) -> Vec<u8> {
    vec![0x20, local]
}

fn set(local: u8) -> Vec<u8> {
    vec![0x21, local]
}

fn tee(local: u8) -> Vec<u8> {
    vec![0x22, local]
}

/// `local += step`, leaving the new value on the stack.
fn step(local: u8, step: i32) -> Vec<u8> {
    [get(local), constant(step), vec![I32_ADD], tee(local)].concat()
}

/// A loop that steps `local` and goes on while `test` holds of its new value and `bound`.
fn counting_loop(local: u8, amount: i32, bound: Vec<u8>, test: u8, around: &[u8]) -> Vec<u8> {
    [
        LOOP.to_vec(),
        around.to_vec(),
        step(local, amount),
        bound,
        vec![test],
        BR_IF_0.to_vec(),
        vec![END],
    ]
    .concat()
}

/// `inner` as the body of a loop that counts I up to the bound parameter.
fn in_a_loop(inner: Vec<u8>) -> Vec<u8> {
    counting_loop(I, 1, get(BOUND), I32_NE, &inner)
}

/// How many interrupt checks the code of `(func (param i32) (local i32 i32) body)` keeps.
fn check_count(body: &[u8]) -> usize {
    let function = [&[1, 2, 0x7f], body, &[END]].concat();
    let mut bytes = b"\0asm\x01\0\0\0".to_vec();
    bytes.extend([0x01, 0x05, 0x01, 0x60, 0x01, 0x7f, 0x00]);
    bytes.extend([0x03, 0x02, 0x01, 0x00]);
    let size = u8::try_from(function.len()).expect("a small function");
    bytes.extend([0x0a, size + 2, 0x01, size]);
    bytes.extend(function);
    let module = Arc::new(libwasm_engine::compile(&bytes).expect("a valid module"));
    let request = CompileRequest {
        stack_limit: 0,
        module,
        function: 0,
        runtime: RuntimeInfo::default(),
        options: Options {
            verify_ir: true,
            dump_ir: true,
            ..Options::default()
        },
        osr: None,
    };
    let compiled = libwasm_jit::compile(&mut CompileContext::default(), &request).expect("the function compiles");
    let dump = compiled.dump.expect("the dump was asked for");
    dump.lines().filter(|line| line.trim() == "check_interrupt").count()
}

#[test]
fn a_counting_loop_without_a_loop_around_it_keeps_its_check() {
    assert_eq!(check_count(&counting_loop(I, 1, get(BOUND), I32_NE, &[])), 1);
    assert_eq!(check_count(&counting_loop(I, 1, get(BOUND), I32_LT_S, &[])), 1);
    assert_eq!(check_count(&counting_loop(I, 1, constant(6000), I32_NE, &[])), 1);
}

#[test]
fn a_short_loop_loses_its_check_wherever_it_is() {
    assert_eq!(check_count(&counting_loop(I, 1, constant(4000), I32_NE, &[])), 0);
    assert_eq!(check_count(&counting_loop(I, 1, constant(4000), I32_LT_S, &[])), 0);
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, 1, constant(4000), I32_NE, &[]))),
        1
    );
}

#[test]
fn a_test_of_inequality_that_starts_past_its_bound_goes_all_the_way_around() {
    let body = [constant(4000), set(I), counting_loop(I, 1, constant(100), I32_NE, &[])].concat();
    assert_eq!(check_count(&body), 1);
}

#[test]
fn a_counting_loop_in_a_loop_loses_its_check() {
    assert_eq!(check_count(&in_a_loop(counting_loop(J, 1, get(BOUND), I32_NE, &[]))), 1);
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, 1, get(BOUND), I32_LT_S, &[]))),
        1
    );
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, -1, get(BOUND), I32_GT_U, &[]))),
        1
    );
    // The value itself is the test.
    let down = [LOOP.to_vec(), step(J, -1), BR_IF_0.to_vec(), vec![END]].concat();
    assert_eq!(check_count(&in_a_loop(down)), 1);
}

#[test]
fn what_can_go_past_its_bound_keeps_its_check() {
    // A step of two can skip the bound, and wrapping around gets past any `<`.
    assert_eq!(check_count(&in_a_loop(counting_loop(J, 2, get(BOUND), I32_NE, &[]))), 2);
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, 2, get(BOUND), I32_LT_S, &[]))),
        2
    );
    // Everything is at most the largest number.
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, 1, get(BOUND), I32_LE_S, &[]))),
        2
    );
    // The wrong way.
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, -1, get(BOUND), I32_LT_S, &[]))),
        2
    );
}

#[test]
fn a_bound_that_runs_away_keeps_its_check() {
    // BOUND goes up with J, so that `!=` never holds it to anything.
    let around = step(BOUND, 1);
    let body = [around, vec![DROP]].concat();
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, 1, get(BOUND), I32_NE, &body))),
        2
    );
}

#[test]
fn a_loop_that_calls_keeps_its_check() {
    let body = [constant(0), CALL_0.to_vec()].concat();
    assert_eq!(
        check_count(&in_a_loop(counting_loop(J, 1, get(BOUND), I32_NE, &body))),
        2
    );
}

#[test]
fn a_subtraction_counts_too() {
    let inner = [
        LOOP.to_vec(),
        get(J),
        constant(1),
        vec![I32_SUB],
        tee(J),
        get(BOUND),
        vec![I32_NE],
        BR_IF_0.to_vec(),
        vec![END],
    ]
    .concat();
    assert_eq!(check_count(&in_a_loop(inner)), 1);
}

#[test]
fn a_counter_set_to_a_constant_keeps_its_check() {
    // The loop goes on while its counter is nonzero, and the back edge gives the counter a constant, whose value isn't
    // the index of any value in the function.
    let body = [
        LOOP.to_vec(),
        get(I),
        constant(2185),
        set(I),
        BR_IF_0.to_vec(),
        vec![END],
    ]
    .concat();
    assert_eq!(check_count(&body), 1);
}
