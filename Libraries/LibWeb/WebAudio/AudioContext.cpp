/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebAudio/Rendering/RealtimeAudioRenderer.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio {

extern "C" void web_rust_audio_context_sources_ended(JSVM*, void*, ReadonlySpan<u64>);
extern "C" RustFFI::FfiSharedCppObject web_audio_realtime_context_create(DOM::EventTarget*, void const*, u64, float);
extern "C" void web_audio_realtime_context_set_callbacks(Rendering::RealtimeAudioRenderer*, Bindings::RustInterfaceCell*);
extern "C" double web_audio_realtime_context_performance_now(DOM::EventTarget*);
RustFFI::FfiSharedCppObject web_audio_realtime_context_create(DOM::EventTarget* global, void const* queue, u64 destination, float sample_rate)
{
    auto factory = as<HTML::Window>(*global).page().client().create_audio_output_factory();
    auto renderer = Rendering::RealtimeAudioRenderer::create(queue, NodeID(destination), sample_rate, 128, move(factory));
    return { &renderer.leak_ref(), [](void* renderer) { static_cast<Rendering::RealtimeAudioRenderer*>(renderer)->ref(); }, [](void* renderer) { static_cast<Rendering::RealtimeAudioRenderer*>(renderer)->unref(); } };
}
void web_audio_realtime_context_set_callbacks(Rendering::RealtimeAudioRenderer* renderer, Bindings::RustInterfaceCell* context)
{
    renderer->set_on_sources_ended([context = GC::make_root(*context)](Vector<NodeID> const& nodes) {
        Vector<u64> ids;
        ids.ensure_capacity(nodes.size());
        for (auto node : nodes)
            ids.unchecked_append(node.value());
        web_rust_audio_context_sources_ended(JS::HostABI::vm_to_abi(JS::VM::the()), context.ptr(), ids);
    });
}
double web_audio_realtime_context_performance_now(DOM::EventTarget* global) { return as<HTML::Window>(*global).performance()->now(); }
extern "C" void web_audio_realtime_context_start(Rendering::RealtimeAudioRenderer*);
void web_audio_realtime_context_start(Rendering::RealtimeAudioRenderer* renderer) { renderer->start_rendering(); }
extern "C" void web_audio_realtime_context_resume(Rendering::RealtimeAudioRenderer*);
void web_audio_realtime_context_resume(Rendering::RealtimeAudioRenderer* renderer) { renderer->resume(); }
extern "C" void web_audio_realtime_context_suspend(Rendering::RealtimeAudioRenderer*);
void web_audio_realtime_context_suspend(Rendering::RealtimeAudioRenderer* renderer) { renderer->suspend(); }
extern "C" void web_audio_realtime_context_stop(Rendering::RealtimeAudioRenderer*);
void web_audio_realtime_context_stop(Rendering::RealtimeAudioRenderer* renderer) { renderer->stop(); }
extern "C" void web_audio_realtime_context_clear_callbacks(Rendering::RealtimeAudioRenderer*);
void web_audio_realtime_context_clear_callbacks(Rendering::RealtimeAudioRenderer* renderer) { renderer->clear_callbacks(); }
extern "C" u64 web_audio_realtime_context_frames_rendered(Rendering::RealtimeAudioRenderer*);
u64 web_audio_realtime_context_frames_rendered(Rendering::RealtimeAudioRenderer* renderer) { return renderer->frames_rendered(); }
extern "C" double web_audio_realtime_context_output_time(Rendering::RealtimeAudioRenderer*);
double web_audio_realtime_context_output_time(Rendering::RealtimeAudioRenderer* renderer) { return renderer->output_time_played(); }

}
