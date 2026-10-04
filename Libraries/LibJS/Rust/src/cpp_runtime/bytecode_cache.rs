/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Materialization of decoded bytecode cache blobs into C++ executables.
//!
//! The format and its validation live in [`crate::bytecode_cache`]. This half
//! turns a validated blob into the C++ runtime's `Executable`s and
//! `SharedFunctionInstanceData`s, or installs it into a program that already
//! has them.

use std::ffi::c_void;

use super::entry_points::ModuleCallbacks;
use super::entry_points::ModuleExportEntryCallback;
use super::ffi::FFISharedFunctionData;
use super::ffi::FFIUtf16Slice;
use crate::ast;
use crate::bytecode::generator::FunctionSfdMetadata;
use crate::bytecode::generator::PendingClassBlueprint;
use crate::bytecode_cache::CachedBytecodeValidation;
use crate::bytecode_cache::DecodedCacheBlob;
use crate::bytecode_cache::DecodedCachedExecutableRecord;
use crate::bytecode_cache::DecodedDeclarationMetadata;
use crate::bytecode_cache::DecodedExecutableRecord;
use crate::bytecode_cache::DecodedFunctionRecord;
use crate::bytecode_cache::DecodedUtf16String;
use crate::bytecode_cache::ModuleDeclarationMetadata;
use crate::bytecode_cache::ModuleExportEntryRecord;
use crate::bytecode_cache::ProgramKind;
use crate::bytecode_cache::ScriptDeclarationMetadata;

struct PreparedUtf16Slice {
    _storage: Option<Vec<u16>>,
    slice: FFIUtf16Slice,
}

impl PreparedUtf16Slice {
    fn new(value: &DecodedUtf16String) -> Self {
        match value {
            DecodedUtf16String::Owned(value) => Self {
                _storage: None,
                slice: FFIUtf16Slice::from(value.as_ref()),
            },
            DecodedUtf16String::Foreign { data, length } => {
                #[cfg(target_endian = "little")]
                {
                    Self {
                        _storage: None,
                        slice: FFIUtf16Slice {
                            data: *data,
                            length: *length,
                        },
                    }
                }
                #[cfg(not(target_endian = "little"))]
                {
                    let storage = value.to_vec();
                    let slice = FFIUtf16Slice::from(storage.as_slice());
                    Self {
                        _storage: Some(storage),
                        slice,
                    }
                }
            }
        }
    }

    fn as_ptr_len(&self) -> (*const u16, usize) {
        (self.slice.data, self.slice.length)
    }
}

fn native_string_table<'a>(strings: impl Iterator<Item = &'a DecodedUtf16String>) -> Vec<ak::Utf16FlyString> {
    strings
        .map(|string| {
            let prepared = PreparedUtf16Slice::new(string);
            // SAFETY: The prepared slice borrows the decoded string or its own converted storage.
            ak::Utf16FlyString::from_utf16(unsafe {
                std::slice::from_raw_parts(prepared.slice.data, prepared.slice.length)
            })
        })
        .collect()
}

