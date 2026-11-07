/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::audio_bus::AudioBus;

#[test]
fn bus_channel_resize_and_copy_preserve_samples() {
    let mut bus = AudioBus::new(2, 4);
    bus.channel_mut(0).fill(1.);
    bus.channel_mut(1).fill(2.);
    bus.set_channel_count(4);
    assert_eq!(bus.channel(0), &[1.; 4]);
    assert_eq!(bus.channel(1), &[2.; 4]);
    assert_eq!(bus.channel(2), &[0.; 4]);
    let mut copy = AudioBus::new(0, 4);
    copy.copy_from(&bus);
    bus.zero();
    assert!(bus.is_silent());
    assert!(!copy.is_silent());
    assert_eq!(copy.channel(1), &[2.; 4]);
    copy.set_channel_count(1);
    copy.set_channel_count(2);
    assert_eq!(copy.channel(0), &[1.; 4]);
    assert_eq!(copy.channel(1), &[0.; 4]);
}
