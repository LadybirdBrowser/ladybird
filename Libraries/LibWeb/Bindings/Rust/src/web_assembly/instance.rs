/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Instance, https://webassembly.github.io/spec/js-api/#instances

use core::cell::RefCell;

use ak::Utf16FlyString;
use libjs_rust::api::{
    Attribute, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, Gc, IntegrityLevel, MarkedVec, Must,
    Object, PropertyAttributes, PropertyKey, Realm, ThrowCompletionOr, Trace, Value, Visitor, Vm,
};
use libwasm_engine::module::ImportDescription;
use libwasm_engine::runtime::{ExternValue, Instance as CoreInstance, InstanceAddress};
use libwasm_engine::types::{FuncType, GlobalType, HeapType, RefType, ValType};

use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, Wrappable, wrap};
use crate::bindings::realm_data::wrapper_world_of;
use crate::bindings::{CallContext, WebIdlResult};
use crate::web_assembly::builtins::{instantiate_a_builtin_set, instantiate_imported_strings, refers_to_a_builtin_set};
use crate::web_assembly::errors::{WebAssemblyError, throw_webassembly_error};
use crate::web_assembly::exported_function::{
    create_exported_function, create_host_function, exported_function_address,
};
use crate::web_assembly::global::Global;
use crate::web_assembly::memory::Memory;
use crate::web_assembly::module::Module;
use crate::web_assembly::table::Table;
use crate::web_assembly::tag::Tag;
use crate::web_assembly::values::to_webassembly_value;
use crate::web_assembly::{Agent, CompiledModule, EntityRef, throw_of_instantiation_error};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Instance {
    // [[Instance]]
    instance: EntityRef<CoreInstance>,
    // [[Exports]]
    exports: Gc<Object>,
}

impl Wrappable for Instance {}

/// The imports of a module, which keep the entities they are alive while they wait for an instance to hold them: the
/// host functions and globals made for the imports are referenced from nowhere else, reading the imports runs user
/// code (getters, valueOf) that may collect garbage, and an asynchronous instantiation holds them until its task runs.
#[derive(Default)]
pub(crate) struct Imports(pub(crate) RefCell<Vec<ExternValue>>);

static IMPORTS_CLASS: EmbedderClass<Imports> = EmbedderClass::new("WebAssemblyImports");

impl EmbedderValue for Imports {
    const CLASS: &'static EmbedderClass<Self> = &IMPORTS_CLASS;
}

// SAFETY: Visits the cell of every entity of the list.
unsafe impl Trace for Imports {
    fn trace(&self, visitor: &mut Visitor) {
        for &import in self.0.borrow().iter() {
            match import {
                ExternValue::Function(address) => EntityRef(address).trace(visitor),
                ExternValue::Table(address) => EntityRef(address).trace(visitor),
                ExternValue::Memory(address) => EntityRef(address).trace(visitor),
                ExternValue::Global(address) => EntityRef(address).trace(visitor),
                ExternValue::Tag(address) => EntityRef(address).trace(visitor),
            }
        }
    }
}

impl Imports {
    pub(crate) fn push(&self, import: ExternValue) {
        self.0.borrow_mut().push(import);
    }
}

/// A value type of the module, in the store's terms (see Store::module_type_identities()).
pub(crate) fn value_type_in_store(vm: &Vm, module: &CompiledModule, ty: ValType) -> ValType {
    let ValType::Ref(RefType {
        nullable,
        heap: HeapType::Concrete(index),
    }) = ty
    else {
        return ty;
    };
    let identities = Agent::the(vm).store().module_type_identities_of(module);
    ValType::Ref(RefType {
        nullable,
        heap: HeapType::Concrete(identities[index as usize]),
    })
}

/// The function type at `type_index` of the module, in the store's terms.
pub(crate) fn function_type_in_store(vm: &Vm, module: &CompiledModule, type_index: u32) -> FuncType {
    let ty = module.types[type_index as usize]
        .as_func()
        .expect("the type of a function import is a function type");
    FuncType {
        params: ty
            .params
            .iter()
            .map(|&ty| value_type_in_store(vm, module, ty))
            .collect(),
        results: ty
            .results
            .iter()
            .map(|&ty| value_type_in_store(vm, module, ty))
            .collect(),
    }
}

