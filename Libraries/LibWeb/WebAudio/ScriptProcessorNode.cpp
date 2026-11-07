/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Weak.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorPipe.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio {

extern "C" void web_rust_script_processor_process(JSVM*, void*, size_t, u64);
extern "C" RustFFI::FfiSharedCppObject web_audio_script_processor_create_pipe(Bindings::RustInterfaceCell*, RustFFI::ScriptProcessorPipeConfig, void const**);
extern "C" void web_audio_script_processor_clear_callback(Rendering::ScriptProcessorPipe*);
RustFFI::FfiSharedCppObject web_audio_script_processor_create_pipe(Bindings::RustInterfaceCell* node, RustFFI::ScriptProcessorPipeConfig config, void const** native_pipe)
{
    auto pipe = Rendering::ScriptProcessorPipe::create({ config.buffer_size, config.input_channel_count, config.output_channel_count, config.block_count, config.waits_for_processing }, Core::EventLoop::current());
    pipe->set_process_callback([node = GC::Weak { *node }](size_t block, u64 start_frame) {
        if (node)
            web_rust_script_processor_process(JS::HostABI::vm_to_abi(JS::VM::the()), node.ptr().ptr(), block, start_frame);
    });
    *native_pipe = pipe->rust_pipe();
    return { &pipe.leak_ref(), [](void* pipe) { static_cast<Rendering::ScriptProcessorPipe*>(pipe)->ref(); }, [](void* pipe) { static_cast<Rendering::ScriptProcessorPipe*>(pipe)->unref(); } };
}
void web_audio_script_processor_clear_callback(Rendering::ScriptProcessorPipe* pipe) { pipe->clear_process_callback(); }

}