impl DecodedCacheBlob {
    pub(crate) unsafe fn materialize_script(
        &self,
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        shared_function_data_list_ptr: *mut c_void,
        gdi_context: *mut c_void,
    ) -> *mut c_void {
        unsafe {
            self.verify_has_been_validated_for_materialization();
            if self.program_type != ast::ProgramType::Script {
                return std::ptr::null_mut();
            }
            let DecodedDeclarationMetadata::Script {
                metadata,
                declaration_functions,
            } = &self.metadata
            else {
                return std::ptr::null_mut();
            };
            let ProgramKind::ScriptOrModule = self.program.kind else {
                return std::ptr::null_mut();
            };
            if declaration_functions.len() != metadata.function_names.len() {
                return std::ptr::null_mut();
            }

            let shared_function_data_owner = super::ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr);
            if !materialize_script_declaration_metadata(
                metadata,
                declaration_functions,
                self.is_strict_mode,
                vm_ptr,
                source_code_ptr,
                shared_function_data_owner,
                gdi_context,
            ) {
                return std::ptr::null_mut();
            }
            materialize_executable(
                &self.program.executable,
                vm_ptr,
                source_code_ptr,
                shared_function_data_owner,
                CachedBytecodeValidation::Validated,
            )
        }
    }

    pub(crate) unsafe fn materialize_module(
        &self,
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        shared_function_data_list_ptr: *mut c_void,
        module_context: *mut c_void,
        callbacks: *const ModuleCallbacks,
        tla_executable_out: *mut *mut c_void,
    ) -> *mut c_void {
        unsafe {
            self.verify_has_been_validated_for_materialization();
            if callbacks.is_null() {
                return std::ptr::null_mut();
            }
            let cb = &*callbacks;
            if self.program_type != ast::ProgramType::Module {
                return std::ptr::null_mut();
            }
            let DecodedDeclarationMetadata::Module {
                metadata,
                declaration_functions,
            } = &self.metadata
            else {
                return std::ptr::null_mut();
            };
            if declaration_functions.len() != metadata.function_names.len() {
                return std::ptr::null_mut();
            }

            let shared_function_data_owner = super::ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr);
            (cb.set_has_top_level_await)(module_context, self.has_top_level_await);
            if !materialize_module_declaration_metadata(
                metadata,
                declaration_functions,
                vm_ptr,
                source_code_ptr,
                shared_function_data_owner,
                module_context,
                cb,
            ) {
                return std::ptr::null_mut();
            }

            match self.program.kind {
                ProgramKind::AsyncModule => {
                    let exec_ptr = materialize_executable(
                        &self.program.executable,
                        vm_ptr,
                        source_code_ptr,
                        shared_function_data_owner,
                        CachedBytecodeValidation::Validated,
                    );
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = exec_ptr;
                    }
                    std::ptr::null_mut()
                }
                ProgramKind::ScriptOrModule => {
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = std::ptr::null_mut();
                    }
                    materialize_executable(
                        &self.program.executable,
                        vm_ptr,
                        source_code_ptr,
                        shared_function_data_owner,
                        CachedBytecodeValidation::Validated,
                    )
                }
            }
        }
    }

    pub(crate) unsafe fn install_script(
        &self,
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        existing_executable_ptr: *const c_void,
        existing_shared_function_data_ptrs: &[*mut c_void],
    ) -> *mut c_void {
        unsafe {
            self.verify_has_been_validated_for_materialization();
            if existing_executable_ptr.is_null() {
                return std::ptr::null_mut();
            }

            if self.program_type != ast::ProgramType::Script {
                return std::ptr::null_mut();
            }
            let DecodedDeclarationMetadata::Script {
                metadata,
                declaration_functions,
            } = &self.metadata
            else {
                return std::ptr::null_mut();
            };
            let ProgramKind::ScriptOrModule = self.program.kind else {
                return std::ptr::null_mut();
            };

            let mut existing_shared_function_data = ExistingSharedFunctionData::new(existing_shared_function_data_ptrs);
            let mut pending_function_installs = Vec::new();
            if !prepare_declaration_function_installs(
                declaration_functions,
                metadata.function_names.len(),
                &mut existing_shared_function_data,
                self.is_strict_mode,
                vm_ptr,
                source_code_ptr,
                &mut pending_function_installs,
            ) {
                return std::ptr::null_mut();
            }
            let executable_ptr = materialize_executable_for_install(
                &self.program.executable,
                Some(&mut existing_shared_function_data),
                super::ffi::SharedFunctionDataOwner::None,
                vm_ptr,
                source_code_ptr,
                &mut pending_function_installs,
                CachedBytecodeValidation::Validated,
            );
            if executable_ptr.is_null() {
                return std::ptr::null_mut();
            }
            if !existing_shared_function_data.all_matched() {
                return std::ptr::null_mut();
            }
            for install in pending_function_installs {
                install.commit();
            }
            executable_ptr
        }
    }

    pub(crate) unsafe fn install_module(
        &self,
        vm_ptr: *mut c_void,
        source_code_ptr: *const c_void,
        existing_executable_ptr: *const c_void,
        existing_shared_function_data_ptrs: &[*mut c_void],
        existing_tla_sfd_ptr: *mut c_void,
        tla_executable_out: *mut *mut c_void,
    ) -> *mut c_void {
        unsafe {
            self.verify_has_been_validated_for_materialization();
            if self.program_type != ast::ProgramType::Module {
                return std::ptr::null_mut();
            }
            let DecodedDeclarationMetadata::Module {
                metadata,
                declaration_functions,
            } = &self.metadata
            else {
                return std::ptr::null_mut();
            };

            let mut existing_shared_function_data = ExistingSharedFunctionData::new(existing_shared_function_data_ptrs);
            let mut pending_function_installs = Vec::new();
            if !prepare_declaration_function_installs(
                declaration_functions,
                metadata.function_names.len(),
                &mut existing_shared_function_data,
                true,
                vm_ptr,
                source_code_ptr,
                &mut pending_function_installs,
            ) {
                return std::ptr::null_mut();
            }
            match self.program.kind {
                ProgramKind::AsyncModule => {
                    if !self.has_top_level_await || existing_tla_sfd_ptr.is_null() {
                        return std::ptr::null_mut();
                    }
                    let exec_ptr = materialize_executable_for_install(
                        &self.program.executable,
                        Some(&mut existing_shared_function_data),
                        super::ffi::SharedFunctionDataOwner::None,
                        vm_ptr,
                        source_code_ptr,
                        &mut pending_function_installs,
                        CachedBytecodeValidation::Validated,
                    );
                    if exec_ptr.is_null() {
                        return std::ptr::null_mut();
                    }
                    if !existing_shared_function_data.all_matched() {
                        return std::ptr::null_mut();
                    }
                    for install in pending_function_installs {
                        install.commit();
                    }
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = exec_ptr;
                    }
                    std::ptr::null_mut()
                }
                ProgramKind::ScriptOrModule => {
                    if self.has_top_level_await || existing_executable_ptr.is_null() {
                        return std::ptr::null_mut();
                    }
                    if !tla_executable_out.is_null() {
                        *tla_executable_out = std::ptr::null_mut();
                    }
                    let executable_ptr = materialize_executable_for_install(
                        &self.program.executable,
                        Some(&mut existing_shared_function_data),
                        super::ffi::SharedFunctionDataOwner::None,
                        vm_ptr,
                        source_code_ptr,
                        &mut pending_function_installs,
                        CachedBytecodeValidation::Validated,
                    );
                    if executable_ptr.is_null() {
                        return std::ptr::null_mut();
                    }
                    if !existing_shared_function_data.all_matched() {
                        return std::ptr::null_mut();
                    }
                    for install in pending_function_installs {
                        install.commit();
                    }
                    executable_ptr
                }
            }
        }
    }
}

