/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/CyclicModule.h>
#include <LibJS/Export.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWeb/Forward.h>

namespace Web::WebAssembly {

// 16.2.1.6 Source Text Module Records, https://tc39.es/ecma262/#sec-source-text-module-records
class WebAssemblyModule final : public JS::CyclicModule {
    GC_CELL(WebAssemblyModule, JS::CyclicModule);
    GC_DECLARE_ALLOCATOR(WebAssemblyModule);

public:
    virtual ~WebAssemblyModule() override;

    static JS::ThrowCompletionOr<GC::Ref<WebAssemblyModule>> parse(ByteBuffer bytes, JS::Realm&, StringView filename = {}, JS::Script::HostDefined* host_defined = nullptr);

    Vector<Utf16FlyString> export_name_list();

    virtual Vector<Utf16FlyString> get_exported_names(JS::VM& vm, HashTable<Module const*>& export_star_set) override;
    virtual JS::ResolvedBinding resolve_export(JS::VM& vm, Utf16FlyString const& export_name, Vector<JS::ResolvedBinding> resolve_set = {}) override;

protected:
    virtual JS::ThrowCompletionOr<void> initialize_environment(JS::VM& vm) override;
    virtual JS::ThrowCompletionOr<void> execute_module(JS::VM& vm, GC::Ptr<JS::PromiseCapability> capability) override;

private:
    WebAssemblyModule(JS::Realm&, StringView filename, WebAssembly::Module& module_source, JS::Script::HostDefined* host_defined, Vector<JS::ModuleRequest> requested_modules);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<Instance> m_instance;                 // [[Instance]]
    GC::Ref<WebAssembly::Module> m_module_source; // [[ModuleSource]]
    GC::Ptr<WebAssemblyModule> m_module_record;   // [[ModuleRecord]]

    Optional<Vector<Utf16FlyString>> m_cached_export_name_list;
};

}
