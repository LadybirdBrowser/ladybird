/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The track a peer connection sends the renderer's video through.

use std::any::Any;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use async_trait::async_trait;
use tokio::sync::Mutex as AsyncMutex;
use webrtc::error::{Error, Result};
use webrtc::media::Sample;
use webrtc::rtp_transceiver::rtp_codec::{RTCRtpCodecParameters, RTPCodecType};
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_local::{TrackLocal, TrackLocalContext};

use crate::video::VideoCodec;

/// The codec a sender's video goes out in, and the payload type that stands for it in the session.
pub type SentCodec = (VideoCodec, u8);

struct Binding {
    payload_type: u8,
    /// Not there when the codec is one the renderer can't encode, in which case nothing is sent.
    codec: Option<VideoCodec>,
    track: Arc<TrackLocalStaticSample>,
}

/// A video track whose codec is chosen when bound to the negotiated session.
///
/// webrtc-rs requires a track before negotiation and cannot send if the answer excludes its codec.
/// Defer codec selection until binding, then choose the first negotiated codec the renderer can encode.
pub struct NegotiatedVideoTrack {
    id: String,
    stream_id: String,
    /// The codecs the renderer can encode, which it says once it has something to send. Until then, any will do.
    sendable_codecs: std::sync::Mutex<Option<Vec<VideoCodec>>>,
    binding: AsyncMutex<Option<Binding>>,
    /// Told which codec to encode in whenever the track is bound, or that there is none.
    on_codec: Box<dyn Fn(Option<SentCodec>) + Send + Sync>,
    is_reading_feedback: AtomicBool,
}

impl NegotiatedVideoTrack {
    pub fn new(id: String, stream_id: String, on_codec: Box<dyn Fn(Option<SentCodec>) + Send + Sync>) -> Self {
        Self {
            id,
            stream_id,
            sendable_codecs: std::sync::Mutex::new(None),
            binding: AsyncMutex::new(None),
            on_codec,
            is_reading_feedback: AtomicBool::new(false),
        }
    }

    pub fn set_sendable_codecs(&self, codecs: Vec<VideoCodec>) {
        *self.sendable_codecs.lock().unwrap() = Some(codecs);
    }

    /// Marks feedback reading as started; returns true only on the first call.
    pub fn start_reading_feedback(&self) -> bool {
        !self.is_reading_feedback.swap(true, Ordering::SeqCst)
    }

    /// Chooses the first negotiated codec the renderer can encode, in peer preference order.
    /// Falls back to the first available codec to keep the sender in the session without sending.
    fn choose<'a>(&self, negotiated: &'a [RTCRtpCodecParameters]) -> Option<(&'a RTCRtpCodecParameters, bool)> {
        let sendable = self.sendable_codecs.lock().unwrap().clone();
        let video_codec =
            |parameters: &RTCRtpCodecParameters| VideoCodec::from_mime_type(&parameters.capability.mime_type);
        let is_sendable = |codec: VideoCodec| sendable.as_ref().is_none_or(|sendable| sendable.contains(&codec));
        negotiated
            .iter()
            .find(|parameters| video_codec(parameters).is_some_and(is_sendable))
            .map(|parameters| (parameters, true))
            .or_else(|| {
                negotiated
                    .iter()
                    .find(|parameters| video_codec(parameters).is_some())
                    .map(|parameters| (parameters, false))
            })
    }

    /// Whether the track is bound to a session in a codec other than the one it would take from these.
    pub async fn is_bound_to_another_codec_than(&self, negotiated: &[RTCRtpCodecParameters]) -> bool {
        let binding = self.binding.lock().await;
        let Some(binding) = binding.as_ref() else {
            return false;
        };
        self.choose(negotiated).is_some_and(|(parameters, is_sendable)| {
            parameters.payload_type != binding.payload_type || is_sendable != binding.codec.is_some()
        })
    }

    /// Sends a frame, if the track is bound in a codec that the renderer encodes.
    pub async fn write_sample(&self, sample: &Sample) -> Result<()> {
        let track = self
            .binding
            .lock()
            .await
            .as_ref()
            .filter(|binding| binding.codec.is_some())
            .map(|binding| binding.track.clone());
        match track {
            Some(track) => track.write_sample(sample).await,
            None => Ok(()),
        }
    }
}

#[async_trait]
impl TrackLocal for NegotiatedVideoTrack {
    async fn bind(&self, context: &TrackLocalContext) -> Result<RTCRtpCodecParameters> {
        let Some((parameters, is_sendable)) = self.choose(context.codec_parameters()) else {
            (self.on_codec)(None);
            return Err(Error::ErrUnsupportedCodec);
        };
        let track = Arc::new(TrackLocalStaticSample::new(
            parameters.capability.clone(),
            self.id.clone(),
            self.stream_id.clone(),
        ));
        let bound = track.bind(context).await?;
        let codec = VideoCodec::from_mime_type(&bound.capability.mime_type).filter(|_| is_sendable);
        *self.binding.lock().await = Some(Binding {
            payload_type: bound.payload_type,
            codec,
            track,
        });
        (self.on_codec)(codec.map(|codec| (codec, bound.payload_type)));
        Ok(bound)
    }

    async fn unbind(&self, context: &TrackLocalContext) -> Result<()> {
        match self.binding.lock().await.take() {
            Some(binding) => binding.track.unbind(context).await,
            None => Ok(()),
        }
    }

    fn id(&self) -> &str {
        &self.id
    }

    fn rid(&self) -> Option<&str> {
        None
    }

    fn stream_id(&self) -> &str {
        &self.stream_id
    }

    fn kind(&self) -> RTPCodecType {
        RTPCodecType::Video
    }

    fn as_any(&self) -> &dyn Any {
        self
    }
}

#[cfg(test)]
mod tests {
    use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;

    use super::*;

    fn parameters(mime_type: &str, payload_type: u8) -> RTCRtpCodecParameters {
        RTCRtpCodecParameters {
            capability: RTCRtpCodecCapability {
                mime_type: mime_type.to_owned(),
                clock_rate: 90000,
                ..Default::default()
            },
            payload_type,
            ..Default::default()
        }
    }

    fn track() -> NegotiatedVideoTrack {
        NegotiatedVideoTrack::new("video".to_owned(), "stream".to_owned(), Box::new(|_| {}))
    }

    #[test]
    fn takes_the_first_negotiated_codec_that_can_be_encoded() {
        let negotiated = [
            parameters("video/H264", 102),
            parameters("video/rtx", 103),
            parameters("video/VP8", 96),
            parameters("video/VP9", 98),
        ];
        let track = track();

        // Before the renderer has said what it encodes, the other end's first choice stands.
        let (chosen, is_sendable) = track.choose(&negotiated).unwrap();
        assert_eq!((chosen.payload_type, is_sendable), (102, true));

        track.set_sendable_codecs(vec![VideoCodec::Vp9, VideoCodec::Vp8]);
        let (chosen, is_sendable) = track.choose(&negotiated).unwrap();
        assert_eq!((chosen.payload_type, is_sendable), (96, true));
    }

    #[test]
    fn stays_in_a_session_that_has_nothing_it_can_encode() {
        let negotiated = [parameters("video/rtx", 103), parameters("video/H264", 102)];
        let track = track();
        track.set_sendable_codecs(vec![VideoCodec::Vp8]);
        let (chosen, is_sendable) = track.choose(&negotiated).unwrap();
        assert_eq!((chosen.payload_type, is_sendable), (102, false));

        assert!(track.choose(&[parameters("video/rtx", 103)]).is_none());
    }
}