unsafe fn prepare_declaration_function_installs(
    declaration_functions: &[DecodedFunctionRecord],
    expected_function_count: usize,
    existing_shared_function_data: &mut ExistingSharedFunctionData<'_>,
    outer_strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    pending_function_installs: &mut Vec<PendingFunctionInstall>,
) -> bool {
    unsafe {
        if declaration_functions.len() != expected_function_count {
            return false;
        }

        for function in declaration_functions {
            if prepare_function_install(
                function,
                outer_strict,
                existing_shared_function_data,
                vm_ptr,
                source_code_ptr,
                pending_function_installs,
                CachedBytecodeValidation::Validated,
            )
            .is_null()
            {
                return false;
            }
        }

        true
    }
}

unsafe fn materialize_script_declaration_metadata(
    metadata: &ScriptDeclarationMetadata,
    declaration_functions: &[DecodedFunctionRecord],
    is_strict_mode: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_owner: super::ffi::SharedFunctionDataOwner,
    gdi_context: *mut c_void,
) -> bool {
    unsafe {
        use super::ffi::script_gdi_push_annex_b_name;
        use super::ffi::script_gdi_push_function;
        use super::ffi::script_gdi_push_lexical_binding;
        use super::ffi::script_gdi_push_lexical_name;
        use super::ffi::script_gdi_push_var_name;
        use super::ffi::script_gdi_push_var_scoped_name;

        for name in &metadata.lexical_names {
            script_gdi_push_lexical_name(gdi_context, name.as_ptr(), name.len());
        }
        for name in &metadata.var_names {
            script_gdi_push_var_name(gdi_context, name.as_ptr(), name.len());
        }
        for (function, name) in declaration_functions.iter().zip(metadata.function_names.iter()) {
            let sfd_ptr = materialize_function(
                function,
                is_strict_mode,
                vm_ptr,
                source_code_ptr,
                shared_function_data_owner,
                CachedBytecodeValidation::Validated,
            );
            if sfd_ptr.is_null() {
                return false;
            }
            script_gdi_push_function(gdi_context, sfd_ptr, name.as_ptr(), name.len());
        }
        for name in &metadata.var_scoped_names {
            script_gdi_push_var_scoped_name(gdi_context, name.as_ptr(), name.len());
        }
        for name in &metadata.annex_b_candidate_names {
            script_gdi_push_annex_b_name(gdi_context, name.as_ptr(), name.len());
        }
        for binding in &metadata.lexical_bindings {
            script_gdi_push_lexical_binding(
                gdi_context,
                binding.name.as_ptr(),
                binding.name.len(),
                binding.is_constant,
            );
        }

        true
    }
}

