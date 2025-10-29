/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/AsyncFunctionDriverWrapper.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/SourceTextModule.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SourceTextModule);

// 16.2.2.4 Static Semantics: WithClauseToAttributes, https://tc39.es/ecma262/#sec-withclausetoattributes
static Vector<ImportAttribute> with_clause_to_assertions(Vector<ImportAttribute> const& source_attributes)
{
    // WithClause : with { WithEntries ,opt }
    // 1. Let attributes be WithClauseToAttributes of WithEntries.
    Vector<ImportAttribute> attributes;

    // AssertEntries : AssertionKey : StringLiteral
    // AssertEntries : AssertionKey : StringLiteral , WithEntries

    for (auto const& attribute : source_attributes) {
        // 1. Let key be the PropName of AttributeKey.
        // 2. Let entry be the ImportAttribute Record { [[Key]]: key, [[Value]]: SV of StringLiteral }.
        // 3. Return « entry ».
        attributes.empend(attribute);
    }

    // 2. Sort attributes according to the lexicographic order of their [[Key]] field, treating the value of each such
    //    field as a sequence of UTF-16 code unit values. NOTE: This sorting is observable only in that hosts are
    //    prohibited from changing behaviour based on the order in which attributes are enumerated.
    // NOTE: The sorting is done in construction of the ModuleRequest object.

    // 3. Return attributes.
    return attributes;
}

// 16.2.1.4 Static Semantics: ModuleRequests, https://tc39.es/ecma262/#sec-static-semantics-modulerequests
static Vector<ModuleRequest> module_requests(Program& program)
{
    // A List of all the ModuleSpecifier strings used by the module represented by this record to request the importation of a module.
    // NOTE: The List is source text occurrence ordered!
    struct RequestedModuleAndSourceIndex {
        u32 source_offset { 0 };
        ModuleRequest const* module_request { nullptr };
    };

    Vector<RequestedModuleAndSourceIndex> requested_modules_with_indices;

    for (auto const& import_statement : program.imports())
        requested_modules_with_indices.empend(import_statement->start_offset(), &import_statement->module_request());

    for (auto const& export_statement : program.exports()) {
        for (auto const& export_entry : export_statement->entries()) {
            if (!export_entry.is_module_request())
                continue;
            requested_modules_with_indices.empend(export_statement->start_offset(), &export_statement->module_request());
        }
    }

    // NOTE: The List is source code occurrence ordered. https://tc39.es/ecma262/#table-cyclic-module-fields
    quick_sort(requested_modules_with_indices, [&](RequestedModuleAndSourceIndex const& lhs, RequestedModuleAndSourceIndex const& rhs) {
        return lhs.source_offset < rhs.source_offset;
    });

    Vector<ModuleRequest> requested_modules_in_source_order;
    requested_modules_in_source_order.ensure_capacity(requested_modules_with_indices.size());

    for (auto const& module : requested_modules_with_indices) {
        if (module.module_request->attributes.is_empty()) {
            // ImportDeclaration : import ImportClause FromClause ;
            // ExportDeclaration : export ExportFromClause FromClause ;

            // 1. Let specifier be SV of FromClause.
            // 2. Return a List whose sole element is the ModuleRequest Record { [[Specifer]]: specifier, [[Attributes]]: « » }.
            requested_modules_in_source_order.empend(module.module_request->module_specifier);
        } else {
            // ImportDeclaration : import ImportClause FromClause WithClause ;
            // ExportDeclaration : export ExportFromClause FromClause WithClause ;

            // 1. Let specifier be the SV of FromClause.
            // 2. Let attributes be WithClauseToAttributes of WithClause.
            auto attributes = with_clause_to_assertions(module.module_request->attributes);

            // NOTE: We have to modify the attributes in place because else it might keep unsupported ones.
            const_cast<ModuleRequest*>(module.module_request)->attributes = move(attributes);

            // 3. Return a List whose sole element is the ModuleRequest Record { [[Specifier]]: specifier, [[Attributes]]: attributes }.
            requested_modules_in_source_order.empend(module.module_request->module_specifier, module.module_request->attributes);
        }
    }

    return requested_modules_in_source_order;
}

