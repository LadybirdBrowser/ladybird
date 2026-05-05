/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/RenderNodes/MediaElementAudioSourceProvider.h>
#include <LibWebAudio/SharedAudioBuffer.h>

namespace Web::WebAudio::Render {

class ScriptProcessorHost;

class GraphResources {
public:
    struct ScriptProcessorTransportMetadata {
        u32 buffer_size { 0 };
        u32 input_channel_count { 0 };
        u32 output_channel_count { 0 };
    };

    HashMap<MediaElementAudioSourceProviderID, NonnullRefPtr<MediaElementAudioSourceProvider>>
        media_element_audio_sources;
    HashMap<MediaStreamAudioSourceProviderID, NonnullRefPtr<MediaElementAudioSourceProvider>>
        media_stream_audio_sources;
    HashMap<MediaStreamAudioSourceProviderID, AudioInputStreamMetadata> media_stream_audio_source_metadata;
    HashMap<ResourceID, NonnullRefPtr<SharedAudioBuffer>> audio_buffers;
    ScriptProcessorHost* script_processor_host { nullptr };
    HashMap<NodeID, ScriptProcessorTransportMetadata> script_processor_transport_metadata;
};

} // namespace Web::WebAudio::Render
