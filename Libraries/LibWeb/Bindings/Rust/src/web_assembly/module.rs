/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Module, https://webassembly.github.io/spec/js-api/#modules

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, ArrayBufferShared, Gc, MarkedVec, Realm, Trace, Value};
use libwasm_engine::module::{ExternalKind, ImportDescription};

use crate::bindings::conversions::{BufferSource, UsvString};
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::web_assembly::builtins::{import_refers_to_a_builtin, validate_builtins_and_imported_strings};
use crate::web_assembly::errors::{WebAssemblyError, throw_webassembly_error};
use crate::web_assembly::web_assembly::WebAssemblyCompileOptions;
use crate::web_assembly::{CompiledModule, compile_a_webassembly_module, implementation_of_value, stable_bytes_of};

unsafe extern "C" {
    /// The current Realm's corresponding agent cluster, as the ID it has if it has one.
    fn web_bindings_current_agent_cluster(agent_cluster: *mut u64) -> bool;

    /// The realm's corresponding agent cluster, as the ID it has if it has one.
    fn web_bindings_agent_cluster_of_realm(realm: *mut c_void, agent_cluster: *mut u64) -> bool;
}

fn current_agent_cluster() -> Option<u64> {
    let mut agent_cluster = 0;
    // SAFETY: The agent cluster is written if there is one.
    unsafe { web_bindings_current_agent_cluster(&raw mut agent_cluster) }.then_some(agent_cluster)
}

fn agent_cluster_of_realm(realm: Gc<Realm>) -> Option<u64> {
    let mut agent_cluster = 0;
    // SAFETY: The realm is live, and the agent cluster is written if there is one.
    unsafe { web_bindings_agent_cluster_of_realm(realm.as_ptr().cast(), &raw mut agent_cluster) }
        .then_some(agent_cluster)
}

/// https://webassembly.github.io/spec/js-api/#enumdef-importexportkind
#[derive(Clone, Copy)]
pub(crate) enum ImportExportKind {
    Function,
    Table,
    Memory,
    Global,
}

/// https://webassembly.github.io/spec/js-api/#dictdef-moduleexportdescriptor
pub(crate) struct ModuleExportDescriptor {
    pub(crate) name: UsvString,
    pub(crate) kind: ImportExportKind,
}

/// https://webassembly.github.io/spec/js-api/#dictdef-moduleimportdescriptor
pub(crate) struct ModuleImportDescriptor {
    pub(crate) module: UsvString,
    pub(crate) name: UsvString,
    pub(crate) kind: ImportExportKind,
}

fn import_export_kind(kind: ExternalKind) -> Option<ImportExportKind> {
    match kind {
        ExternalKind::Function => Some(ImportExportKind::Function),
        ExternalKind::Table => Some(ImportExportKind::Table),
        ExternalKind::Memory => Some(ImportExportKind::Memory),
        ExternalKind::Global => Some(ImportExportKind::Global),
        ExternalKind::Tag => None,
    }
}

