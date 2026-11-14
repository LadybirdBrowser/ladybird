/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::DOM {

extern "C" {
void* web_rust_abort_signal_create(JSVM*);
bool web_rust_abort_signal_aborted(AbortSignal const*);
JSValue web_rust_abort_signal_reason(AbortSignal const*);
bool web_rust_abort_signal_add_abort_algorithm(JSVM*, AbortSignal*, GC::Function<void()>*, u64* id);
void web_rust_abort_signal_remove_abort_algorithm(JSVM*, AbortSignal*, u64 id);
void web_rust_abort_signal_signal_abort(JSVM*, AbortSignal*, JSValue reason, void* realm);
void* web_rust_abort_signal_create_dependent_abort_signal(JSVM*, ReadonlySpan<GC::Ref<AbortSignal>>);
void web_dom_run_abort_algorithm(GC::Function<void()>*);
}

GC::Ref<AbortSignal> AbortSignal::create()
{
    return *static_cast<AbortSignal*>(web_rust_abort_signal_create(JS::HostABI::vm_to_abi(JS::VM::the())));
}

bool AbortSignal::aborted() const
{
    return web_rust_abort_signal_aborted(this);
}

JS::Value AbortSignal::reason() const
{
    return JS::HostABI::value_from_abi(web_rust_abort_signal_reason(this));
}

Optional<AbortSignal::AbortAlgorithmID> AbortSignal::add_abort_algorithm(GC::Ref<GC::Function<void()>> algorithm)
{
    u64 id = 0;
    if (!web_rust_abort_signal_add_abort_algorithm(JS::HostABI::vm_to_abi(JS::VM::the()), this, algorithm.ptr(), &id))
        return {};
    return id;
}

void AbortSignal::remove_abort_algorithm(AbortAlgorithmID id)
{
    web_rust_abort_signal_remove_abort_algorithm(JS::HostABI::vm_to_abi(JS::VM::the()), this, id);
}

void AbortSignal::signal_abort(JS::Value reason, JS::Object& relevant_global_object)
{
    web_rust_abort_signal_signal_abort(JS::HostABI::vm_to_abi(JS::VM::the()), this, JS::HostABI::value_to_abi(reason), &HTML::relevant_realm(relevant_global_object));
}

WebIDL::ExceptionOr<GC::Ref<AbortSignal>> AbortSignal::create_dependent_abort_signal(GC::RootVector<GC::Ref<AbortSignal>> const& signals)
{
    return GC::Ref { *static_cast<AbortSignal*>(web_rust_abort_signal_create_dependent_abort_signal(JS::HostABI::vm_to_abi(JS::VM::the()), signals)) };
}

void web_dom_run_abort_algorithm(GC::Function<void()>* algorithm)
{
    algorithm->function()();
}

}