unsafe fn materialize_module_declaration_metadata(
    metadata: &ModuleDeclarationMetadata,
    declaration_functions: &[DecodedFunctionRecord],
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_owner: super::ffi::SharedFunctionDataOwner,
    module_context: *mut c_void,
    cb: &ModuleCallbacks,
) -> bool {
    unsafe {
        for entry in &metadata.import_entries {
            let (import_name, import_name_len, is_namespace) = entry
                .import_name
                .as_ref()
                .map(|name| (name.as_ptr(), name.len(), false))
                .unwrap_or((std::ptr::null(), 0, true));
            let attributes = import_attributes_to_ffi(&entry.module_request.attributes);
            (cb.push_import_entry)(
                module_context,
                import_name,
                import_name_len,
                is_namespace,
                entry.local_name.as_ptr(),
                entry.local_name.len(),
                entry.module_request.specifier.as_ptr(),
                entry.module_request.specifier.len(),
                attributes.keys.as_ptr(),
                attributes.values.as_ptr(),
                attributes.keys.len(),
            );
        }

        for entry in &metadata.local_exports {
            push_module_export_entry(module_context, cb.push_local_export, entry);
        }
        for entry in &metadata.indirect_exports {
            push_module_export_entry(module_context, cb.push_indirect_export, entry);
        }
        for entry in &metadata.star_exports {
            push_module_export_entry(module_context, cb.push_star_export, entry);
        }
        for request in &metadata.requested_modules {
            let attributes = import_attributes_to_ffi(&request.attributes);
            (cb.push_requested_module)(
                module_context,
                request.specifier.as_ptr(),
                request.specifier.len(),
                attributes.keys.as_ptr(),
                attributes.values.as_ptr(),
                attributes.keys.len(),
            );
        }
        if let Some(name) = &metadata.default_export_binding_name {
            (cb.set_default_export_binding)(module_context, name.as_ptr(), name.len());
        }
        for name in &metadata.var_declared_names {
            (cb.push_var_name)(module_context, name.as_ptr(), name.len());
        }
        for (function, name) in declaration_functions.iter().zip(metadata.function_names.iter()) {
            let sfd_ptr = materialize_function(
                function,
                true,
                vm_ptr,
                source_code_ptr,
                shared_function_data_owner,
                CachedBytecodeValidation::Validated,
            );
            if sfd_ptr.is_null() {
                return false;
            }
            (cb.push_function)(module_context, sfd_ptr, name.as_ptr(), name.len());
        }
        for binding in &metadata.lexical_bindings {
            (cb.push_lexical_binding)(
                module_context,
                binding.name.as_ptr(),
                binding.name.len(),
                binding.is_constant,
                binding.function_index,
            );
        }

        true
    }
}

struct ImportAttributesFfi {
    keys: Vec<FFIUtf16Slice>,
    values: Vec<FFIUtf16Slice>,
}

fn import_attributes_to_ffi(attributes: &[ast::ImportAttribute]) -> ImportAttributesFfi {
    ImportAttributesFfi {
        keys: attributes
            .iter()
            .map(|attribute| FFIUtf16Slice::from(attribute.key.as_ref()))
            .collect(),
        values: attributes
            .iter()
            .map(|attribute| FFIUtf16Slice::from(attribute.value.as_ref()))
            .collect(),
    }
}

