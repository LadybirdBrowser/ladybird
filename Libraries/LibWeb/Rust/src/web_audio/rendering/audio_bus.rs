/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://webaudio.github.io/web-audio-api/#ChannelLayouts
// Speakers channel layouts exist for mono, stereo, quad and 5.1; all other channel counts use discrete mixing.
const LEFT: usize = 0;
const RIGHT: usize = 1;
const CENTER: usize = 2;
const SURROUND_LEFT: usize = 4;
const SURROUND_RIGHT: usize = 5;

// A block of audio samples for a single render quantum, holding one buffer of frames per channel.
pub struct AudioBus {
    channels: Vec<Vec<f32>>,
    frame_count: usize,
}

impl AudioBus {
    pub fn new(channel_count: usize, frame_count: usize) -> Self {
        let mut bus = Self {
            channels: Vec::new(),
            frame_count,
        };
        bus.set_channel_count(channel_count);
        bus
    }

    pub fn channel_count(&self) -> usize {
        self.channels.len()
    }

    pub fn frame_count(&self) -> usize {
        self.frame_count
    }

    pub fn channel(&self, index: usize) -> &[f32] {
        &self.channels[index]
    }

    pub fn channel_mut(&mut self, index: usize) -> &mut [f32] {
        &mut self.channels[index]
    }

    pub(crate) unsafe fn channel_pointer(bus: *mut Self, index: usize) -> (*mut f32, usize) {
        let channels = unsafe { &raw mut (*bus).channels };
        assert!(index < unsafe { (*channels).len() });
        let channel = unsafe { (*channels).as_mut_ptr().add(index) };
        unsafe { ((*channel).as_mut_ptr(), (*channel).len()) }
    }

    pub fn zero(&mut self) {
        for channel in &mut self.channels {
            channel.fill(0.);
        }
    }

    pub fn is_silent(&self) -> bool {
        !self
            .channels
            .iter()
            .any(|channel| channel.iter().any(|&sample| sample != 0.))
    }

    pub fn set_channel_count(&mut self, channel_count: usize) {
        if self.channels.len() == channel_count {
            return;
        }
        self.channels.resize_with(channel_count, Vec::new);
        for channel in &mut self.channels {
            channel.resize(self.frame_count, 0.);
        }
    }

    pub fn copy_from(&mut self, source: &Self) {
        assert_eq!(source.frame_count(), self.frame_count());
        self.set_channel_count(source.channel_count());
        for channel_index in 0..self.channels.len() {
            self.channels[channel_index].clone_from(&source.channels[channel_index]);
        }
    }

    // https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
    pub fn sum_from(&mut self, source: &Self, speakers: bool) {
        assert_eq!(source.frame_count(), self.frame_count());
        if speakers {
            self.sum_from_speakers(source);
        } else {
            self.sum_from_discrete(source);
        }
    }

