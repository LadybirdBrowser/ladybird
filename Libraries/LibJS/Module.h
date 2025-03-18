/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/ModuleLoading.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>

namespace JS {

struct ResolvedBinding {
    enum Type {
        BindingName,
        Namespace,
        Ambiguous,
        Null,
    };

    static ResolvedBinding null()
    {
        return {};
    }

    static ResolvedBinding ambiguous()
    {
        ResolvedBinding binding;
        binding.type = Ambiguous;
        return binding;
    }

    Type type { Null };
    GC::Ptr<Module> module;
    FlyString export_name;

    bool is_valid() const
    {
        return type == BindingName || type == Namespace;
    }

    bool is_namespace() const
    {
        return type == Namespace;
    }

    bool is_ambiguous() const
    {
        return type == Ambiguous;
    }
};

// https://tc39.es/ecma262/#graphloadingstate-record
struct GraphLoadingState : public Cell {
    GC_CELL(GraphLoadingState, Cell);
    GC_DECLARE_ALLOCATOR(GraphLoadingState);

public:
    struct HostDefined : Cell {
        GC_CELL(HostDefined, Cell);

    public:
        virtual ~HostDefined() = default;
    };

    GC::Ptr<PromiseCapability> promise_capability; // [[PromiseCapability]]
    bool is_loading { false };                     // [[IsLoading]]
    size_t pending_module_count { 0 };             // [[PendingModulesCount]]
    HashTable<GC::Ptr<CyclicModule>> visited;      // [[Visited]]
    GC::Ptr<HostDefined> host_defined;             // [[HostDefined]]

private:
    GraphLoadingState(GC::Ptr<PromiseCapability> promise_capability, bool is_loading, size_t pending_module_count, HashTable<GC::Ptr<CyclicModule>> visited, GC::Ptr<HostDefined> host_defined)
        : promise_capability(move(promise_capability))
        , is_loading(is_loading)
        , pending_module_count(pending_module_count)
        , visited(move(visited))
        , host_defined(move(host_defined))
    {
    }
    virtual void visit_edges(Cell::Visitor&) override;
};

// 16.2.1.4 Abstract Module Records, https://tc39.es/ecma262/#sec-abstract-module-records
class Module : public Cell {
    GC_CELL(Module, Cell);
    GC_DECLARE_ALLOCATOR(Module);

public:
    virtual ~Module() override;

    Realm& realm() { return *m_realm; }
    Realm const& realm() const { return *m_realm; }

    StringView filename() const { return m_filename; }

    GC::Ptr<ModuleEnvironment> environment() { return m_environment; }

    Script::HostDefined* host_defined() const { return m_host_defined; }

    ThrowCompletionOr<Object*> get_module_namespace(VM& vm);

    virtual ThrowCompletionOr<void> link(VM& vm) = 0;
    virtual ThrowCompletionOr<Promise*> evaluate(VM& vm) = 0;

    virtual ThrowCompletionOr<Vector<FlyString>> get_exported_names(VM& vm, Vector<Module*> export_star_set = {}) = 0;
    virtual ThrowCompletionOr<ResolvedBinding> resolve_export(VM& vm, FlyString const& export_name, Vector<ResolvedBinding> resolve_set = {}) = 0;

    virtual ThrowCompletionOr<u32> inner_module_linking(VM& vm, Vector<Module*>& stack, u32 index);
    virtual ThrowCompletionOr<u32> inner_module_evaluation(VM& vm, Vector<Module*>& stack, u32 index);

    virtual PromiseCapability& load_requested_modules(GC::Ptr<GraphLoadingState::HostDefined>) = 0;

protected:
    Module(Realm&, ByteString filename, Script::HostDefined* host_defined = nullptr);

    virtual void visit_edges(Cell::Visitor&) override;

    void set_environment(GC::Ref<ModuleEnvironment> environment)
    {
        m_environment = environment;
    }

private:
    Object* module_namespace_create(Vector<FlyString> unambiguous_names);

    // These handles are only safe as long as the VM they live in is valid.
    // But evaluated modules SHOULD be stored in the VM so unless you intentionally
    // destroy the VM but keep the modules this should not happen. Because VM
    // stores modules with a RefPtr we cannot just store the VM as that leads to
    // cycles.
    GC::Ptr<Realm> m_realm;                          // [[Realm]]
    GC::Ptr<ModuleEnvironment> m_environment;        // [[Environment]]
    GC::Ptr<Object> m_namespace;                     // [[Namespace]]
    Script::HostDefined* m_host_defined { nullptr }; // [[HostDefined]]

    // Needed for potential lookups of modules.
    ByteString m_filename;
};

class CyclicModule;
struct GraphLoadingState;

void finish_loading_imported_module(ImportedModuleReferrer, ModuleRequest const&, ImportedModulePayload, ThrowCompletionOr<GC::Ref<Module>> const&);

}