unsafe fn push_module_export_entry(
    module_context: *mut c_void,
    callback: ModuleExportEntryCallback,
    entry: &ModuleExportEntryRecord,
) {
    unsafe {
        let (export_name, export_name_len) = entry
            .export_name
            .as_ref()
            .map(|name| (name.as_ptr(), name.len()))
            .unwrap_or((std::ptr::null(), 0));
        let (local_or_import_name, local_or_import_name_len) = entry
            .local_or_import_name
            .as_ref()
            .map(|name| (name.as_ptr(), name.len()))
            .unwrap_or((std::ptr::null(), 0));
        let (module_specifier, module_specifier_len, attributes) = entry
            .module_request
            .as_ref()
            .map(|request| {
                (
                    request.specifier.as_ptr(),
                    request.specifier.len(),
                    import_attributes_to_ffi(&request.attributes),
                )
            })
            .unwrap_or((
                std::ptr::null(),
                0,
                ImportAttributesFfi {
                    keys: Vec::new(),
                    values: Vec::new(),
                },
            ));

        callback(
            module_context,
            entry.kind as u8,
            export_name,
            export_name_len,
            local_or_import_name,
            local_or_import_name_len,
            module_specifier,
            module_specifier_len,
            attributes.keys.as_ptr(),
            attributes.values.as_ptr(),
            attributes.keys.len(),
        );
    }
}

enum PendingFunctionInstallReplacement {
    CachedBytecode(DecodedCachedExecutableRecord),
    Executable(*mut c_void),
}

struct ExistingSharedFunctionData<'a> {
    ptrs: &'a [*mut c_void],
    matched: Vec<bool>,
}

impl<'a> ExistingSharedFunctionData<'a> {
    fn new(ptrs: &'a [*mut c_void]) -> Self {
        Self {
            ptrs,
            matched: vec![false; ptrs.len()],
        }
    }

    unsafe fn take_matching(&mut self, data: &FFISharedFunctionData) -> *mut c_void {
        unsafe {
            for (index, ptr) in self.ptrs.iter().copied().enumerate() {
                if self.matched[index] || ptr.is_null() {
                    continue;
                }
                if super::ffi::rust_sfd_matches_bytecode_cache_function(ptr, data) {
                    self.matched[index] = true;
                    return ptr;
                }
            }
            std::ptr::null_mut()
        }
    }

    fn all_matched(&self) -> bool {
        self.matched.iter().all(|matched| *matched)
    }
}

struct PendingFunctionInstall {
    existing_sfd_ptr: *mut c_void,
    replacement: PendingFunctionInstallReplacement,
    metadata: FunctionSfdMetadata,
}

impl PendingFunctionInstall {
    unsafe fn commit(self) {
        unsafe {
            match self.replacement {
                PendingFunctionInstallReplacement::CachedBytecode(cached_executable) => {
                    cached_executable.verify_has_been_validated_for_materialization();
                    let cached_executable_ptr = Box::into_raw(Box::new(cached_executable)) as *mut c_void;
                    super::ffi::rust_sfd_install_cached_bytecode_executable(
                        self.existing_sfd_ptr,
                        cached_executable_ptr,
                        self.metadata.uses_this,
                        self.metadata.this_value_needs_environment_resolution,
                        self.metadata.function_environment_needed,
                        self.metadata.function_environment_bindings_count,
                        self.metadata.var_environment_bindings_count,
                        self.metadata.might_need_arguments,
                        self.metadata.contains_eval,
                    );
                }
                PendingFunctionInstallReplacement::Executable(executable_ptr) => {
                    super::ffi::rust_sfd_install_bytecode_cache_executable(
                        self.existing_sfd_ptr,
                        executable_ptr,
                        self.metadata.uses_this,
                        self.metadata.this_value_needs_environment_resolution,
                        self.metadata.function_environment_needed,
                        self.metadata.function_environment_bindings_count,
                        self.metadata.var_environment_bindings_count,
                        self.metadata.might_need_arguments,
                        self.metadata.contains_eval,
                    );
                }
            }
        }
    }
}