SourceTextModule::SourceTextModule(Realm& realm, StringView filename, Script::HostDefined* host_defined, bool has_top_level_await, NonnullRefPtr<Program> body, Vector<ModuleRequest> requested_modules,
    Vector<ImportEntry> import_entries, Vector<ExportEntry> local_export_entries,
    Vector<ExportEntry> indirect_export_entries, Vector<ExportEntry> star_export_entries,
    RefPtr<ExportStatement const> default_export)
    : CyclicModule(realm, filename, has_top_level_await, move(requested_modules), host_defined)
    , m_ecmascript_code(move(body))
    , m_execution_context(ExecutionContext::create(0, 0))
    , m_import_entries(move(import_entries))
    , m_local_export_entries(move(local_export_entries))
    , m_indirect_export_entries(move(indirect_export_entries))
    , m_star_export_entries(move(star_export_entries))
    , m_default_export(move(default_export))
{
}

SourceTextModule::~SourceTextModule() = default;

void SourceTextModule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_import_meta);
    m_execution_context->visit_edges(visitor);
}

// 16.2.1.7.1 ParseModule ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parsemodule
Result<GC::Ref<SourceTextModule>, Vector<ParserError>> SourceTextModule::parse(StringView source_text, Realm& realm, StringView filename, Script::HostDefined* host_defined)
{
    // 1. Let body be ParseText(sourceText, Module).
    auto parser = Parser(Lexer(source_text, filename), Program::Type::Module);
    auto body = parser.parse_program();

    // 2. If body is a List of errors, return body.
    if (parser.has_errors())
        return parser.errors();

    // 3. Let requestedModules be the ModuleRequests of body.
    auto requested_modules = module_requests(*body);

    // 4. Let importEntries be ImportEntries of body.
    Vector<ImportEntry> import_entries;
    for (auto const& import_statement : body->imports())
        import_entries.extend(import_statement->entries());

    // 5. Let importedBoundNames be ImportedLocalNames(importEntries).
    // NOTE: Since we have to potentially extract the import entry we just use importEntries
    //       In the future it might be an optimization to have a set/map of string to speed up the search.

    // 6. Let indirectExportEntries be a new empty List.
    Vector<ExportEntry> indirect_export_entries;

    // 7. Let localExportEntries be a new empty List.
    Vector<ExportEntry> local_export_entries;

    // 8. Let starExportEntries be a new empty List.
    Vector<ExportEntry> star_export_entries;

    // NOTE: Not in the spec but makes it easier to find the default.
    RefPtr<ExportStatement const> default_export;

    // 9. Let exportEntries be ExportEntries of body.
    // 10. For each ExportEntry Record ee of exportEntries, do
    for (auto const& export_statement : body->exports()) {
        if (export_statement->is_default_export()) {
            VERIFY(!default_export);
            VERIFY(export_statement->entries().size() == 1);
            VERIFY(export_statement->has_statement());

            auto const& entry = export_statement->entries()[0];
            VERIFY(entry.kind == ExportEntry::Kind::NamedExport);
            VERIFY(!entry.is_module_request());
            VERIFY(import_entries.find_if(
                                     [&](ImportEntry const& import_entry) {
                                         return import_entry.local_name == entry.local_or_import_name;
                                     })
                    .is_end());
            default_export = export_statement;
        }

        for (auto const& export_entry : export_statement->entries()) {
            // Special case, export {} from "module" should add "module" to
            // required_modules but not any import or export so skip here.
            if (export_entry.kind == ExportEntry::Kind::EmptyNamedExport) {
                VERIFY(export_statement->entries().size() == 1);
                break;
            }

            // a. If ee.[[ModuleRequest]] is null, then
            if (!export_entry.is_module_request()) {

                auto in_imported_bound_names = import_entries.find_if(
                    [&](ImportEntry const& import_entry) {
                        return import_entry.local_name == export_entry.local_or_import_name;
                    });

                // i. If ee.[[LocalName]] is not an element of importedBoundNames, then
                if (in_imported_bound_names.is_end()) {
                    // 1. Append ee to localExportEntries.
                    local_export_entries.empend(export_entry);
                }
                // ii. Else,
                else {
                    // 1. Let ie be the element of importEntries whose [[LocalName]] is the same as ee.[[LocalName]].
                    auto& import_entry = *in_imported_bound_names;

                    // 2. If ie.[[ImportName]] is NAMESPACE-OBJECT, then
                    if (import_entry.is_namespace()) {
                        // a. NOTE: This is a re-export of an imported module namespace object.
                        // b. Append ee to localExportEntries.
                        local_export_entries.empend(export_entry);
                    }
                    // 3. Else,
                    else {
                        // a. NOTE: This is a re-export of a single name.
                        // b. Append the ExportEntry Record { [[ModuleRequest]]: ie.[[ModuleRequest]], [[ImportName]]: ie.[[ImportName]], [[LocalName]]: null, [[ExportName]]: ee.[[ExportName]] } to indirectExportEntries.
                        indirect_export_entries.empend(ExportEntry::indirect_export_entry(import_entry.module_request(), export_entry.export_name, import_entry.import_name));
                    }
                }
            }
            // b. Else if ee.[[ImportName]] is all-but-default, then
            else if (export_entry.kind == ExportEntry::Kind::ModuleRequestAllButDefault) {
                // i. Assert: ee.[[ExportName]] is null.
                VERIFY(!export_entry.export_name.has_value());
                // ii. Append ee to starExportEntries.
                star_export_entries.empend(export_entry);
            }
            // c. Else,
            else {
                // i. Append ee to indirectExportEntries.
                indirect_export_entries.empend(export_entry);
            }
        }
    }

    // 11. Let async be body Contains await.
    bool async = body->has_top_level_await();

    // 12. Return Source Text Module Record {
    //          [[Realm]]: realm, [[Environment]]: empty, [[Namespace]]: empty, [[CycleRoot]]: empty, [[HasTLA]]: async,
    //          [[AsyncEvaluation]]: false, [[TopLevelCapability]]: empty, [[AsyncParentModules]]: « »,
    //          [[PendingAsyncDependencies]]: empty, [[Status]]: unlinked, [[EvaluationError]]: empty,
    //          [[HostDefined]]: hostDefined, [[ECMAScriptCode]]: body, [[Context]]: empty, [[ImportMeta]]: empty,
    //          [[RequestedModules]]: requestedModules, [[ImportEntries]]: importEntries, [[LocalExportEntries]]: localExportEntries,
    //          [[IndirectExportEntries]]: indirectExportEntries, [[StarExportEntries]]: starExportEntries, [[DFSIndex]]: empty, [[DFSAncestorIndex]]: empty }.
    return realm.heap().allocate<SourceTextModule>(
        realm,
        filename,
        host_defined,
        async,
        move(body),
        move(requested_modules),
        move(import_entries),
        move(local_export_entries),
        move(indirect_export_entries),
        move(star_export_entries),
        move(default_export));
}

