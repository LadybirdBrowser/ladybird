/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/ModuleLoading.h>
#include <LibJS/Runtime/Completion.h>
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
    Utf16FlyString export_name;

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
// The state of loading a module graph, which the runtime keeps to itself; a host only passes it back to
// finish_loading_imported_module() in the payload it received.
struct GraphLoadingState final : public EngineCell {
};

// 16.2.1.4 Abstract Module Records, https://tc39.es/ecma262/#sec-abstract-module-records
// A Module Record of the Rust runtime. The facade types of its kinds derive from Module. The abstract methods dispatch
// to the record's kind inside the runtime.
class JS_API Module : public EngineCell {
public:
    Realm& realm();
    Realm const& realm() const;

    GC::Ptr<ModuleEnvironment> environment();

    GC::Ptr<GC::Cell> host_defined() const;

    ThrowCompletionOr<void> link(VM& vm);
    ThrowCompletionOr<GC::Ref<PromiseCapability>> evaluate(VM& vm);

    // The runtime resolves an export with a resolve set of its own, so a host passes none.
    ResolvedBinding resolve_export(VM& vm, Utf16FlyString const& export_name, Vector<ResolvedBinding> resolve_set = {});

    PromiseCapability& load_requested_modules(GC::Ptr<GC::Cell> host_defined);

protected:
    // Only for host modules, whose initialize_environment hook creates their environment.
    void set_environment(GC::Ref<ModuleEnvironment> environment);
};

JS_API void finish_loading_imported_module(ImportedModuleReferrer, ModuleRequest const&, ImportedModulePayload, ThrowCompletionOr<GC::Ref<Module>> const&);

}