unsafe fn materialize_function(
    function: &DecodedFunctionRecord,
    outer_strict: bool,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_owner: super::ffi::SharedFunctionDataOwner,
    validation: CachedBytecodeValidation,
) -> *mut c_void {
    unsafe {
        let parameter_names = function
            .parameter_names
            .as_ref()
            .map(|names| native_string_table(names.iter()))
            .unwrap_or_default();
        let name_storage = function.name.as_ref().map(PreparedUtf16Slice::new);
        let (name, name_len) = name_storage
            .as_ref()
            .map(PreparedUtf16Slice::as_ptr_len)
            .unwrap_or((std::ptr::null(), 0));
        let source_text_offset = function.source_text_start as usize;
        let source_text_length = function
            .source_text_end
            .checked_sub(function.source_text_start)
            .map(|length| length as usize)
            .unwrap_or(0);

        let data = FFISharedFunctionData {
            name,
            name_len,
            function_kind: function.kind as u8,
            function_length: function.function_length,
            formal_parameter_count: function.formal_parameter_count,
            strict: function.is_strict_mode || outer_strict,
            is_arrow: function.is_arrow_function,
            has_simple_parameter_list: function.parameter_names.is_some(),
            parameter_names: parameter_names.as_ptr().cast(),
            parameter_name_count: parameter_names.len(),
            source_text_offset,
            source_text_length,
            rust_function_ast: std::ptr::null_mut(),
            uses_this: function.uses_this,
            uses_this_from_environment: function.uses_this_from_environment,
        };

        let sfd_ptr = match shared_function_data_owner {
            super::ffi::SharedFunctionDataOwner::None => {
                super::ffi::rust_create_sfd(vm_ptr, source_code_ptr, &raw const data)
            }
            super::ffi::SharedFunctionDataOwner::List(list_ptr) => {
                assert!(!list_ptr.is_null(), "SharedFunctionDataOwner::List must not be null");
                super::ffi::rust_create_sfd_in_list(vm_ptr, source_code_ptr, list_ptr, &raw const data)
            }
        };
        if sfd_ptr.is_null() {
            return std::ptr::null_mut();
        }

        if let Some((name, is_private)) = &function.class_field_initializer_name {
            let name_storage = PreparedUtf16Slice::new(name);
            let (name, name_len) = name_storage.as_ptr_len();
            super::ffi::rust_sfd_set_class_field_initializer_name(sfd_ptr, name, name_len, *is_private);
        }

        let cached_executable_ptr =
            Box::into_raw(Box::new(function.precompiled.validated_copy(validation))) as *mut c_void;
        super::ffi::rust_sfd_set_cached_bytecode_executable(
            sfd_ptr,
            cached_executable_ptr,
            function.metadata.uses_this,
            function.metadata.this_value_needs_environment_resolution,
            function.metadata.function_environment_needed,
            function.metadata.function_environment_bindings_count,
            function.metadata.var_environment_bindings_count,
            function.metadata.might_need_arguments,
            function.metadata.contains_eval,
        );

        sfd_ptr
    }
}

unsafe fn prepare_function_install(
    function: &DecodedFunctionRecord,
    outer_strict: bool,
    existing_shared_function_data: &mut ExistingSharedFunctionData<'_>,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    pending_function_installs: &mut Vec<PendingFunctionInstall>,
    validation: CachedBytecodeValidation,
) -> *mut c_void {
    unsafe {
        let parameter_names = function
            .parameter_names
            .as_ref()
            .map(|names| native_string_table(names.iter()))
            .unwrap_or_default();
        let name_storage = function.name.as_ref().map(PreparedUtf16Slice::new);
        let (name, name_len) = name_storage
            .as_ref()
            .map(PreparedUtf16Slice::as_ptr_len)
            .unwrap_or((std::ptr::null(), 0));
        let source_text_offset = function.source_text_start as usize;
        let source_text_length = function
            .source_text_end
            .checked_sub(function.source_text_start)
            .map(|length| length as usize)
            .unwrap_or(0);

        let data = FFISharedFunctionData {
            name,
            name_len,
            function_kind: function.kind as u8,
            function_length: function.function_length,
            formal_parameter_count: function.formal_parameter_count,
            strict: function.is_strict_mode || outer_strict,
            is_arrow: function.is_arrow_function,
            has_simple_parameter_list: function.parameter_names.is_some(),
            parameter_names: parameter_names.as_ptr().cast(),
            parameter_name_count: parameter_names.len(),
            source_text_offset,
            source_text_length,
            rust_function_ast: std::ptr::null_mut(),
            uses_this: function.uses_this,
            uses_this_from_environment: function.uses_this_from_environment,
        };

        let existing_sfd_ptr = existing_shared_function_data.take_matching(&data);
        if existing_sfd_ptr.is_null() {
            return std::ptr::null_mut();
        }

        if super::ffi::rust_sfd_executable(existing_sfd_ptr).is_null() {
            pending_function_installs.push(PendingFunctionInstall {
                existing_sfd_ptr,
                replacement: PendingFunctionInstallReplacement::CachedBytecode(
                    function.precompiled.validated_copy(validation),
                ),
                metadata: function.metadata.clone(),
            });
            return existing_sfd_ptr;
        }

        let Some(executable) = function.precompiled.decode_validated_executable(validation) else {
            return std::ptr::null_mut();
        };
        let executable_ptr = materialize_executable_for_install(
            &executable,
            Some(existing_shared_function_data),
            super::ffi::SharedFunctionDataOwner::None,
            vm_ptr,
            source_code_ptr,
            pending_function_installs,
            validation,
        );
        if executable_ptr.is_null() {
            return std::ptr::null_mut();
        }

        pending_function_installs.push(PendingFunctionInstall {
            existing_sfd_ptr,
            replacement: PendingFunctionInstallReplacement::Executable(executable_ptr),
            metadata: function.metadata.clone(),
        });

        existing_sfd_ptr
    }
}

