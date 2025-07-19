/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Module.h>

namespace JS {

// 16.2.1.8 Synthetic Module Records, https://tc39.es/ecma262/#sec-synthetic-module-records
class SyntheticModule final : public Module {
    GC_CELL(SyntheticModule, Module);
    GC_DECLARE_ALLOCATOR(SyntheticModule);

public:
    using EvaluationFunction = GC::Ref<GC::Function<ThrowCompletionOr<void>(SyntheticModule&)>>;

    static GC::Ref<SyntheticModule> create_default_export_synthetic_module(Realm& realm, Value default_export, ByteString filename);

    ThrowCompletionOr<void> set_synthetic_module_export(FlyString const& export_name, Value export_value);

    virtual PromiseCapability& load_requested_modules(GC::Ptr<GraphLoadingState::HostDefined>) override;
    virtual Vector<FlyString> get_exported_names(VM& vm, HashTable<Module const*>& export_star_set) override;
    virtual ResolvedBinding resolve_export(VM& vm, FlyString const& export_name, Vector<ResolvedBinding> resolve_set) override;
    virtual ThrowCompletionOr<void> link(VM& vm) override;
    virtual ThrowCompletionOr<GC::Ref<Promise>> evaluate(VM& vm) override;

private:
    SyntheticModule(Realm& realm, Vector<FlyString> export_names, EvaluationFunction evaluation_steps, ByteString filename);

    virtual void visit_edges(Cell::Visitor&) override;

    Vector<FlyString> m_export_names;      // [[ExportNames]]
    EvaluationFunction m_evaluation_steps; // [[EvaluationSteps]]
};

ThrowCompletionOr<GC::Ref<Module>> parse_json_module(Realm& realm, StringView source_text, ByteString filename);

}
