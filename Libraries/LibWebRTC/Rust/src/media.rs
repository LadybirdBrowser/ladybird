/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Audio capture/playback wiring for the WebRTC stack.
//!
//! Layout:
//! - sources push 20ms Opus-encoded frames into `TrackLocalStaticSample`
//! - sinks read RTP from `TrackRemote`, decode Opus, play through cpal
//!
//! Stereo Opus @ 48kHz to match `MediaEngine::register_default_codecs`.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{Context, Result};
use audiopus::coder::{Decoder, Encoder};
use audiopus::{Application, Channels, SampleRate};
use bytes::Bytes;
use cpal::StreamConfig;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use tokio::sync::mpsc;
use webrtc::media::Sample;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_remote::TrackRemote;

const SAMPLE_RATE_HZ: u32 = 48000;
const CHANNELS: u16 = 2;
const FRAME_SAMPLES_PER_CHANNEL: usize = 960; // 20ms @ 48kHz
const FRAME_INTERLEAVED_SAMPLES: usize = FRAME_SAMPLES_PER_CHANNEL * CHANNELS as usize;
const FRAME_DURATION: Duration = Duration::from_millis(20);

/// Read RTP from a remote track, decode Opus, and play through the default
/// output device.
///
/// CPAL `Stream` is `!Send`, so the device handle is owned by a dedicated
/// thread that builds the stream and parks; an async task feeds it via a
/// mutex-protected ring buffer.
pub fn play_track(track: Arc<TrackRemote>) -> Result<()> {
    let buffer = Arc::new(Mutex::new(VecDeque::<i16>::new()));

    {
        let buffer = buffer.clone();
        std::thread::spawn(move || {
            if let Err(e) = run_speaker(buffer) {
                log::error!("speaker thread: {e:?}");
            }
        });
    }

    tokio::spawn(async move {
        let mut decoder = match Decoder::new(SampleRate::Hz48000, Channels::Mono) {
            Ok(d) => d,
            Err(e) => {
                log::error!("opus decoder: {e}");
                return;
            }
        };
        // Opus max frame is 120ms; allocate generously.
        let mut pcm = vec![0i16; FRAME_INTERLEAVED_SAMPLES * 6];
        let mut packets_logged = 0usize;
        while let Ok((packet, _)) = track.read_rtp().await {
            if packet.payload.is_empty() {
                continue;
            }
            if packets_logged < 30 {
                let p = &packet.payload;
                log::info!(
                    "rx pt={} mark={} pad={} ext={} ssrc={} len={} first16={:02x?}",
                    packet.header.payload_type,
                    packet.header.marker,
                    packet.header.padding,
                    packet.header.extension,
                    packet.header.ssrc,
                    p.len(),
                    &p[..p.len().min(16)]
                );
                packets_logged += 1;
            }
            match decoder.decode(Some(&packet.payload[..]), &mut pcm[..], false) {
                Ok(samples_per_channel) => {
                    // The decoder is mono; the speaker is stereo.
                    let mut buf = buffer.lock().unwrap();
                    for &s in &pcm[..samples_per_channel] {
                        buf.push_back(s);
                        buf.push_back(s);
                    }
                    // Bound jitter buffer at ~500ms to keep latency in check.
                    let cap = SAMPLE_RATE_HZ as usize * CHANNELS as usize / 2;
                    while buf.len() > cap {
                        buf.pop_front();
                    }
                }
                Err(e) => log::warn!("opus decode: {e}"),
            }
        }
    });

    Ok(())
}

fn run_speaker(buffer: Arc<Mutex<VecDeque<i16>>>) -> Result<()> {
    let host = cpal::default_host();
    let device = host.default_output_device().context("no default output device")?;
    let config = StreamConfig {
        channels: CHANNELS,
        sample_rate: cpal::SampleRate(SAMPLE_RATE_HZ),
        buffer_size: cpal::BufferSize::Default,
    };
    let stream = device.build_output_stream::<f32, _, _>(
        &config,
        move |out: &mut [f32], _| {
            let mut buf = buffer.lock().unwrap();
            for slot in out.iter_mut() {
                *slot = buf.pop_front().map_or(0.0, |s| f32::from(s) / f32::from(i16::MAX));
            }
        },
        |err| log::warn!("speaker: {err}"),
        None,
    )?;
    stream.play()?;
    std::thread::park();
    drop(stream);
    Ok(())
}