/// The global type of the module, in the store's terms.
pub(crate) fn global_type_in_store(vm: &Vm, module: &CompiledModule, ty: GlobalType) -> GlobalType {
    GlobalType {
        value: value_type_in_store(vm, module, ty.value),
        mutable: ty.mutable,
    }
}

fn link_error<T>(vm: &Vm, message: &str) -> ThrowCompletionOr<T> {
    let realm = vm.current_realm().expect("reading the imports runs in a realm");
    Err(throw_webassembly_error(vm, realm, WebAssemblyError::Link, message))
}

// https://webassembly.github.io/spec/js-api/#read-the-imports
pub(crate) fn read_the_imports(
    vm: &Vm,
    module: &CompiledModule,
    import_object: Option<Gc<Object>>,
    builtin_set_names: &[UsvString],
    imported_string_module: Option<&UsvString>,
) -> ThrowCompletionOr<Gc<EmbedderCell<Imports>>> {
    // 1. If module_imports(module) is not empty, and importObject is missing, throw a TypeError exception.
    if !module.imports.is_empty() && import_object.is_none() {
        return vm.throw_completion_with_message(
            ErrorKind::TypeError,
            "ImportObject must be provided when module has imports".to_string(),
        );
    }

    // 2. Let builtinOrStringImports be the ordered map «[ ]».
    // NB: The keys are UTF-8, as the module names of imports are.
    let builtin_or_string_imports = MarkedVec::<(String, Gc<Object>)>::new(vm);
    let contains_key = |key: &str| {
        builtin_or_string_imports
            .values()
            .iter()
            .any(|(builtin_or_string_import, _)| builtin_or_string_import == key)
    };

    // 3. For each builtinSetName of builtinSetNames,
    for builtin_set_name in builtin_set_names {
        // 3.1. Let builtinSetQualifiedName be builtinSetName prefixed with "wasm:"
        let builtin_set_qualified_name = format!("wasm:{}", builtin_set_name.to_utf8());

        // 3.2. Assert: builtinOrStringImports does not contain builtinSetQualifiedName
        assert!(!contains_key(&builtin_set_qualified_name));

        // 3.3. If builtinSetName does not refer to a builtin set, then continue.
        if !refers_to_a_builtin_set(builtin_set_name) {
            continue;
        }

        // 3.4. Let exportsObject be the result of instantiate a builtin set with builtinSetName
        let exports_object = instantiate_a_builtin_set(vm, builtin_set_name);

        // 3.5. Set builtinOrStringImports[builtinSetQualifiedName] to exportsObject
        builtin_or_string_imports.push((builtin_set_qualified_name, exports_object));
    }

    // 4. If importedStringModule is not null,
    if let Some(imported_string_module) = imported_string_module {
        // 4.1. Let exportsObject be the result of instantiate imported strings with module and importedStringModule
        let exports_object = instantiate_imported_strings(vm, module, imported_string_module);

        // 4.2. Set builtinOrStringImports[importedStringModule] to exportsObject
        let imported_string_module = imported_string_module.to_utf8();
        let existing = builtin_or_string_imports
            .values()
            .iter()
            .position(|(builtin_or_string_import, _)| *builtin_or_string_import == imported_string_module);
        match existing {
            Some(index) => builtin_or_string_imports.set(index, (imported_string_module, exports_object)),
            None => builtin_or_string_imports.push((imported_string_module, exports_object)),
        }
    }

    // 5. Let imports be « ».
    let imports = IMPORTS_CLASS.allocate(vm, |_| Imports::default());
    // NB: A module without an import object has no imports (step 1).
    let Some(import_object) = import_object else {
        return Ok(imports);
    };
    let mut imported_function_count = 0;

    // 6. For each (moduleName, componentName, externtype) of module_imports(module),
    for import in &module.imports {
        let module_name = PropertyKey::from(Utf16FlyString::from_utf8(&import.module));
        let component_name = PropertyKey::from(Utf16FlyString::from_utf8(&import.name));

        let builtin_or_string_import = builtin_or_string_imports
            .values()
            .iter()
            .find(|(builtin_or_string_import, _)| *builtin_or_string_import == import.module)
            .map(|&(_, exports_object)| exports_object);
        let value = match builtin_or_string_import {
            // 6.1. If builtinOrStringImports[moduleName] exists,
            Some(builtin_or_string_import) => {
                // 6.1.1. Let o be builtinOrStringImports[moduleName].
                let mut value = Value::from_object(builtin_or_string_import);

                // 6.1.2. If o is not an Object or if ? HasProperty(o, componentName) is false,
                if !value.is_object() || !value.as_object().has_property(vm, &component_name)? {
                    // 6.1.2.1. Set o to ? Get(importObject, moduleName).
                    value = import_object.get(vm, &module_name)?;
                }
                value
            }
            // 6.2. Else,
            None => {
                // 6.2.1. Let o be ? Get(importObject, moduleName).
                import_object.get(vm, &module_name)?
            }
        };

        // 6.3. If o is not an Object, throw a TypeError exception.
        if !value.is_object() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IsNotAEvaluatedFrom,
                &[
                    &value.to_utf16_string_without_side_effects(),
                    &"Object",
                    &format!("[wasm import object][\"{}\"]", import.module),
                ],
            );
        }

        // 6.4. Let v be ? Get(o, componentName).
        let v = value.as_object().get(vm, &component_name)?;

        match &import.description {
            // 6.5. If externtype is of the form func functype,
            ImportDescription::Function(type_index) => {
                // 6.5.1. If IsCallable(v) is false, throw a LinkError exception.
                if !v.is_function() {
                    let message = ErrorType::IsNotAEvaluatedFrom.message(&[
                        &v.to_utf16_string_without_side_effects(),
                        &"function",
                        &format!("[wasm import object][\"{}\"]", import.name),
                    ]);
                    return link_error(vm, &String::from_utf16_lossy(&message.to_utf16()));
                }

                // 6.5.2. If v has a [[FunctionAddress]] internal slot, and therefore is an Exported Function,
                //     6.5.2.1. Let funcaddr be the value of v's [[FunctionAddress]] internal slot.
                // 6.5.3. Otherwise,
                //     6.5.3.1. Create a host function from v and functype, and let funcaddr be the result.
                //     6.5.3.2. Let index be the number of external functions in imports. This value index is known as the index of the host function funcaddr.
                let function = exported_function_address(v.as_object()).unwrap_or_else(|| {
                    create_host_function(
                        vm,
                        v.as_function(),
                        function_type_in_store(vm, module, *type_index),
                        imported_function_count,
                    )
                });

                // 6.5.4. Let externfunc be the external value func funcaddr.
                // 6.5.5. Append externfunc to imports.
                imports.push(ExternValue::Function(function));
                imported_function_count += 1;
            }
            // 6.6. If externtype is of the form global mut valtype,
            ImportDescription::Global(ty) => {
                let ty = global_type_in_store(vm, module, *ty);
                // 6.6.1. If v implements Global,
                //     6.6.1.1. Let globaladdr be v.[[Global]].
                let global = if let Some(global) = Global::from_value(v) {
                    global.address()
                }
                // 6.6.2. Otherwise,
                else {
                    // 6.6.2.1. If valtype is i64 and v is not a BigInt, throw a LinkError exception.
                    if ty.value == ValType::I64 && !v.is_bigint() {
                        return link_error(vm, "Import resolution attempted to use a non-BigInt for an i64 global");
                    }
                    // 6.6.2.2. If valtype is one of i32, f32 or f64 and v is not a Number, throw a LinkError exception.
                    if matches!(ty.value, ValType::I32 | ValType::F32 | ValType::F64) && !v.is_number() {
                        return link_error(
                            vm,
                            "Import resolution attempted to use a non-Number for a numeric global",
                        );
                    }
                    // 6.6.2.3. If valtype is v128, throw a LinkError exception.
                    if ty.value == ValType::V128 {
                        return link_error(vm, "Import resolution attempted to use a value for a V128 global");
                    }
                    // 6.6.2.4. If mut is var, throw a LinkError exception.
                    if ty.mutable {
                        return link_error(vm, "Import resolution attempted to use a value for a mutable global");
                    }
                    // 6.6.2.5. Let value be ToWebAssemblyValue(v, valtype). If this throws a TypeError, catch it, and throw
                    //          a LinkError exception.
                    let Ok(value) = to_webassembly_value(vm, v, ty.value) else {
                        return link_error(
                            vm,
                            "Import resolution attempted to use a value of the wrong type for a global",
                        );
                    };
                    // 6.6.2.6. Let store be the surrounding agent's associated store.
                    // 6.6.2.7. Let (store, globaladdr) be global_alloc(store, const valtype, value).
                    // 6.6.2.8. Set the surrounding agent's associated store to store.
                    Agent::the(vm).store().allocate_global(
                        GlobalType {
                            value: ty.value,
                            mutable: false,
                        },
                        value,
                    )
                };

                // 6.6.3. Let externglobal be global globaladdr.
                // 6.6.4. Append externglobal to imports.
                imports.push(ExternValue::Global(global));
            }
            // 6.7. If externtype is of the form mem memtype,
            ImportDescription::Memory(_) => {
                // 6.7.1. If v does not implement Memory, throw a LinkError exception.
                let Some(memory) = Memory::from_value(v) else {
                    return link_error(vm, "Expected an instance of WebAssembly.Memory for a memory import");
                };

                // 6.7.2. Let externmem be the external value mem v.[[Memory]].
                // 6.7.3. Append externmem to imports.
                imports.push(ExternValue::Memory(memory.address()));
            }
            // 6.8. If externtype is of the form table tabletype,
            ImportDescription::Table(_) => {
                // 6.8.1. If v does not implement Table, throw a LinkError exception.
                let Some(table) = Table::from_value(v) else {
                    return link_error(vm, "Expected an instance of WebAssembly.Table for a table import");
                };

                // 6.8.2. Let tableaddr be v.[[Table]].
                // 6.8.3. Let externtable be the external value table tableaddr.
                // 6.8.4. Append externtable to imports.
                imports.push(ExternValue::Table(table.address()));
            }
            // 6.9. If externtype is of the form tag attribute functype,
            ImportDescription::Tag(_) => {
                // 6.9.1. Assert: attribute is exception.
                // 6.9.2. If v does not implement Tag, throw a LinkError exception.
                let Some(tag) = Tag::from_value(v) else {
                    return link_error(vm, "Expected an instance of WebAssembly.Tag for a tag import");
                };

                // 6.9.3. Let tagaddr be v.[[Address]].
                // 6.9.4. Let externtag be the external value tag tagaddr.
                // 6.9.5. Append externtag to imports.
                imports.push(ExternValue::Tag(tag.address()));
            }
        }
    }

    // 7. Return imports.
    Ok(imports)
}