fn kind_of_import(description: &ImportDescription) -> ExternalKind {
    match description {
        ImportDescription::Function(_) => ExternalKind::Function,
        ImportDescription::Table(_) => ExternalKind::Table,
        ImportDescription::Memory(_) => ExternalKind::Memory,
        ImportDescription::Global(_) => ExternalKind::Global,
        ImportDescription::Tag(_) => ExternalKind::Tag,
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Module {
    // [[Module]], which keeps the bytes it was compiled from: [[Bytes]].
    #[gc(untraced)]
    module: CompiledModule,
    // [[BuiltinSets]]
    #[gc(untraced)]
    builtin_sets: Vec<UsvString>,
    // [[ImportedStringModule]]
    #[gc(untraced)]
    imported_string_module: Option<UsvString>,
}

impl Wrappable for Module {}

impl Module {
    /// https://webassembly.github.io/spec/js-api/#construct-a-webassembly-module-object
    pub(crate) fn new(
        module: CompiledModule,
        builtin_sets: Vec<UsvString>,
        imported_string_module: Option<UsvString>,
    ) -> Self {
        // 1. Let moduleObject be a new Module.
        // 2. Set moduleObject.[[Module]] to module.
        // 3. Set moduleObject.[[Bytes]] to bytes.
        // NB: The compiled module keeps the bytes it was compiled from.
        // 4. Set moduleObject.[[BuiltinSets]] to builtinSetNames.
        // 5. Set moduleObject.[[ImportedStringModule]] to importedStringModule.
        // 6. Return moduleObject.
        Self {
            module,
            builtin_sets,
            imported_string_module,
        }
    }

    // https://webassembly.github.io/spec/js-api/#dom-module-module
    pub(crate) fn construct(
        cx: &mut CallContext,
        bytes: BufferSource,
        options: WebAssemblyCompileOptions,
    ) -> WebIdlResult<Self> {
        // 1. Let stableBytes be a copy of the bytes held by the buffer bytes.
        let stable_bytes = stable_bytes_of(bytes);

        // 2. Compile the WebAssembly module stableBytes and store the result as module.
        // 3. If module is error, throw a CompileError exception.
        let module = compile_a_webassembly_module(cx.vm(), cx.realm(), &stable_bytes)?;

        // 4. Let builtinSetNames be options["builtins"].
        let builtin_set_names = options.builtins;

        // 5. Let importedStringModule be options["importedStringConstants"].
        let imported_string_module = options.imported_string_constants;

        // 6. If validating builtins and imported strings for module with builtinSetNames and importedStringModule returns
        //    false, throw a CompileError exception.
        if !validate_builtins_and_imported_strings(
            cx.vm(),
            &module,
            &builtin_set_names,
            imported_string_module.as_ref(),
        ) {
            return Err(throw_webassembly_error(
                cx.vm(),
                cx.realm(),
                WebAssemblyError::Compile,
                "Invalid builtin or imported string",
            )
            .into());
        }

        // 7. Set this.[[Module]] to module.
        // 8. Set this.[[Bytes]] to stableBytes.
        // 9. Set this.[[BuiltinSets]] to builtinSetNames.
        // 10. Set this.[[ImportedStringModule]] to importedStringModule.
        Ok(Self::new(module, builtin_set_names, imported_string_module))
    }

    /// [[Bytes]]: the bytes the module was compiled from.
    fn bytes(&self) -> &[u8] {
        &self.module.bytes
    }

    pub(crate) fn from_value(value: Value) -> Option<Ref<Self>> {
        implementation_of_value(value)
    }

    pub(crate) fn compiled_module(&self) -> &CompiledModule {
        &self.module
    }

    pub(crate) fn builtin_sets(&self) -> &[UsvString] {
        &self.builtin_sets
    }

    pub(crate) fn imported_string_module(&self) -> Option<&UsvString> {
        self.imported_string_module.as_ref()
    }

    // https://webassembly.github.io/threads/js-api/index.html#dom-module-exports
    pub(crate) fn exports(_: &mut CallContext, module_object: Ref<Self>) -> WebIdlResult<Vec<ModuleExportDescriptor>> {
        // 1. Let module be moduleObject.[[Module]].
        // 2. Let exports be « ».
        // 3. For each (name, type) of module_exports(module),
        let exports = module_object
            .module
            .exports
            .iter()
            .filter_map(|export| {
                // 3.1. Let kind be the string value of the extern type type.
                // FIXME: Exporting tags.
                let kind = import_export_kind(export.kind)?;

                // 3.2. Let obj be «[ "name" → name, "kind" → kind ]».
                // 3.3. Append obj to exports.
                Some(ModuleExportDescriptor {
                    name: UsvString::from_utf8(&export.name),
                    kind,
                })
            })
            .collect();

        // 4. Return exports.
        Ok(exports)
    }

    // https://webassembly.github.io/spec/js-api/#dom-module-imports
    pub(crate) fn imports(_: &mut CallContext, module_object: Ref<Self>) -> WebIdlResult<Vec<ModuleImportDescriptor>> {
        // 1. Let module be moduleObject.[[Module]].
        let module = &module_object.module;

        // 2. Let builtinSetNames be moduleObject.[[BuiltinSets]].
        let builtin_set_names = &module_object.builtin_sets;

        // 3. Let importedStringModule be moduleObject.[[ImportedStringModule]].
        let imported_string_module = module_object.imported_string_module.as_ref().map(UsvString::to_utf8);

        // 4. Let imports be « ».
        // 5. For each (moduleName, name, type) of module_imports(module),
        let imports = module
            .imports
            .iter()
            .filter_map(|import| {
                // 1. If find a builtin for (moduleName, name, type) and builtinSetNames is not null, then continue.
                if import_refers_to_a_builtin(import, builtin_set_names) {
                    return None;
                }

                // 2. If importedStringModule is not null and moduleName equals importedStringModule, then continue.
                if imported_string_module.as_ref() == Some(&import.module) {
                    return None;
                }

                // 3. Let kind be the string value of the extern type type.
                // FIXME: Importing tags.
                let kind = import_export_kind(kind_of_import(&import.description))?;

                // 4. Let obj be «[ "module" → moduleName, "name" → name, "kind" → kind ]».
                // 5. Append obj to imports.
                Some(ModuleImportDescriptor {
                    module: UsvString::from_utf8(&import.module),
                    name: UsvString::from_utf8(&import.name),
                    kind,
                })
            })
            .collect();

        // 6. Return imports.
        Ok(imports)
    }

    // https://webassembly.github.io/threads/js-api/index.html#dom-module-customsections
    pub(crate) fn custom_sections<'vm>(
        cx: &mut CallContext<'vm>,
        module_object: Ref<Self>,
        section_name: Utf16String,
    ) -> WebIdlResult<MarkedVec<'vm, Gc<ArrayBuffer>>> {
        // 1. Let bytes be moduleObject.[[Bytes]].
        // 2. Let customSections be « ».
        let module = &module_object.module;
        let custom_sections = MarkedVec::new(cx.vm());

        // 3. For each custom section customSection of bytes, interpreted according to the module grammar,
        for section in &module.custom_sections {
            if Utf16String::from_utf8(&section.name) != section_name {
                continue;
            }
            // 3.3.1. Append a new ArrayBuffer containing a copy of the bytes in bytes for the range matched by this customsec production to customSections.
            let contents = &module.bytes[section.contents.start as usize..section.contents.end as usize];
            custom_sections.push(ArrayBuffer::create_from_bytes(
                cx.vm(),
                cx.realm(),
                contents,
                ArrayBufferShared::No,
            ));
        }

        // 4. Return customSections.
        Ok(custom_sections)
    }
}