// 16.2.1.7.2.1 GetExportedNames ( [ exportStarSet ] ), https://tc39.es/ecma262/#sec-getexportednames
Vector<Utf16FlyString> SourceTextModule::get_exported_names(VM& vm, HashTable<Module const*>& export_star_set)
{
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] get_export_names of {}", filename());

    // 1. Assert: module.[[Status]] is not NEW.
    VERIFY(m_status != ModuleStatus::New);

    // 2. If exportStarSet is not present, set exportStarSet to a new empty List.
    // NOTE: This is done by Module.

    // 3. If exportStarSet contains module, then
    if (export_star_set.contains(this)) {
        // a. Assert: We've reached the starting point of an export * circularity.
        // FIXME: How do we check that?

        // b. Return a new empty List.
        return {};
    }

    // 4. Append module to exportStarSet.
    export_star_set.set(this);

    // 5. Let exportedNames be a new empty List.
    Vector<Utf16FlyString> exported_names;

    // 6. For each ExportEntry Record e of module.[[LocalExportEntries]], do
    for (auto const& entry : m_local_export_entries) {
        // a. Assert: module provides the direct binding for this export.
        // FIXME: How do we check that?

        // b. Assert: e.[[ExportName]] is not null.
        VERIFY(entry.export_name.has_value());

        // c. Append e.[[ExportName]] to exportedNames.
        exported_names.empend(entry.export_name.value());
    }

    // 7. For each ExportEntry Record e of module.[[IndirectExportEntries]], do
    for (auto const& entry : m_indirect_export_entries) {
        // a. a. Assert: module imports a specific binding for this export.
        // FIXME: How do we check that?

        // b. Assert: e.[[ExportName]] is not null.
        VERIFY(entry.export_name.has_value());

        // c. Append e.[[ExportName]] to exportedNames.
        exported_names.empend(entry.export_name.value());
    }

    // 8. For each ExportEntry Record e of module.[[StarExportEntries]], do
    for (auto const& entry : m_star_export_entries) {
        // a. Assert: e.[[ModuleRequest]] is not null.
        // b. Let requestedModule be GetImportedModule(module, e.[[ModuleRequest]]).
        auto requested_module = get_imported_module(entry.module_request());

        // c. Let starNames be requestedModule.GetExportedNames(exportStarSet).
        auto star_names = requested_module->get_exported_names(vm, export_star_set);

        // d. For each element n of starNames, do
        for (auto const& name : star_names) {
            // i. If n is not "default", then
            if (name != "default"sv) {
                // 1. If exportedNames does not contain n, then
                if (!exported_names.contains_slow(name)) {
                    // a. Append n to exportedNames.
                    exported_names.empend(name);
                }
            }
        }
    }

    // 9. Return exportedNames.
    return exported_names;
}