// https://webassembly.github.io/spec/js-api/index.html#instantiate-the-core-of-a-webassembly-module
pub(crate) fn instantiate_the_core_of_a_webassembly_module(
    vm: &Vm,
    module: &CompiledModule,
    imports: &Imports,
) -> ThrowCompletionOr<InstanceAddress> {
    // 1. Let store be the surrounding agent's associated store.
    // 2. Let result be module_instantiate(store, module, imports).
    // 3. If result is error, throw an appropriate exception type:
    //     * A LinkError exception for most cases which occur during linking.
    //     * If the error came when running the start function, throw a RuntimeError for most errors which occur from
    //       WebAssembly, or the error object propagated from inner ECMAScript code.
    //     * Another error type if appropriate, for example an out-of-memory exception, as documented in the
    //       WebAssembly error mapping.
    // 4. Let (store, instance) be result.
    // 5. Set the surrounding agent's associated store to store.
    // 6. Return instance.
    let imports = imports.0.borrow().clone();
    Agent::the(vm)
        .store()
        .instantiate(module.clone(), &imports)
        .map_err(|error| throw_of_instantiation_error(vm, error))
}

// https://webassembly.github.io/spec/js-api/#create-an-exports-object
fn create_an_exports_object(vm: &Vm, realm: Gc<Realm>, instance: InstanceAddress) -> Gc<Object> {
    let cx = CallContext::new(vm, realm);
    let exports_object = Object::create(vm, realm, None);
    let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::ENUMERABLE | Attribute::CONFIGURABLE);

    // NB: Creating the object of an export may reach the store, so nothing of it stays borrowed across the loop.
    let module = Agent::the(vm).store().instance(instance).module.clone();
    for export in &module.exports {
        let Some(value) = Agent::the(vm).store().instance(instance).export(&export.name) else {
            continue;
        };
        let value = match value {
            ExternValue::Function(function) => {
                Value::from_object(create_exported_function(vm, realm, function).upcast::<Object>())
            }
            ExternValue::Global(global) => Value::from_object(wrap(&cx, Global::from_address(vm, global))),
            ExternValue::Memory(memory) => Value::from_object(wrap(&cx, Memory::from_address(vm, memory))),
            ExternValue::Table(table) => Value::from_object(wrap(&cx, Table::from_address(vm, table))),
            ExternValue::Tag(tag) => Value::from_object(wrap(&cx, Tag::from_address(vm, realm, tag))),
        };
        exports_object.define_direct_property(
            vm,
            &PropertyKey::from(Utf16FlyString::from_utf8(&export.name)),
            value,
            attributes,
        );
    }

    exports_object.set_integrity_level(vm, IntegrityLevel::Frozen).must();
    exports_object
}

