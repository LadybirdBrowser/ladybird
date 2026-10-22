/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A decoded module. Function bodies and constant expressions are not decoded into another form: they are byte
//! ranges into the module's binary, which the validator checks and the interpreter executes in place.

use crate::types::{FuncType, GlobalType, MemoryType, RefType, SubType, TableType, TagType, ValType};
use crate::validator::FunctionInfo;
use std::ops::Range;

/// A byte range of the module's binary.
pub type ByteRange = Range<u32>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExternalKind {
    Function,
    Table,
    Memory,
    Global,
    Tag,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImportDescription {
    Function(u32),
    Table(TableType),
    Memory(MemoryType),
    Global(GlobalType),
    Tag(TagType),
}

#[derive(Debug, Clone)]
pub struct Import {
    pub module: String,
    pub name: String,
    pub description: ImportDescription,
}

#[derive(Debug, Clone)]
pub struct Export {
    pub name: String,
    pub kind: ExternalKind,
    pub index: u32,
}

/// A constant expression: the bytes of its instructions, including the final `end`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConstantExpression {
    pub code: ByteRange,
}

#[derive(Debug, Clone)]
pub struct Table {
    pub ty: TableType,
    pub initializer: Option<ConstantExpression>,
}

#[derive(Debug, Clone)]
pub struct Global {
    pub ty: GlobalType,
    pub initializer: ConstantExpression,
}

#[derive(Debug, Clone)]
pub enum ElementMode {
    Passive,
    Declarative,
    Active { table: u32, offset: ConstantExpression },
}

#[derive(Debug, Clone)]
pub enum ElementItems {
    Functions(Vec<u32>),
    Expressions(Vec<ConstantExpression>),
}

impl ElementItems {
    pub fn len(&self) -> usize {
        match self {
            Self::Functions(functions) => functions.len(),
            Self::Expressions(expressions) => expressions.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[derive(Debug, Clone)]
pub struct Element {
    pub ty: RefType,
    pub mode: ElementMode,
    pub items: ElementItems,
}

#[derive(Debug, Clone)]
pub enum DataMode {
    Passive,
    Active { memory: u32, offset: ConstantExpression },
}

#[derive(Debug, Clone)]
pub struct Data {
    pub mode: DataMode,
    pub bytes: ByteRange,
}

/// The body of a defined function.
#[derive(Debug, Clone)]
pub struct FunctionBody {
    /// The declared locals after the parameters, as (count, type) runs.
    pub locals: Vec<(u32, ValType)>,
    /// The total number of locals after the parameters.
    pub declared_local_count: u32,
    /// The function's instructions, starting at the first instruction and ending after its final `end`.
    pub code: ByteRange,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecursionGroup {
    pub start: u32,
    pub count: u32,
}

/// A custom section: its name and the range of its contents.
#[derive(Debug, Clone)]
pub struct CustomSection {
    pub name: String,
    pub contents: ByteRange,
}

#[derive(Debug, Default)]
pub struct Module {
    pub bytes: Box<[u8]>,
    pub types: Vec<SubType>,
    pub recursion_groups: Vec<RecursionGroup>,
    pub imports: Vec<Import>,
    /// The type index of each defined function.
    pub functions: Vec<u32>,
    pub tables: Vec<Table>,
    pub memories: Vec<MemoryType>,
    pub globals: Vec<Global>,
    pub tags: Vec<TagType>,
    pub exports: Vec<Export>,
    pub start: Option<u32>,
    pub elements: Vec<Element>,
    pub data_count: Option<u32>,
    pub code: Vec<FunctionBody>,
    pub data: Vec<Data>,
    pub custom_sections: Vec<CustomSection>,

    // Index spaces, imports first.
    pub imported_function_count: u32,
    pub imported_global_count: u32,
    /// The type index of every function.
    pub function_space: Vec<u32>,
    pub table_space: Vec<TableType>,
    pub memory_space: Vec<MemoryType>,
    pub global_space: Vec<GlobalType>,
    pub tag_space: Vec<TagType>,

    /// Filled in by validation: the canonical identity of each type, equal exactly for equal types, assigned by a
    /// canonicalizer local to this module.
    pub type_identities: Vec<u32>,
    /// Filled in by validation: what the interpreter needs to run each defined function.
    pub function_infos: Vec<FunctionInfo>,
}

impl Module {
    pub fn bytes_of(&self, range: &ByteRange) -> &[u8] {
        &self.bytes[range.start as usize..range.end as usize]
    }

    pub fn function_type(&self, function: u32) -> Option<&FuncType> {
        self.types
            .get(*self.function_space.get(function as usize)? as usize)?
            .as_func()
    }

    pub fn total_function_count(&self) -> u32 {
        self.function_space.len() as u32
    }
}