// 16.2.1.7.3.1 InitializeEnvironment ( ), https://tc39.es/ecma262/#sec-source-text-module-record-initialize-environment
ThrowCompletionOr<void> SourceTextModule::initialize_environment(VM& vm)
{
    // 1. For each ExportEntry Record e of module.[[IndirectExportEntries]], do
    for (auto const& entry : m_indirect_export_entries) {
        // a. Assert: e.[[ExportName]] is not null.
        VERIFY(entry.export_name.has_value());

        // a. Let resolution be module.ResolveExport(e.[[ExportName]]).
        auto resolution = resolve_export(vm, entry.export_name.value());

        // b. If resolution is either null or AMBIGUOUS, throw a SyntaxError exception.
        if (!resolution.is_valid())
            return vm.throw_completion<SyntaxError>(ErrorType::InvalidOrAmbiguousExportEntry, entry.export_name);

        // c. Assert: resolution is a ResolvedBinding Record.
        VERIFY(resolution.is_valid());
    }

    // 2. Assert: All named exports from module are resolvable.
    // NOTE: We check all the indirect export entries above in step 1 and all the local named exports are resolvable by construction.

    // 3. Let realm be module.[[Realm]].
    // 4. Assert: realm is not undefined.
    auto& realm = this->realm();

    // 5. Let env be NewModuleEnvironment(realm.[[GlobalEnv]]).
    auto environment = vm.heap().allocate<ModuleEnvironment>(&realm.global_environment());

    // 6. Set module.[[Environment]] to env.
    set_environment(environment);

    // 7. For each ImportEntry Record in of module.[[ImportEntries]], do
    for (auto const& import_entry : m_import_entries) {
        // a. Let importedModule be GetImportedModule(module, in.[[ModuleRequest]]).
        auto imported_module = get_imported_module(import_entry.module_request());

        // b. If in.[[ImportName]] is NAMESPACE-OBJECT, then
        if (import_entry.is_namespace()) {
            // i. Let namespace be GetModuleNamespace(importedModule).
            auto namespace_ = imported_module->get_module_namespace(vm);

            // ii. Perform ! env.CreateImmutableBinding(in.[[LocalName]], true).
            MUST(environment->create_immutable_binding(vm, import_entry.local_name, true));

            // iii. Perform ! env.InitializeBinding(in.[[LocalName]], namespace, normal).
            MUST(environment->initialize_binding(vm, import_entry.local_name, namespace_, Environment::InitializeBindingHint::Normal));
        }
        // c. Else,
        else {
            auto const& import_name = import_entry.import_name.value();

            // i. Let resolution be importedModule.ResolveExport(in.[[ImportName]]).
            auto resolution = imported_module->resolve_export(vm, import_name);

            // ii. If resolution is either null or AMBIGUOUS, throw a SyntaxError exception.
            if (!resolution.is_valid())
                return vm.throw_completion<SyntaxError>(ErrorType::InvalidOrAmbiguousExportEntry, import_name);

            // iii. If resolution.[[BindingName]] is NAMESPACE, then
            if (resolution.is_namespace()) {
                // 1. Let namespace be GetModuleNamespace(resolution.[[Module]]).
                auto namespace_ = resolution.module->get_module_namespace(vm);

                // 2. Perform ! env.CreateImmutableBinding(in.[[LocalName]], true).
                MUST(environment->create_immutable_binding(vm, import_entry.local_name, true));

                // 3. Perform ! env.InitializeBinding(in.[[LocalName]], namespace, normal).
                MUST(environment->initialize_binding(vm, import_entry.local_name, namespace_, Environment::InitializeBindingHint::Normal));
            }
            // iv. Else,
            else {
                // 1. Perform env.CreateImportBinding(in.[[LocalName]], resolution.[[Module]], resolution.[[BindingName]]).
                MUST(environment->create_import_binding(import_entry.local_name, resolution.module, resolution.export_name));
            }
        }
    }

    // 8. Let moduleContext be a new ECMAScript code execution context.
    // NOTE: this has already been created during the construction of this object.

    // 9. Set the Function of moduleContext to null.

    // 10. Assert: module.[[Realm]] is not undefined.
    // NOTE: This must be true because we use a reference.

    // 11. Set the Realm of moduleContext to module.[[Realm]].
    m_execution_context->realm = &this->realm();

    // 12. Set the ScriptOrModule of moduleContext to module.
    m_execution_context->script_or_module = GC::Ref<Module>(*this);

    // 13. Set the VariableEnvironment of moduleContext to module.[[Environment]].
    m_execution_context->variable_environment = environment;

    // 14. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
    m_execution_context->lexical_environment = environment;

    // 15. Set the PrivateEnvironment of moduleContext to null.

    // 16. Set module.[[Context]] to moduleContext.
    // NOTE: We're already working on that one.

    // 17. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
    TRY(vm.push_execution_context(*m_execution_context, {}));

    // 18. Let code be module.[[ECMAScriptCode]].

    // 19. Let varDeclarations be the VarScopedDeclarations of code.
    // NOTE: We just loop through them in step 21.

    // 20. Let declaredVarNames be a new empty List.
    Vector<Utf16FlyString> declared_var_names;

    // 21. For each element d of varDeclarations, do
    // a. For each element dn of the BoundNames of d, do
    // NOTE: Due to the use of MUST with `create_mutable_binding` and `initialize_binding` below,
    //       an exception should not result from `for_each_var_declared_identifier`.
    MUST(m_ecmascript_code->for_each_var_declared_identifier([&](Identifier const& identifier) {
        auto const& name = identifier.string();

        // i. If dn is not an element of declaredVarNames, then
        if (!declared_var_names.contains_slow(name)) {
            // 1. Perform ! env.CreateMutableBinding(dn, false).
            MUST(environment->create_mutable_binding(vm, name, false));

            // 2. Perform ! env.InitializeBinding(dn, undefined, normal).
            MUST(environment->initialize_binding(vm, name, js_undefined(), Environment::InitializeBindingHint::Normal));

            // 3. Append dn to declaredVarNames.
            declared_var_names.empend(name);
        }
    }));

    // 22. Let lexDeclarations be the LexicallyScopedDeclarations of code.
    // NOTE: We only loop through them in step 24.

    // 23. Let privateEnv be null.
    PrivateEnvironment* private_environment = nullptr;

    // 24. For each element d of lexDeclarations, do
    // NOTE: Due to the use of MUST in the callback, an exception should not result from `for_each_lexically_scoped_declaration`.
    MUST(m_ecmascript_code->for_each_lexically_scoped_declaration([&](Declaration const& declaration) {
        // a. For each element dn of the BoundNames of d, do
        MUST(declaration.for_each_bound_identifier([&](Identifier const& identifier) {
            auto const& name = identifier.string();

            // i. If IsConstantDeclaration of d is true, then
            if (declaration.is_constant_declaration()) {
                // 1. Perform ! env.CreateImmutableBinding(dn, true).
                MUST(environment->create_immutable_binding(vm, name, true));
            }
            // ii. Else,
            else {
                // 1. Perform ! env.CreateMutableBinding(dn, false).
                MUST(environment->create_mutable_binding(vm, name, false));
            }

            // iii. If d is a FunctionDeclaration, a GeneratorDeclaration, an AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration, then
            if (declaration.is_function_declaration()) {
                VERIFY(is<FunctionDeclaration>(declaration));
                auto const& function_declaration = static_cast<FunctionDeclaration const&>(declaration);

                // 1. Let fo be InstantiateFunctionObject of d with arguments env and privateEnv.
                // NOTE: Special case if the function is a default export of an anonymous function
                //       it has name "*default*" but internally should have name "default".
                auto function_name = function_declaration.name();
                if (function_name == ExportStatement::local_name_for_default)
                    function_name = "default"_utf16_fly_string;
                auto function = ECMAScriptFunctionObject::create_from_function_node(
                    function_declaration,
                    move(function_name),
                    realm,
                    environment,
                    private_environment);

                // 2. Perform ! env.InitializeBinding(dn, fo, normal).
                MUST(environment->initialize_binding(vm, name, function, Environment::InitializeBindingHint::Normal));
            }
        }));
    }));

    // NOTE: The default export name is also part of the local lexical declarations but instead of making that a special
    //       case in the parser we just check it here. This is only needed for things which are not declarations. For more
    //       info check Parser::parse_export_statement. Furthermore, that declaration is not constant. so we take 24.a.ii.
    if (m_default_export) {
        VERIFY(m_default_export->has_statement());

        if (auto const& statement = m_default_export->statement(); !is<Declaration>(statement)) {
            auto const& name = m_default_export->entries()[0].local_or_import_name.value();
            dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] Adding default export to lexical declarations: local name: {}, Expression: {}", name, statement.class_name());

            // 1. Perform ! env.CreateMutableBinding(dn, false).
            MUST(environment->create_mutable_binding(vm, name, false));

            // NOTE: Since this is not a function declaration 24.a.iii never applies
        }
    }

    // 25. Remove moduleContext from the execution context stack.
    vm.pop_execution_context();

    // 26. Return unused.
    return {};
}