    // https://webaudio.github.io/web-audio-api/#UpMix-sub and https://webaudio.github.io/web-audio-api/#down-mix
    fn sum_from_speakers(&mut self, source: &Self) {
        let source_channels = source.channel_count();
        let destination_channels = self.channel_count();

        let mut sum_channel = |destination_index: usize, source_index: usize, gain: f32| {
            let destination_span = &mut self.channels[destination_index];
            let source_span = source.channel(source_index);
            for frame in 0..self.frame_count {
                destination_span[frame] += source_span[frame] * gain;
            }
        };

        if source_channels == destination_channels {
            for channel_index in 0..source_channels {
                sum_channel(channel_index, channel_index, 1.);
            }
            return;
        }

        // NB: Up-mixing: mono/stereo/quad sources into stereo, quad or 5.1 destinations.
        if source_channels == 1 && destination_channels == 2 {
            // 1 -> 2: output.L = input, output.R = input
            sum_channel(LEFT, 0, 1.);
            sum_channel(RIGHT, 0, 1.);
            return;
        }
        if source_channels == 1 && destination_channels == 4 {
            // 1 -> 4: output.L = input, output.R = input, output.SL = 0, output.SR = 0
            sum_channel(LEFT, 0, 1.);
            sum_channel(RIGHT, 0, 1.);
            return;
        }
        if source_channels == 1 && destination_channels == 6 {
            // 1 -> 5.1: output.C = input, all other channels silent
            sum_channel(CENTER, 0, 1.);
            return;
        }
        if source_channels == 2 && destination_channels == 4 {
            // 2 -> 4: output.L = input.L, output.R = input.R, surround channels silent
            sum_channel(LEFT, LEFT, 1.);
            sum_channel(RIGHT, RIGHT, 1.);
            return;
        }
        if source_channels == 2 && destination_channels == 6 {
            // 2 -> 5.1: output.L = input.L, output.R = input.R, all other channels silent
            sum_channel(LEFT, LEFT, 1.);
            sum_channel(RIGHT, RIGHT, 1.);
            return;
        }
        if source_channels == 4 && destination_channels == 6 {
            // 4 -> 5.1: output.L = input.L, output.R = input.R, output.SL = input.SL, output.SR = input.SR
            sum_channel(LEFT, LEFT, 1.);
            sum_channel(RIGHT, RIGHT, 1.);
            sum_channel(SURROUND_LEFT, 2, 1.);
            sum_channel(SURROUND_RIGHT, 3, 1.);
            return;
        }

        // NB: Down-mixing: stereo/quad/5.1 sources into mono, stereo or quad destinations.
        if source_channels == 2 && destination_channels == 1 {
            // 2 -> 1: output = 0.5 * (input.L + input.R)
            sum_channel(0, LEFT, 0.5);
            sum_channel(0, RIGHT, 0.5);
            return;
        }
        if source_channels == 4 && destination_channels == 1 {
            // 4 -> 1: output = 0.25 * (input.L + input.R + input.SL + input.SR)
            for channel_index in 0..4 {
                sum_channel(0, channel_index, 0.25);
            }
            return;
        }
        if source_channels == 6 && destination_channels == 1 {
            // 5.1 -> 1: output = sqrt(0.5) * (input.L + input.R) + input.C + 0.5 * (input.SL + input.SR)
            sum_channel(0, LEFT, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(0, RIGHT, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(0, CENTER, 1.);
            sum_channel(0, SURROUND_LEFT, 0.5);
            sum_channel(0, SURROUND_RIGHT, 0.5);
            return;
        }
        if source_channels == 4 && destination_channels == 2 {
            // 4 -> 2: output.L = 0.5 * (input.L + input.SL), output.R = 0.5 * (input.R + input.SR)
            sum_channel(LEFT, LEFT, 0.5);
            sum_channel(LEFT, 2, 0.5);
            sum_channel(RIGHT, RIGHT, 0.5);
            sum_channel(RIGHT, 3, 0.5);
            return;
        }
        if source_channels == 6 && destination_channels == 2 {
            // 5.1 -> 2: output.L = input.L + sqrt(0.5) * (input.C + input.SL),
            //           output.R = input.R + sqrt(0.5) * (input.C + input.SR)
            sum_channel(LEFT, LEFT, 1.);
            sum_channel(LEFT, CENTER, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(LEFT, SURROUND_LEFT, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(RIGHT, RIGHT, 1.);
            sum_channel(RIGHT, CENTER, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(RIGHT, SURROUND_RIGHT, std::f32::consts::FRAC_1_SQRT_2);
            return;
        }
        if source_channels == 6 && destination_channels == 4 {
            // 5.1 -> 4: output.L = input.L + sqrt(0.5) * input.C, output.R = input.R + sqrt(0.5) * input.C,
            //           output.SL = input.SL, output.SR = input.SR
            sum_channel(LEFT, LEFT, 1.);
            sum_channel(LEFT, CENTER, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(RIGHT, RIGHT, 1.);
            sum_channel(RIGHT, CENTER, std::f32::consts::FRAC_1_SQRT_2);
            sum_channel(2, SURROUND_LEFT, 1.);
            sum_channel(3, SURROUND_RIGHT, 1.);
            return;
        }

        // NB: All other layout combinations fall back to discrete mixing.
        self.sum_from_discrete(source);
    }

    // https://webaudio.github.io/web-audio-api/#dom-channelinterpretation-discrete
    fn sum_from_discrete(&mut self, source: &Self) {
        // Up-mix by filling channels until they run out then zero out remaining channels. Down-mix by filling as many
        // channels as possible, then dropping remaining channels.
        let channels_to_sum = source.channel_count().min(self.channel_count());
        for channel_index in 0..channels_to_sum {
            let destination_span = &mut self.channels[channel_index];
            let source_span = source.channel(channel_index);
            for frame in 0..self.frame_count {
                destination_span[frame] += source_span[frame];
            }
        }
    }
}
