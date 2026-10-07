/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode cache blobs on this runtime's types: Script and Source Text Module Records materialized from a decoded
//! blob, and blobs installed into records that already run.
//!
//! The executables made from a blob run their bytecode in place in it and keep it alive. A function's executable stays
//! in the blob until the function is first called. Installing a blob into a running record matches every function the
//! record has created to a function of the blob, then gives each one that ran an executable from the blob, which takes
//! over the inline caches of the one it replaces, and each one that did not its executable in the blob.

use core::cell::{Ref, RefCell};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use crate::bytecode::executable::Executable;
use crate::bytecode::generator::FunctionSfdMetadata;
use crate::bytecode_cache::{
    DecodedCacheBlob, DecodedCachedExecutableRecord, DecodedExecutableRecord, DecodedFunctionRecord,
};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::parser_error::ParserError;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::source_code::SourceCode;

/// RustIntegration::DecodedBytecodeCache: a decoded blob, which the records materialized from it share, and which is
/// validated against the source code once.
pub struct DecodedBytecodeCache {
    blob: RefCell<DecodedCacheBlob>,
}

impl DecodedBytecodeCache {
    pub fn new(blob: DecodedCacheBlob) -> Self {
        Self {
            blob: RefCell::new(blob),
        }
    }

    /// Validates the blob for source code of `source_length_in_code_units` code units, and checks nothing else after
    /// that succeeded once.
    pub fn validate(&self, source_length_in_code_units: usize) -> bool {
        self.blob
            .borrow_mut()
            .validate_for_materialization(source_length_in_code_units)
            .is_ok()
    }

    /// The blob, if it passed validation for source code of `source_length_in_code_units` code units.
    pub fn validated_blob(&self, source_length_in_code_units: usize) -> Option<Ref<'_, DecodedCacheBlob>> {
        self.validate(source_length_in_code_units).then(|| self.blob.borrow())
    }
}

/// The error a record that cannot be materialized from a bytecode cache blob reports.
pub fn failed_to_materialize_bytecode_cache() -> Vec<ParserError> {
    vec![ParserError {
        message: "Failed to materialize bytecode cache".to_string(),
        line: 0,
        column: 0,
    }]
}

/// ExecutableBacking: what the executables of a Script or Source Text Module Record were made from, and how that
/// changes while a bytecode cache is generated for the record and installed into it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExecutableBacking {
    /// Compiled from source code on the VM's thread.
    Source,
    /// Compiled on another thread.
    HeapBytecode,
    GeneratingFreshCacheFromSource,
    GeneratingFreshCacheFromHeapBytecode,
    /// Materialized from a bytecode cache blob, or with one installed.
    MappedBytecodeCache,
}

impl ExecutableBacking {
    pub fn is_source(self) -> bool {
        matches!(self, Self::Source | Self::GeneratingFreshCacheFromSource)
    }

    pub fn is_heap_bytecode(self) -> bool {
        matches!(self, Self::HeapBytecode | Self::GeneratingFreshCacheFromHeapBytecode)
    }

    pub fn is_mapped_bytecode_cache(self) -> bool {
        self == Self::MappedBytecodeCache
    }

    pub fn can_generate_bytecode_cache(self) -> bool {
        matches!(self, Self::Source | Self::HeapBytecode)
    }

    pub fn can_install_generated_bytecode_cache(self) -> bool {
        matches!(
            self,
            Self::GeneratingFreshCacheFromSource | Self::GeneratingFreshCacheFromHeapBytecode
        )
    }

    /// The backing once the generation of a bytecode cache began.
    pub fn with_bytecode_cache_generation_begun(self) -> Self {
        match self {
            Self::Source => Self::GeneratingFreshCacheFromSource,
            Self::HeapBytecode => Self::GeneratingFreshCacheFromHeapBytecode,
            _ => panic!("a bytecode cache is only generated for a record that has none, once at a time"),
        }
    }

