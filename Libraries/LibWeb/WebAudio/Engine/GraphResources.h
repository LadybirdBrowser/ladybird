/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AudioServer/AudioInputDeviceInfo.h>
#include <LibWeb/WebAudio/Engine/SharedAudioBuffer.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceProvider.h>

namespace Web::WebAudio::Render {

class ScriptProcessorHost;

class GraphResourceResolver {
public:
    virtual ~GraphResourceResolver() = default;

    virtual RefPtr<MediaElementAudioSourceProvider> resolve_media_element_audio_source(MediaElementAudioSourceProviderID provider_id) const = 0;
    virtual RefPtr<MediaElementAudioSourceProvider> resolve_media_stream_audio_source(MediaStreamAudioSourceProviderID provider_id) const = 0;
    virtual RefPtr<SharedAudioBuffer> resolve_audio_buffer(u64 buffer_id) const = 0;
};

class NullGraphResourceResolver final : public GraphResourceResolver {
public:
    static NullGraphResourceResolver const& the()
    {
        static NullGraphResourceResolver s_instance;
        return s_instance;
    }

    virtual RefPtr<MediaElementAudioSourceProvider> resolve_media_element_audio_source(MediaElementAudioSourceProviderID) const override
    {
        return nullptr;
    }

    virtual RefPtr<MediaElementAudioSourceProvider> resolve_media_stream_audio_source(MediaStreamAudioSourceProviderID) const override
    {
        return nullptr;
    }

    virtual RefPtr<SharedAudioBuffer> resolve_audio_buffer(u64) const override
    {
        return nullptr;
    }

private:
    NullGraphResourceResolver() = default;
};

class GraphResourceRegistry final : public GraphResourceResolver {
public:
    struct ScriptProcessorTransportMetadata {
        u32 buffer_size { 0 };
        u32 input_channel_count { 0 };
        u32 output_channel_count { 0 };
    };

    bool has_media_element_audio_sources() const
    {
        return !m_media_element_sources.is_empty();
    }

    bool has_media_stream_audio_sources() const
    {
        return !m_media_stream_sources.is_empty();
    }

    bool has_media_stream_audio_source_metadata() const
    {
        return !m_media_stream_source_metadata.is_empty();
    }

    bool has_audio_buffers() const
    {
        return !m_audio_buffers.is_empty();
    }

    void set_media_element_audio_source(MediaElementAudioSourceProviderID id, NonnullRefPtr<MediaElementAudioSourceProvider> provider)
    {
        m_media_element_sources.set(id, move(provider));
    }

    void clear_media_element_audio_sources()
    {
        m_media_element_sources.clear();
    }

    void set_media_stream_audio_source(MediaStreamAudioSourceProviderID id, NonnullRefPtr<MediaElementAudioSourceProvider> provider)
    {
        m_media_stream_sources.set(id, move(provider));
    }

    void clear_media_stream_audio_sources()
    {
        m_media_stream_sources.clear();
    }

    void set_media_stream_audio_source_metadata(MediaStreamAudioSourceProviderID id, AudioInputStreamMetadata metadata)
    {
        m_media_stream_source_metadata.set(id, metadata);
    }

    void clear_media_stream_audio_source_metadata()
    {
        m_media_stream_source_metadata.clear();
    }

    void set_audio_buffer(u64 buffer_id, NonnullRefPtr<SharedAudioBuffer> buffer)
    {
        m_audio_buffers.set(buffer_id, move(buffer));
    }

    void clear_audio_buffers()
    {
        m_audio_buffers.clear();
    }

    void set_script_processor_host(ScriptProcessorHost* host)
    {
        m_script_processor_host = host;
    }

    ScriptProcessorHost* script_processor_host() const
    {
        return m_script_processor_host;
    }

    void set_script_processor_transport_metadata(NodeID node_id, ScriptProcessorTransportMetadata metadata)
    {
        m_script_processor_transport_metadata.set(node_id, metadata);
    }

    void clear_script_processor_transport_metadata()
    {
        m_script_processor_transport_metadata.clear();
    }

    HashMap<NodeID, ScriptProcessorTransportMetadata> const& script_processor_transport_metadata() const
    {
        return m_script_processor_transport_metadata;
    }

    NonnullOwnPtr<GraphResourceRegistry> clone() const
    {
        auto copy = make<GraphResourceRegistry>();
        copy->m_media_element_sources = m_media_element_sources;
        copy->m_media_stream_sources = m_media_stream_sources;
        copy->m_media_stream_source_metadata = m_media_stream_source_metadata;
        copy->m_audio_buffers = m_audio_buffers;
        copy->m_script_processor_host = m_script_processor_host;
        copy->m_script_processor_transport_metadata = m_script_processor_transport_metadata;
        return copy;
    }

    void merge_media_element_audio_sources_from(GraphResourceRegistry&& other)
    {
        for (auto& it : other.m_media_element_sources)
            m_media_element_sources.set(it.key, move(it.value));
        other.m_media_element_sources.clear();
    }

    void merge_media_stream_audio_sources_from(GraphResourceRegistry&& other)
    {
        for (auto& it : other.m_media_stream_sources)
            m_media_stream_sources.set(it.key, move(it.value));
        other.m_media_stream_sources.clear();
    }

    void merge_media_stream_audio_source_metadata_from(GraphResourceRegistry&& other)
    {
        for (auto& it : other.m_media_stream_source_metadata)
            m_media_stream_source_metadata.set(it.key, it.value);
        other.m_media_stream_source_metadata.clear();
    }

    HashMap<MediaElementAudioSourceProviderID, NonnullRefPtr<MediaElementAudioSourceProvider>> const& media_element_audio_sources() const
    {
        return m_media_element_sources;
    }

    HashMap<MediaStreamAudioSourceProviderID, NonnullRefPtr<MediaElementAudioSourceProvider>> const& media_stream_audio_sources() const
    {
        return m_media_stream_sources;
    }

    HashMap<MediaStreamAudioSourceProviderID, AudioInputStreamMetadata> const& media_stream_audio_source_metadata() const
    {
        return m_media_stream_source_metadata;
    }

    virtual RefPtr<MediaElementAudioSourceProvider> resolve_media_element_audio_source(MediaElementAudioSourceProviderID provider_id) const override
    {
        if (auto it = m_media_element_sources.find(provider_id); it != m_media_element_sources.end())
            return it->value;
        return nullptr;
    }

    virtual RefPtr<MediaElementAudioSourceProvider> resolve_media_stream_audio_source(MediaStreamAudioSourceProviderID provider_id) const override
    {
        if (auto it = m_media_stream_sources.find(provider_id); it != m_media_stream_sources.end())
            return it->value;
        return nullptr;
    }

    virtual RefPtr<SharedAudioBuffer> resolve_audio_buffer(u64 buffer_id) const override
    {
        if (auto it = m_audio_buffers.find(buffer_id); it != m_audio_buffers.end())
            return it->value;
        return nullptr;
    }

private:
    HashMap<MediaElementAudioSourceProviderID, NonnullRefPtr<MediaElementAudioSourceProvider>> m_media_element_sources;
    HashMap<MediaStreamAudioSourceProviderID, NonnullRefPtr<MediaElementAudioSourceProvider>> m_media_stream_sources;
    HashMap<MediaStreamAudioSourceProviderID, AudioInputStreamMetadata> m_media_stream_source_metadata;
    HashMap<u64, NonnullRefPtr<SharedAudioBuffer>> m_audio_buffers;
    ScriptProcessorHost* m_script_processor_host { nullptr };

    HashMap<NodeID, ScriptProcessorTransportMetadata> m_script_processor_transport_metadata;
};

}