/// Push a stereo 440 Hz sine wave into a local track as Opus samples.
/// Synthetic source for end-to-end smoke tests; avoids mic/speaker feedback
/// when both peers run in the same process.
pub fn push_sine_to_track(track: Arc<TrackLocalStaticSample>, freq_hz: f64) -> Result<()> {
    tokio::spawn(async move {
        let encoder = match Encoder::new(SampleRate::Hz48000, Channels::Stereo, Application::Audio) {
            Ok(e) => e,
            Err(e) => {
                log::error!("opus encoder: {e}");
                return;
            }
        };
        let mut pcm = [0i16; FRAME_INTERLEAVED_SAMPLES];
        let mut out = [0u8; 4000];
        let mut phase: f64 = 0.0;
        let phase_step = 2.0 * std::f64::consts::PI * freq_hz / f64::from(SAMPLE_RATE_HZ);
        let mut interval = tokio::time::interval(FRAME_DURATION);
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            interval.tick().await;
            for chunk in pcm.as_chunks_mut::<{ CHANNELS as usize }>().0 {
                let v = (phase.sin() * f64::from(i16::MAX) * 0.3) as i16;
                chunk[0] = v;
                chunk[1] = v;
                phase += phase_step;
                if phase > 2.0 * std::f64::consts::PI {
                    phase -= 2.0 * std::f64::consts::PI;
                }
            }
            let n = match encoder.encode(&pcm[..], &mut out[..]) {
                Ok(n) => n,
                Err(e) => {
                    log::warn!("opus encode: {e}");
                    continue;
                }
            };
            let sample = Sample {
                data: Bytes::copy_from_slice(&out[..n]),
                duration: FRAME_DURATION,
                ..Default::default()
            };
            if let Err(e) = track.write_sample(&sample).await {
                log::warn!("write_sample: {e}");
                break;
            }
        }
    });
    Ok(())
}

/// Capture from the default input device, encode with Opus, write to a local
/// track. PCM accumulator is on the audio thread; encoding runs on a tokio
/// task fed via an unbounded channel.
pub fn start_mic_to_track(track: Arc<TrackLocalStaticSample>) -> Result<()> {
    let (pcm_tx, mut pcm_rx) = mpsc::unbounded_channel::<Vec<i16>>();

    std::thread::spawn(move || {
        if let Err(e) = run_mic(pcm_tx) {
            log::error!("mic thread: {e:?}");
        }
    });

    tokio::spawn(async move {
        let encoder = match Encoder::new(SampleRate::Hz48000, Channels::Stereo, Application::Voip) {
            Ok(e) => e,
            Err(e) => {
                log::error!("opus encoder: {e}");
                return;
            }
        };
        let mut out = [0u8; 4000];
        while let Some(frame) = pcm_rx.recv().await {
            match encoder.encode(&frame[..], &mut out[..]) {
                Ok(n) => {
                    let sample = Sample {
                        data: Bytes::copy_from_slice(&out[..n]),
                        duration: FRAME_DURATION,
                        ..Default::default()
                    };
                    if let Err(e) = track.write_sample(&sample).await {
                        log::warn!("write_sample: {e}");
                        break;
                    }
                }
                Err(e) => log::warn!("opus encode: {e}"),
            }
        }
    });

    Ok(())
}

fn run_mic(pcm_tx: mpsc::UnboundedSender<Vec<i16>>) -> Result<()> {
    let host = cpal::default_host();
    let device = host.default_input_device().context("no default input device")?;
    let config = StreamConfig {
        channels: CHANNELS,
        sample_rate: cpal::SampleRate(SAMPLE_RATE_HZ),
        buffer_size: cpal::BufferSize::Default,
    };
    let mut accumulator: Vec<i16> = Vec::with_capacity(FRAME_INTERLEAVED_SAMPLES * 2);
    let stream = device.build_input_stream::<f32, _, _>(
        &config,
        move |data: &[f32], _| {
            accumulator.extend(data.iter().map(|&s| (s.clamp(-1.0, 1.0) * f32::from(i16::MAX)) as i16));
            while accumulator.len() >= FRAME_INTERLEAVED_SAMPLES {
                let frame: Vec<i16> = accumulator.drain(..FRAME_INTERLEAVED_SAMPLES).collect();
                if pcm_tx.send(frame).is_err() {
                    break;
                }
            }
        },
        |err| log::warn!("mic: {err}"),
        None,
    )?;
    stream.play()?;
    std::thread::park();
    drop(stream);
    Ok(())
}
