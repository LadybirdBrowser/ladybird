/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Media {

class AudioDecoder;
class AudioMixer;
class AudioPlaybackSink;
class AudioProducer;
class AudioSink;
class CodedFrame;
class DecodedAudioProducer;
class DecodedVideoProducer;
class DecoderError;
class Demuxer;
class DisplayingVideoSink;
class FrameQueueItem;
class IncrementallyPopulatedStream;
class MediaStream;
class MediaStreamCursor;
class MediaTimeProvider;
class PlaybackManager;
class ReadonlyBytesCursor;
class Track;
class VideoDecoder;
class VideoFrame;
class VideoProducer;
class VideoSink;

}