pub(crate) unsafe fn materialize_cached_function(
    cached_executable_ptr: *mut c_void,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_list_ptr: *mut c_void,
) -> *mut c_void {
    unsafe {
        if cached_executable_ptr.is_null() {
            return std::ptr::null_mut();
        }
        let cached_executable = Box::from_raw(cached_executable_ptr as *mut DecodedCachedExecutableRecord);
        let Some(executable) = cached_executable.decode_executable() else {
            return std::ptr::null_mut();
        };
        let shared_function_data_owner = if shared_function_data_list_ptr.is_null() {
            super::ffi::SharedFunctionDataOwner::None
        } else {
            super::ffi::SharedFunctionDataOwner::List(shared_function_data_list_ptr)
        };
        materialize_executable(
            &executable,
            vm_ptr,
            source_code_ptr,
            shared_function_data_owner,
            CachedBytecodeValidation::Validated,
        )
    }
}

pub(crate) unsafe fn free_cached_function(cached_executable_ptr: *mut c_void) {
    unsafe {
        if !cached_executable_ptr.is_null() {
            drop(Box::from_raw(
                cached_executable_ptr as *mut DecodedCachedExecutableRecord,
            ));
        }
    }
}

unsafe fn materialize_executable(
    executable: &DecodedExecutableRecord,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    shared_function_data_owner: super::ffi::SharedFunctionDataOwner,
    validation: CachedBytecodeValidation,
) -> *mut c_void {
    unsafe {
        let mut pending_function_installs = Vec::new();
        materialize_executable_for_install(
            executable,
            None,
            shared_function_data_owner,
            vm_ptr,
            source_code_ptr,
            &mut pending_function_installs,
            validation,
        )
    }
}