impl Serializable for Module {
    // https://webassembly.github.io/spec/web-api/#ref-for-serialization-steps
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. If forStorage is true, throw a "DataCloneError" DOMException.
        if for_storage {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_utf8("WebAssembly Module instances may not be serialized for storage"),
            ));
        }

        // 2. Set serialized.[[Bytes]] to the sub-serialization of value.[[Bytes]].
        serialized.write_bytes(self.bytes());

        // 3. Set serialized.[[AgentCluster]] to the current Realm’s corresponding agent cluster.
        serialized.write_optional_u64(current_agent_cluster());

        Ok(())
    }

    // https://webassembly.github.io/spec/web-api/#ref-for-deserialization-steps
    fn deserialization_steps(cx: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // 1. Let bytes be the sub-deserialization of serialized.[[Bytes]].
        // 2. Set value.[[Bytes]] to bytes.
        let bytes = serialized.read_bytes()?;

        // 3. If targetRealm’s corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.
        let agent_cluster = serialized.read_optional_u64()?;
        if agent_cluster.is_none() || agent_cluster != agent_cluster_of_realm(cx.realm()) {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_utf8("Cannot deserialize WebAssembly Module outside its agent cluster"),
            ));
        }

        // 4. Compile a WebAssembly module from bytes and set value.[[Module]] to the result.
        let module = compile_a_webassembly_module(cx.vm(), cx.realm(), &bytes)?;
        Ok(Self::new(module, Vec::new(), None))
    }
}