    /// The backing once the generation of a bytecode cache ended without installing it.
    pub fn with_bytecode_cache_generation_finished_without_install(self) -> Self {
        match self {
            Self::GeneratingFreshCacheFromSource => Self::Source,
            Self::GeneratingFreshCacheFromHeapBytecode => Self::HeapBytecode,
            _ => panic!("only the generation of a bytecode cache that began can finish"),
        }
    }
}

/// Every function a record has created so far: those it declares, those its executables create, and so on for the
/// executables of the functions that ran.
pub fn functions_created_by(
    vm: &Vm,
    declared_functions: impl IntoIterator<Item = Gc<SharedFunctionInstanceData>>,
    executable: Option<Gc<Executable>>,
) -> MarkedVec<'_, Gc<SharedFunctionInstanceData>> {
    let functions = MarkedVec::new(vm);
    let mut seen = HashSet::new();
    let mut executables_to_visit: Vec<Gc<Executable>> = executable.into_iter().collect();
    let mut add_function = |function: Gc<SharedFunctionInstanceData>,
                            executables_to_visit: &mut Vec<Gc<Executable>>| {
        if seen.insert(function) {
            functions.push(function);
            executables_to_visit.extend(function.executable());
        }
    };
    for function in declared_functions {
        add_function(function, &mut executables_to_visit);
    }
    while let Some(executable) = executables_to_visit.pop() {
        for index in 0..executable.shared_function_data_count() {
            let function = executable.shared_function_data(u32::try_from(index).expect("the index fits in u32"));
            add_function(function, &mut executables_to_visit);
        }
    }
    functions
}

/// Whether none of `functions` still has an AST or bytecode compiled ahead of its first call, which a record with a
/// bytecode cache installed only compiles from the blob.
pub fn have_only_bytecode_cache_compile_inputs(functions: &MarkedVec<'_, Gc<SharedFunctionInstanceData>>) -> bool {
    functions
        .to_vec()
        .iter()
        .all(|function| !function.has_function_ast() && !function.has_precompiled_bytecode())
}

/// Creates the executable of a cached record together with the functions its bytecode creates, whose own executables
/// stay in the blob. Returns `None` if the record turns out to be malformed.
pub fn create_executable_and_its_functions(
    vm: &Vm,
    record: &DecodedExecutableRecord,
    source_code: &Rc<SourceCode>,
) -> Option<Gc<Executable>> {
    let functions = MarkedVec::new(vm);
    for function in record.functions()? {
        functions.push(SharedFunctionInstanceData::create_from_bytecode_cache(
            vm,
            &function,
            record.is_strict(),
            source_code,
        ));
    }
    Executable::create_from_bytecode_cache(vm, record, &functions, source_code, None)
}

/// The executable of a function whose executable stayed in a bytecode cache blob until its first call, as
/// rust_materialize_bytecode_cache_function makes it.
pub fn materialize_cached_function_executable(
    vm: &Vm,
    cached_executable: &DecodedCachedExecutableRecord,
    source_code: &Rc<SourceCode>,
) -> Option<Gc<Executable>> {
    create_executable_and_its_functions(vm, &cached_executable.decode_executable()?, source_code)
}

enum Replacement {
    CachedExecutable(DecodedCachedExecutableRecord),
    Executable(Gc<Executable>),
}

/// What installing a blob gives one function, once every function turned out to have its counterpart in the blob.
struct PendingFunctionInstall {
    function: Gc<SharedFunctionInstanceData>,
    replacement: Replacement,
    metadata: FunctionSfdMetadata,
}

/// Installing a bytecode cache blob into a running record: the functions the record has created, each of which must
/// match one function of the blob, and what each one gets once they all did.
pub struct BytecodeCacheInstall<'vm, 'source> {
    vm: &'vm Vm,
    source_code: &'source Rc<SourceCode>,
    existing_functions: Vec<Gc<SharedFunctionInstanceData>>,
    /// The indices of the existing functions by the source text range a blob identifies them by, in their order.
    existing_functions_by_source_text_range: HashMap<(usize, usize), Vec<usize>, foldhash::fast::RandomState>,
    matched: Vec<bool>,
    pending_installs: Vec<PendingFunctionInstall>,
    /// The executables made for functions that ran, which nothing else holds until the install commits.
    new_executables: MarkedVec<'vm, Gc<Executable>>,
}