unsafe fn materialize_executable_for_install(
    executable: &DecodedExecutableRecord,
    mut existing_shared_function_data: Option<&mut ExistingSharedFunctionData<'_>>,
    shared_function_data_owner: super::ffi::SharedFunctionDataOwner,
    vm_ptr: *mut c_void,
    source_code_ptr: *const c_void,
    pending_function_installs: &mut Vec<PendingFunctionInstall>,
    validation: CachedBytecodeValidation,
) -> *mut c_void {
    unsafe {
        let Some(identifier_table) = executable.identifier_table.values() else {
            return std::ptr::null_mut();
        };
        let Some(property_key_table) = executable.property_key_table.values() else {
            return std::ptr::null_mut();
        };
        let native_identifiers = native_string_table(identifier_table.iter());
        let native_property_keys = native_string_table(property_key_table.iter());
        let Some(string_table) = executable.string_table.values() else {
            return std::ptr::null_mut();
        };
        let native_strings = native_string_table(string_table.iter());
        let Some((constants_count, constants_bytes)) = executable.constants.encoded_constants() else {
            return std::ptr::null_mut();
        };
        let Some(local_variables) = executable.local_variables.values() else {
            return std::ptr::null_mut();
        };
        let local_variable_names = native_string_table(local_variables.iter().map(|local_variable| {
            let _ = local_variable.is_lexically_declared;
            let _ = local_variable.is_initialized_during_declaration_instantiation;
            &local_variable.name
        }));
        let local_variable_metadata: Vec<super::ffi::FFILocalVariableMetadata> = local_variables
            .iter()
            .zip(&local_variable_names)
            .map(|(variable, name)| super::ffi::FFILocalVariableMetadata {
                name: name.raw_identity(),
                is_mutable: variable.is_mutable,
                has_scope_range: variable.scope_range.is_some(),
                scope_start_line: variable.scope_range.map_or(0, |range| range.start.line),
                scope_start_column: variable.scope_range.map_or(0, |range| range.start.column),
                scope_end_line: variable.scope_range.map_or(0, |range| range.end.line),
                scope_end_column: variable.scope_range.map_or(0, |range| range.end.column),
            })
            .collect();
        let Some(argument_variable_names) = executable.argument_variable_names.values() else {
            return std::ptr::null_mut();
        };
        let argument_variable_names = native_string_table(argument_variable_names.iter());

        let Some(shared_functions) = executable.shared_functions.values() else {
            return std::ptr::null_mut();
        };
        let mut sfd_ptrs = Vec::with_capacity(shared_functions.len());
        for function in &shared_functions {
            let sfd_ptr = if let Some(registry) = existing_shared_function_data.as_deref_mut() {
                prepare_function_install(
                    function,
                    executable.strict,
                    registry,
                    vm_ptr,
                    source_code_ptr,
                    pending_function_installs,
                    validation,
                ) as *const c_void
            } else {
                materialize_function(
                    function,
                    executable.strict,
                    vm_ptr,
                    source_code_ptr,
                    shared_function_data_owner,
                    validation,
                ) as *const c_void
            };
            sfd_ptrs.push(sfd_ptr);
        }
        if sfd_ptrs.iter().any(|ptr| ptr.is_null()) {
            return std::ptr::null_mut();
        }

        let Some(class_blueprints) = executable.class_blueprints.values() else {
            return std::ptr::null_mut();
        };
        let class_blueprints: Vec<PendingClassBlueprint> =
            class_blueprints.iter().map(PendingClassBlueprint::from).collect();
        let bp_ptrs: Vec<*mut c_void> = class_blueprints
            .iter()
            .map(|blueprint| super::ffi::materialize_class_blueprint(blueprint, vm_ptr, source_code_ptr))
            .collect();
        if bp_ptrs.iter().any(|ptr| ptr.is_null()) {
            return std::ptr::null_mut();
        }
        let Some(exception_handlers) = executable.exception_handlers.values() else {
            return std::ptr::null_mut();
        };
        let Some(source_map) = executable.source_map.values() else {
            return std::ptr::null_mut();
        };

        super::ffi::create_executable_from_slices(
            super::ffi::ExecutableParts {
                bytecode: executable.bytecode.as_slice(),
                // The C++ executable adopts this as its bytecode_owner, so the callback
                // returns the exact owner type rust_create_executable() expects. The
                // original decoded blob keeps its own owner until materialization finishes.
                bytecode_owner: executable.bytecode.clone_blob_owner(),
                exception_handlers: &exception_handlers,
                source_map: &source_map,
                basic_block_start_offsets: &[],
                number_of_registers: executable.number_of_registers,
                number_of_arguments: executable.number_of_arguments,
            },
            super::ffi::ExecutableMetadata {
                property_lookup_cache_count: executable.cache_counters.property_lookup_cache_count,
                global_variable_cache_count: executable.cache_counters.global_variable_cache_count,
                environment_coordinate_cache_count: executable.cache_counters.environment_coordinate_cache_count,
                template_object_cache_count: executable.cache_counters.template_object_cache_count,
                object_shape_cache_count: executable.cache_counters.object_shape_cache_count,
                object_property_iterator_cache_count: executable.cache_counters.object_property_iterator_cache_count,
                environment_shape_cache_count: executable.cache_counters.environment_shape_cache_count,
                is_strict: executable.strict,
                length_identifier: executable.length_identifier,
            },
            super::ffi::ExecutableSlices {
                identifier_table: &native_identifiers,
                property_key_table: &native_property_keys,
                string_table: &native_strings,
                constants_data: constants_bytes.as_slice(),
                constants_count,
                local_variable_metadata: &local_variable_metadata,
                argument_variable_names: &argument_variable_names,
                compiled_regexes: &[],
            },
            vm_ptr,
            source_code_ptr,
            &sfd_ptrs,
            &bp_ptrs,
        )
    }
}
