/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibGC/Weak.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>

namespace Web::WebAudio {

class BaseAudioContext;
class SessionClientOfWebAudioWorker;

}

namespace Web::WebAudio::Render {

void install_worklet_event_callbacks(SessionClientOfWebAudioWorker&);
void register_worklet_context(u64 session_id, GC::Weak<Web::WebAudio::BaseAudioContext> context);
void unregister_worklet_context(u64 session_id);
void clear_all_worklet_contexts();

void note_worklet_processor_registration(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation);
void note_worklet_failed_processor_registrations(u64 session_id, Vector<String> const& failed_processor_registrations);
bool get_worklet_replay_state_for_generation(u64 session_id, u64 required_generation, HashMap<String, Vector<Web::WebAudio::AudioParamDescriptor>>& out_registered_descriptors, HashTable<String>& out_failed_registrations, u64& out_last_generation);
void clear_worklet_session_state(u64 session_id);
void clear_all_worklet_session_state();

}