// 16.2.1.7.2.2 ResolveExport ( exportName [ , resolveSet ] ), https://tc39.es/ecma262/#sec-resolveexport
ResolvedBinding SourceTextModule::resolve_export(VM& vm, Utf16FlyString const& export_name, Vector<ResolvedBinding> resolve_set)
{
    // 1. Assert: module.[[Status]] is not NEW.
    VERIFY(m_status != ModuleStatus::New);

    // 2. If resolveSet is not present, set resolveSet to a new empty List.
    // NOTE: This is done by the default argument.

    // 3. For each Record { [[Module]], [[ExportName]] } r of resolveSet, do
    for (auto const& [type, module, exported_name] : resolve_set) {
        // a. If module and r.[[Module]] are the same Module Record and exportName is r.[[ExportName]], then
        if (module == this && exported_name == export_name) {
            // i. Assert: This is a circular import request.

            // ii. Return null.
            return ResolvedBinding::null();
        }
    }

    // 4. Append the Record { [[Module]]: module, [[ExportName]]: exportName } to resolveSet.
    resolve_set.append({ ResolvedBinding::Type::BindingName, this, export_name });

    // 5. For each ExportEntry Record e of module.[[LocalExportEntries]], do
    for (auto const& entry : m_local_export_entries) {
        // a. If e.[[ExportName]] is exportName, then
        if (export_name != entry.export_name)
            continue;

        // i. Assert: module provides the direct binding for this export.
        // FIXME: What does this mean?

        // ii. Return ResolvedBinding Record { [[Module]]: module, [[BindingName]]: e.[[LocalName]] }.
        return ResolvedBinding {
            ResolvedBinding::Type::BindingName,
            this,
            entry.local_or_import_name.value(),
        };
    }

    // 5. For each ExportEntry Record e of module.[[IndirectExportEntries]], do
    for (auto const& entry : m_indirect_export_entries) {
        // a. If e.[[ExportName]] is exportName, then
        if (export_name != entry.export_name)
            continue;

        // i. Assert: e.[[ModuleRequest]] is not null.
        // ii. Let importedModule be GetImportedModule(module, e.[[ModuleRequest]]).
        auto imported_module = get_imported_module(entry.module_request());

        // iii. If e.[[ImportName]] is all, then
        if (entry.kind == ExportEntry::Kind::ModuleRequestAll) {
            // 1. Assert: module does not provide the direct binding for this export.
            // FIXME: What does this mean? / How do we check this

            // 2. Return ResolvedBinding Record { [[Module]]: importedModule, [[BindingName]]: NAMESPACE }.
            return ResolvedBinding {
                ResolvedBinding::Type::Namespace,
                imported_module.ptr(),
                {}
            };
        }
        // iv. Else,
        else {
            // 1. Assert: module imports a specific binding for this export.
            // FIXME: What does this mean? / How do we check this

            // 2. Return importedModule.ResolveExport(e.[[ImportName]], resolveSet).
            return imported_module->resolve_export(vm, entry.local_or_import_name.value(), resolve_set);
        }
    }

    // 7. If exportName is "default", then
    if (export_name == "default"sv) {
        // a. Assert: A default export was not explicitly defined by this module.
        // FIXME: What does this mean? / How do we check this

        // b. Return null.
        return ResolvedBinding::null();

        // c. NOTE: A default export cannot be provided by an export * from "mod" declaration.
    }

    // 8. Let starResolution be null.
    auto star_resolution = ResolvedBinding::null();

    // 9. For each ExportEntry Record e of module.[[StarExportEntries]], do
    for (auto const& entry : m_star_export_entries) {
        // a. Assert: e.[[ModuleRequest]] is not null.
        // b. Let importedModule be GetImportedModule(module, e.[[ModuleRequest]]).
        auto imported_module = get_imported_module(entry.module_request());

        // c. Let resolution be importedModule.ResolveExport(exportName, resolveSet).
        auto resolution = imported_module->resolve_export(vm, export_name, resolve_set);

        // d. If resolution is AMBIGUOUS, return AMBIGUOUS.
        if (resolution.is_ambiguous())
            return ResolvedBinding::ambiguous();

        // e. If resolution is not null, then
        if (resolution.type == ResolvedBinding::Null)
            continue;

        // i. Assert: resolution is a ResolvedBinding Record.
        VERIFY(resolution.is_valid());

        // ii. If starResolution is null, set starResolution to resolution.
        if (star_resolution.type == ResolvedBinding::Null) {
            star_resolution = resolution;
        }
        // iii. Else,
        else {
            // 1. Assert: There is more than one * import that includes the requested name.
            // FIXME: Assert this

            // 2. If resolution.[[Module]] and starResolution.[[Module]] are not the same Module Record, return AMBIGUOUS.
            if (resolution.module != star_resolution.module)
                return ResolvedBinding::ambiguous();

            // 3. If resolution.[[BindingName]] is not starResolution.[[BindingName]] and either resolution.[[BindingName]]
            //    or starResolution.[[BindingName]] is NAMESPACE, return AMBIGUOUS.
            if (resolution.is_namespace() != star_resolution.is_namespace())
                return ResolvedBinding::ambiguous();

            // 4. If resolution.[[BindingName]] is a String, starResolution.[[BindingName]] is a String, and
            //    resolution.[[BindingName]] is not starResolution.[[BindingName]], return ambiguous.
            // NOTE: We know from the previous step that either both are namespaces or both are string, so we can check just one.
            if (!resolution.is_namespace() && resolution.export_name != star_resolution.export_name)
                return ResolvedBinding::ambiguous();
        }
    }

    // 10. Return starResolution.
    return star_resolution;
}

