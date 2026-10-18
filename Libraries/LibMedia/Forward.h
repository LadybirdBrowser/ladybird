/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMediaCodecs/Forward.h>

namespace Media {

class AudioDecoder;
class AudioMixer;
class AudioOutputFactory;
class AudioPlaybackSink;
class AudioProducer;
class AudioSink;
class ContainerNavigator;
class DecodedAudioProducer;
class DecodedVideoProducer;
class Demuxer;
class DisplayingVideoSink;
class FrameQueueItem;
class FrameScanTimeline;
class IncrementallyPopulatedStream;
class MediaStream;
class MediaStreamCursor;
class MediaClock;
class PlaybackManager;
class ReadonlyBytesCursor;
class AudioTimeStretchProcessor;
class Track;
class VideoProducer;
class VideoPresentationServerConnection;
class VideoSink;

}