impl Instance {
    // https://webassembly.github.io/spec/js-api/#dom-instance-instance
    pub(crate) fn construct(
        cx: &mut CallContext,
        module_object: Ref<Module>,
        import_object: Option<Gc<Object>>,
    ) -> WebIdlResult<Self> {
        let vm = cx.vm();

        // 1. Let builtinSetNames be moduleObject.[[BuiltinSets]].
        let builtin_set_names = module_object.builtin_sets();

        // 2. Let importedStringModule be moduleObject.[[ImportedStringModule]].
        let imported_string_module = module_object.imported_string_module();

        // 3. Let module be moduleObject.[[Module]].
        let module = module_object.compiled_module().clone();

        // 4. Read the imports of module with imports importObject, builtinSetNames, and importedStringModule, and let
        //    imports be the result.
        let imports = read_the_imports(vm, &module, import_object, builtin_set_names, imported_string_module)?;

        // 5. Instantiate the core of a WebAssembly module module with imports, and let instance be the result.
        let instance = instantiate_the_core_of_a_webassembly_module(vm, &module, &imports)?;

        // 6. Initialize this from module and instance.
        Ok(Self::initialize(vm, cx.realm(), instance))
    }

    // https://webassembly.github.io/spec/js-api/#initialize-an-instance-object
    pub(crate) fn initialize(vm: &Vm, realm: Gc<Realm>, instance: InstanceAddress) -> Self {
        // NB: An instance's exports, like its memories and tables, belong to the realm it was created in.
        Self {
            instance: EntityRef(instance),
            exports: create_an_exports_object(vm, realm, instance),
        }
    }

    pub(crate) fn address(&self) -> InstanceAddress {
        self.instance.0
    }

    // https://webassembly.github.io/spec/js-api/#dom-instance-exports
    pub(crate) fn exports(&self, cx: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        let vm = cx.vm();
        assert!(
            wrapper_world_of(vm, cx.realm()) == wrapper_world_of(vm, self.exports.shape().realm()),
            "Reading a WebAssembly instance's exports from another world"
        );
        Ok(self.exports)
    }
}
