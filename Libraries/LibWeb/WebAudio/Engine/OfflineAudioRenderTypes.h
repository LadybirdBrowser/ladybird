/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>

namespace Web::WebAudio::Render {

struct OfflineAudioGraphUpdate {
    GraphDescription graph;
    GraphResourceRegistry resources;
};

struct OfflineAudioRenderRequest {
    GraphDescription graph;
    NonnullOwnPtr<GraphResourceRegistry> resources { make<GraphResourceRegistry>() };
    u32 number_of_channels { 2 };
    u32 length_in_sample_frames { 0 };
    f32 sample_rate { 44100.0f };
    u32 render_quantum_size { 0 };

    // Quantum-aligned frame indices at which rendering should suspend before processing the quantum.
    // Used to implement OfflineAudioContext.suspend()/resume().
    Vector<u32> suspend_frame_indices;
};

struct OfflineAudioRenderResult {
    Vector<Vector<f32>> rendered_channels;

    // Captured input signals for analyser nodes, keyed by NodeID.
    // Each buffer is of length analyser.fftSize and contains the most recent frames.
    HashMap<NodeID, Vector<f32>> analyser_time_domain_data;

    // Current frequency data (in dB) for analyser nodes, keyed by NodeID.
    // Each buffer is of length analyser.frequencyBinCount.
    HashMap<NodeID, Vector<f32>> analyser_frequency_data_db;
};

// Analyser snapshot captured at a specific quantum-aligned frame index.
// Used to make analyser data observable during OfflineAudioContext suspension.
struct OfflineAudioAnalyserSnapshot {
    u32 frame_index { 0 };
    size_t render_quantum_index { 0 };

    HashMap<NodeID, Vector<f32>> analyser_time_domain_data;
    HashMap<NodeID, Vector<f32>> analyser_frequency_data_db;
};

}