impl<'vm, 'source> BytecodeCacheInstall<'vm, 'source> {
    /// `existing_functions` must stay alive until the install commits or is dropped, as the record does for those it
    /// created.
    pub fn new(
        vm: &'vm Vm,
        source_code: &'source Rc<SourceCode>,
        existing_functions: &MarkedVec<'_, Gc<SharedFunctionInstanceData>>,
    ) -> Self {
        let existing_functions = existing_functions.to_vec();
        let mut existing_functions_by_source_text_range: HashMap<_, Vec<_>, _> = HashMap::default();
        for (index, function) in existing_functions.iter().enumerate() {
            existing_functions_by_source_text_range
                .entry(function.bytecode_cache_source_text_range())
                .or_default()
                .push(index);
        }
        Self {
            vm,
            source_code,
            matched: vec![false; existing_functions.len()],
            existing_functions,
            existing_functions_by_source_text_range,
            pending_installs: Vec::new(),
            new_executables: MarkedVec::new(vm),
        }
    }

    fn take_matching_function(
        &mut self,
        function: &DecodedFunctionRecord,
        outer_strict: bool,
    ) -> Option<Gc<SharedFunctionInstanceData>> {
        let source_text_range = function.source_text_range();
        let index = self
            .existing_functions_by_source_text_range
            .get(&(
                source_text_range.start,
                source_text_range.end.saturating_sub(source_text_range.start),
            ))?
            .iter()
            .copied()
            .find(|&index| {
                !self.matched[index]
                    && self.existing_functions[index].matches_bytecode_cache_function(function, outer_strict)
            })?;
        self.matched[index] = true;
        Some(self.existing_functions[index])
    }

    /// Matches a function of the blob, nested in code that is strict if `outer_strict` is, to one the record created,
    /// and prepares what that one gets: an executable from the blob if it ran, which recursively matches the functions
    /// it creates, and its executable in the blob otherwise. Returns `None` if no function matches or the blob turns
    /// out to be malformed.
    pub fn prepare_function(
        &mut self,
        function: &DecodedFunctionRecord,
        outer_strict: bool,
    ) -> Option<Gc<SharedFunctionInstanceData>> {
        let existing_function = self.take_matching_function(function, outer_strict)?;
        let replacement = match existing_function.executable() {
            None => Replacement::CachedExecutable(function.cached_executable()),
            Some(existing_executable) => {
                let record = function.cached_executable().decode_executable()?;
                Replacement::Executable(self.prepare_executable(&record, Some(existing_executable))?)
            }
        };
        self.pending_installs.push(PendingFunctionInstall {
            function: existing_function,
            replacement,
            metadata: function.scope_metadata().clone(),
        });
        Some(existing_function)
    }

    /// The executable of a record of the blob, whose functions match functions the record created, and which takes over
    /// the inline caches of `replaced_executable`.
    pub fn prepare_executable(
        &mut self,
        record: &DecodedExecutableRecord,
        replaced_executable: Option<Gc<Executable>>,
    ) -> Option<Gc<Executable>> {
        let functions = MarkedVec::new(self.vm);
        for function in record.functions()? {
            functions.push(self.prepare_function(&function, record.is_strict())?);
        }
        let executable =
            Executable::create_from_bytecode_cache(self.vm, record, &functions, self.source_code, replaced_executable)?;
        self.new_executables.push(executable);
        Some(executable)
    }

    /// Gives every function what was prepared for it, if every function the record created found its counterpart in
    /// the blob. Otherwise nothing changes and this returns false.
    pub fn commit(self) -> bool {
        if !self.matched.iter().all(|matched| *matched) {
            return false;
        }
        for install in self.pending_installs {
            match install.replacement {
                Replacement::CachedExecutable(cached_executable) => install
                    .function
                    .install_cached_bytecode_executable(cached_executable, &install.metadata),
                Replacement::Executable(executable) => install
                    .function
                    .install_bytecode_cache_executable(executable, &install.metadata),
            }
        }
        drop(self.new_executables);
        true
    }
}