// 16.2.1.6.5 ExecuteModule ( [ capability ] ), https://tc39.es/ecma262/#sec-source-text-module-record-execute-module
// 9.1.1.1.2 ExecuteModule ( [ capability ] ), https://tc39.es/proposal-explicit-resource-management/#sec-source-text-module-record-execute-module
ThrowCompletionOr<void> SourceTextModule::execute_module(VM& vm, GC::Ptr<PromiseCapability> capability)
{
    dbgln_if(JS_MODULE_DEBUG, "[JS MODULE] SourceTextModule::execute_module({}, PromiseCapability @ {})", filename(), capability.ptr());

    GC::Ptr<Bytecode::Executable> executable;
    if (!m_has_top_level_await) {
        Completion result;

        auto maybe_executable = Bytecode::compile(vm, m_ecmascript_code, FunctionKind::Normal, "ShadowRealmEval"_utf16_fly_string);
        if (maybe_executable.is_error()) {
            result = maybe_executable.release_error();
        } else {
            executable = maybe_executable.release_value();
        }

        if (result.is_error())
            return result.release_error();
    }

    u32 registers_and_constants_and_locals_count = 0;
    if (executable) {
        registers_and_constants_and_locals_count = executable->number_of_registers + executable->constants.size() + executable->local_variable_names.size();
    }

    // 1. Let moduleContext be a new ECMAScript code execution context.
    ExecutionContext* module_context = nullptr;
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(module_context, registers_and_constants_and_locals_count, 0);

    // 2. Set the Function of moduleContext to null.

    // 3. Set the Realm of moduleContext to module.[[Realm]].
    module_context->realm = &realm();

    // 4. Set the ScriptOrModule of moduleContext to module.
    module_context->script_or_module = GC::Ref<Module>(*this);

    // 5. Assert: module has been linked and declarations in its module environment have been instantiated.
    VERIFY(m_status != ModuleStatus::New);
    VERIFY(m_status != ModuleStatus::Unlinked);
    VERIFY(m_status != ModuleStatus::Linking);
    VERIFY(environment());

    // 6. Set the VariableEnvironment of moduleContext to module.[[Environment]].
    module_context->variable_environment = environment();

    // 7. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
    module_context->lexical_environment = environment();

    // 8. Suspend the currently running execution context.
    // NOTE: Done by the push of execution context in steps below.

    // 9. If module.[[HasTLA]] is false, then
    if (!m_has_top_level_await) {
        // a. Assert: capability is not present.
        VERIFY(capability == nullptr);

        // b. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
        TRY(vm.push_execution_context(*module_context, {}));

        // c. Let result be the result of evaluating module.[[ECMAScriptCode]].
        Completion result;

        auto result_and_return_register = vm.bytecode_interpreter().run_executable(*module_context, *executable, {});
        if (result_and_return_register.value.is_error()) {
            result = result_and_return_register.value.release_error();
        } else {
            result = result_and_return_register.return_register_value.is_special_empty_value() ? js_undefined() : result_and_return_register.return_register_value;
        }

        // d. Let env be moduleContext's LexicalEnvironment.
        auto& env = as<DeclarativeEnvironment>(*module_context->lexical_environment);

        // e. Set result to Completion(DisposeResources(env.[[DisposeCapability]], result)).
        result = dispose_resources(vm, env.dispose_capability(), result);

        // f. Suspend moduleContext and remove it from the execution context stack.
        vm.pop_execution_context();

        // g. Resume the context that is now on the top of the execution context stack as the running execution context.
        // FIXME: We don't have resume yet.

        // h. If result is an abrupt completion, then
        if (result.is_error()) {
            // i. Return ? result.
            return result.release_error();
        }
    }
    // 10. Else,
    else {
        // a. Assert: capability is a PromiseCapability Record.
        VERIFY(capability != nullptr);

        // b. Perform AsyncBlockStart(capability, module.[[ECMAScriptCode]], moduleContext).

        // AD-HOC: We implement asynchronous execution via synthetic generator functions,
        //         so we fake "AsyncBlockStart" here by creating an async function to wrap
        //         the top-level module code.
        // FIXME: Improve this situation, so we can match the spec better.

        // NOTE: Like AsyncBlockStart, we need to push/pop the moduleContext around the function construction to ensure that
        //       the async execution context captures the module execution context.
        vm.push_execution_context(*module_context);

        FunctionParsingInsights parsing_insights;
        parsing_insights.uses_this_from_environment = true;
        parsing_insights.uses_this = true;
        auto module_wrapper_function = ECMAScriptFunctionObject::create(
            realm(), "module code with top-level await"_utf16_fly_string, StringView {}, this->m_ecmascript_code,
            FunctionParameters::empty(), 0, {}, environment(), nullptr, FunctionKind::Async, true, parsing_insights);
        module_wrapper_function->set_is_module_wrapper(true);

        vm.pop_execution_context();

        auto result = call(vm, Value { module_wrapper_function }, js_undefined(), ReadonlySpan<Value> {});

        // AD-HOC: This is basically analogous to what AsyncBlockStart would do.
        if (result.is_throw_completion()) {
            MUST(call(vm, *capability->reject(), js_undefined(), result.throw_completion().value()));
        } else {
            MUST(call(vm, *capability->resolve(), js_undefined(), result.value()));
        }
    }

    // 11. Return unused.
    return {};
}

}
