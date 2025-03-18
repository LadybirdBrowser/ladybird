/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Module.h>

namespace JS {

// 1.2 Synthetic Module Records, https://tc39.es/proposal-json-modules/#sec-synthetic-module-records
class SyntheticModule final : public Module {
    GC_CELL(SyntheticModule, Module);
    GC_DECLARE_ALLOCATOR(SyntheticModule);

public:
    using EvaluationFunction = Function<ThrowCompletionOr<void>(SyntheticModule&)>;

    static GC::Ref<SyntheticModule> create_default_export_synthetic_module(Value default_export, Realm& realm, StringView filename);

    ThrowCompletionOr<void> set_synthetic_module_export(FlyString const& export_name, Value export_value);

    virtual ThrowCompletionOr<void> link(VM& vm) override;
    virtual ThrowCompletionOr<Promise*> evaluate(VM& vm) override;
    virtual ThrowCompletionOr<Vector<FlyString>> get_exported_names(VM& vm, Vector<Module*> export_star_set) override;
    virtual ThrowCompletionOr<ResolvedBinding> resolve_export(VM& vm, FlyString const& export_name, Vector<ResolvedBinding> resolve_set) override;
    virtual PromiseCapability& load_requested_modules(GC::Ptr<GraphLoadingState::HostDefined>) override;

private:
    SyntheticModule(Vector<FlyString> export_names, EvaluationFunction evaluation_steps, Realm& realm, StringView filename);

    Vector<FlyString> m_export_names;      // [[ExportNames]]
    EvaluationFunction m_evaluation_steps; // [[EvaluationSteps]]
};

ThrowCompletionOr<GC::Ref<Module>> parse_json_module(StringView source_text, Realm& realm, StringView filename);

}
