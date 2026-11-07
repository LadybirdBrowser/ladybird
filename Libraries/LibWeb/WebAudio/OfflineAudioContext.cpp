/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/WebAudio/Rendering/OfflineAudioRenderer.h>
#include <LibWeb/WebAudio/RustFFI.h>
#include <LibWeb/WebIDL/Promise.h>
namespace Web::WebAudio {

extern "C" void web_rust_audio_context_sources_ended(JSVM*, void*, ReadonlySpan<u64>);
extern "C" void web_rust_offline_audio_context_complete(JSVM*, void*, void*);
extern "C" void web_rust_offline_audio_context_suspended(JSVM*, void*, double);
extern "C" RustFFI::FfiSharedCppObject web_audio_offline_context_create(void const*, u64, u32, u32, float);
extern "C" void web_audio_offline_context_set_callbacks(Rendering::OfflineAudioRenderer*, Bindings::RustInterfaceCell*, JS::PromiseCapability*);
extern "C" bool web_audio_offline_context_request_suspend(Rendering::OfflineAudioRenderer*, u64);
extern "C" void web_audio_offline_context_channel(Rendering::OfflineAudioRenderer*, size_t, ReadonlySpan<float>*);
RustFFI::FfiSharedCppObject web_audio_offline_context_create(void const* queue, u64 destination, u32 channels, u32 length, float sample_rate)
{
    auto renderer = Rendering::OfflineAudioRenderer::create(queue, NodeID(destination), channels, length, sample_rate, 128);
    return { &renderer.leak_ref(), [](void* renderer) { static_cast<Rendering::OfflineAudioRenderer*>(renderer)->ref(); }, [](void* renderer) { static_cast<Rendering::OfflineAudioRenderer*>(renderer)->unref(); } };
}
void web_audio_offline_context_set_callbacks(Rendering::OfflineAudioRenderer* renderer, Bindings::RustInterfaceCell* context, JS::PromiseCapability* promise)
{
    renderer->set_on_complete([context = GC::make_root(*context), promise = GC::make_root(*promise)] { web_rust_offline_audio_context_complete(JS::HostABI::vm_to_abi(JS::VM::the()), context.ptr(), promise.ptr()); });
    renderer->set_on_suspended([context = GC::make_root(*context)](double time) { web_rust_offline_audio_context_suspended(JS::HostABI::vm_to_abi(JS::VM::the()), context.ptr(), time); });
    renderer->set_on_sources_ended([context = GC::make_root(*context)](Vector<NodeID> const& nodes) {
        Vector<u64> ids;
        ids.ensure_capacity(nodes.size());
        for (auto node : nodes)
            ids.unchecked_append(node.value());
        web_rust_audio_context_sources_ended(JS::HostABI::vm_to_abi(JS::VM::the()), context.ptr(), ids);
    });
}
bool web_audio_offline_context_request_suspend(Rendering::OfflineAudioRenderer* renderer, u64 frame) { return renderer->request_suspend(frame); }
void web_audio_offline_context_channel(Rendering::OfflineAudioRenderer* renderer, size_t channel, ReadonlySpan<float>* samples) { *samples = renderer->rendered_channels()[channel]; }
extern "C" void web_audio_offline_context_start(Rendering::OfflineAudioRenderer*);
void web_audio_offline_context_start(Rendering::OfflineAudioRenderer* renderer) { renderer->start_rendering(); }
extern "C" void web_audio_offline_context_resume(Rendering::OfflineAudioRenderer*);
void web_audio_offline_context_resume(Rendering::OfflineAudioRenderer* renderer) { renderer->resume(); }
extern "C" void web_audio_offline_context_stop(Rendering::OfflineAudioRenderer*);
void web_audio_offline_context_stop(Rendering::OfflineAudioRenderer* renderer) { renderer->stop(); }
extern "C" void web_audio_offline_context_clear_callbacks(Rendering::OfflineAudioRenderer*);
void web_audio_offline_context_clear_callbacks(Rendering::OfflineAudioRenderer* renderer) { renderer->clear_callbacks(); }
extern "C" u64 web_audio_offline_context_frames_rendered(Rendering::OfflineAudioRenderer*);
u64 web_audio_offline_context_frames_rendered(Rendering::OfflineAudioRenderer* renderer) { return renderer->frames_rendered(); }

}
