/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The binary format decoder. It decodes the module structure and leaves function bodies and constant expressions as
//! byte ranges, which validation decodes as it checks them.

use crate::error::{Error, Result};
use crate::module::CustomSection;
use crate::module::{
    ConstantExpression, Data, DataMode, Element, ElementItems, ElementMode, Export, ExternalKind, FunctionBody, Global,
    Import, ImportDescription, Module, RecursionGroup, Table,
};
use crate::opcode;
use crate::reader::Reader;
use crate::types::{
    AddressType, CompositeType, FieldType, FuncType, GlobalType, HeapType, Limits, MemoryType, RefType, StorageType,
    SubType, TableType, TagType, ValType,
};

// https://webassembly.github.io/spec/js-api/#limits
// The maximum size of a function body, including locals declarations, is 7,654,321 bytes.
const MAX_FUNCTION_SIZE: u32 = 7_654_321;

const SECTION_CUSTOM: u8 = 0;
const SECTION_TYPE: u8 = 1;
const SECTION_IMPORT: u8 = 2;
const SECTION_FUNCTION: u8 = 3;
const SECTION_TABLE: u8 = 4;
const SECTION_MEMORY: u8 = 5;
const SECTION_GLOBAL: u8 = 6;
const SECTION_EXPORT: u8 = 7;
const SECTION_START: u8 = 8;
const SECTION_ELEMENT: u8 = 9;
const SECTION_CODE: u8 = 10;
const SECTION_DATA: u8 = 11;
const SECTION_DATA_COUNT: u8 = 12;
const SECTION_TAG: u8 = 13;

/// The position of each non-custom section in the order the binary format requires.
fn section_order(id: u8) -> Option<u8> {
    Some(match id {
        SECTION_TYPE => 1,
        SECTION_IMPORT => 2,
        SECTION_FUNCTION => 3,
        SECTION_TABLE => 4,
        SECTION_MEMORY => 5,
        SECTION_TAG => 6,
        SECTION_GLOBAL => 7,
        SECTION_EXPORT => 8,
        SECTION_START => 9,
        SECTION_ELEMENT => 10,
        SECTION_DATA_COUNT => 11,
        SECTION_CODE => 12,
        SECTION_DATA => 13,
        _ => return None,
    })
}

pub fn parse(bytes: &[u8]) -> Result<Module> {
    let mut module = Module {
        bytes: bytes.into(),
        ..Module::default()
    };
    let mut reader = Reader::new(bytes);
    let magic = reader.read_bytes(4).map_err(|_| Error::malformed("unexpected end"))?;
    if magic != b"\0asm" {
        return Err(Error::malformed("magic header not detected"));
    }
    let version = reader.read_bytes(4).map_err(|_| Error::malformed("unexpected end"))?;
    if version != [1, 0, 0, 0] {
        return Err(Error::malformed("unknown binary version"));
    }

    let mut last_order = 0;
    let mut function_count = None;
    while !reader.is_at_end() {
        let id = reader.read_u8()?;
        let size = reader.read_u32()?;
        let mut section = reader.sub_reader(size as usize)?;
        if id != SECTION_CUSTOM {
            let order = section_order(id).ok_or_else(|| Error::malformed("malformed section id"))?;
            if order <= last_order {
                return Err(Error::malformed("unexpected content after last section"));
            }
            last_order = order;
        }
        match id {
            SECTION_CUSTOM => {
                let name = section.read_name()?.to_string();
                module.custom_sections.push(CustomSection {
                    name,
                    contents: section.position() as u32..section.end() as u32,
                });
                continue;
            }
            SECTION_TYPE => parse_type_section(&mut section, &mut module)?,
            SECTION_IMPORT => parse_import_section(&mut section, &mut module)?,
            SECTION_FUNCTION => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    let type_index = section.read_u32()?;
                    module.functions.push(type_index);
                    module.function_space.push(type_index);
                }
                function_count = Some(count);
            }
            SECTION_TABLE => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    let table = parse_table(&mut section)?;
                    module.table_space.push(table.ty);
                    module.tables.push(table);
                }
            }
            SECTION_MEMORY => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    let memory = parse_memory_type(&mut section)?;
                    module.memory_space.push(memory);
                    module.memories.push(memory);
                }
            }
            SECTION_TAG => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    let tag = parse_tag_type(&mut section)?;
                    module.tag_space.push(tag);
                    module.tags.push(tag);
                }
            }
            SECTION_GLOBAL => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    let ty = parse_global_type(&mut section)?;
                    let initializer = parse_constant_expression(&mut section)?;
                    module.global_space.push(ty);
                    module.globals.push(Global { ty, initializer });
                }
            }
            SECTION_EXPORT => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    let name = section.read_name()?.to_string();
                    let kind = match section.read_u8()? {
                        0x00 => ExternalKind::Function,
                        0x01 => ExternalKind::Table,
                        0x02 => ExternalKind::Memory,
                        0x03 => ExternalKind::Global,
                        0x04 => ExternalKind::Tag,
                        _ => return Err(Error::malformed("malformed export kind")),
                    };
                    let index = section.read_u32()?;
                    module.exports.push(Export { name, kind, index });
                }
            }
            SECTION_START => module.start = Some(section.read_u32()?),
            SECTION_ELEMENT => {
                let count = section.read_vector_length()?;
                for _ in 0..count {
                    module.elements.push(parse_element(&mut section)?);
                }
            }
            SECTION_DATA_COUNT => module.data_count = Some(section.read_u32()?),
            SECTION_CODE => {
                let count = section.read_vector_length()?;
                if function_count.unwrap_or(0) != count {
                    return Err(Error::malformed("function and code section have inconsistent lengths"));
                }
                for _ in 0..count {
                    module.code.push(parse_function_body(&mut section)?);
                }
            }
            SECTION_DATA => {
                let count = section.read_vector_length()?;
                if let Some(data_count) = module.data_count
                    && data_count != count
                {
                    return Err(Error::malformed(
                        "data count and data section have inconsistent lengths",
                    ));
                }
                for _ in 0..count {
                    module.data.push(parse_data(&mut section)?);
                }
            }
            _ => unreachable!("section ids are checked above"),
        }
        if !section.is_at_end() {
            return Err(Error::malformed("section size mismatch"));
        }
    }
    if function_count.unwrap_or(0) != 0 && module.code.is_empty() {
        return Err(Error::malformed("function and code section have inconsistent lengths"));
    }
    if module.data_count.is_some_and(|count| count != 0) && module.data.is_empty() {
        return Err(Error::malformed(
            "data count and data section have inconsistent lengths",
        ));
    }
    Ok(module)
}

fn parse_type_section(section: &mut Reader<'_>, module: &mut Module) -> Result<()> {
    let count = section.read_vector_length()?;
    for _ in 0..count {
        let start = module.types.len() as u32;
        if section.peek_u8()? == 0x4e {
            section.read_u8()?;
            let group_size = section.read_vector_length()?;
            for _ in 0..group_size {
                module.types.push(parse_sub_type(section)?);
            }
        } else {
            module.types.push(parse_sub_type(section)?);
        }
        let count = module.types.len() as u32 - start;
        module.recursion_groups.push(RecursionGroup { start, count });
    }
    Ok(())
}

fn parse_sub_type(reader: &mut Reader<'_>) -> Result<SubType> {
    let (is_final, supertypes) = match reader.peek_u8()? {
        byte @ (0x50 | 0x4f) => {
            reader.read_u8()?;
            let count = reader.read_vector_length()?;
            let mut supertypes = Vec::with_capacity(count as usize);
            for _ in 0..count {
                supertypes.push(reader.read_u32()?);
            }
            (byte == 0x4f, supertypes)
        }
        _ => (true, Vec::new()),
    };
    let composite = match reader.read_u8()? {
        0x60 => {
            let params = parse_value_types(reader)?;
            let results = parse_value_types(reader)?;
            CompositeType::Func(FuncType { params, results })
        }
        0x5f => {
            let count = reader.read_vector_length()?;
            let mut fields = Vec::with_capacity(count as usize);
            for _ in 0..count {
                fields.push(parse_field_type(reader)?);
            }
            CompositeType::Struct(fields)
        }
        0x5e => CompositeType::Array(parse_field_type(reader)?),
        _ => return Err(Error::malformed("integer representation too long")),
    };
    Ok(SubType {
        is_final,
        supertypes,
        composite,
    })
}

fn parse_field_type(reader: &mut Reader<'_>) -> Result<FieldType> {
    let storage = match reader.peek_u8()? {
        0x78 => {
            reader.read_u8()?;
            StorageType::I8
        }
        0x77 => {
            reader.read_u8()?;
            StorageType::I16
        }
        _ => StorageType::Val(parse_value_type(reader)?),
    };
    let mutable = match reader.read_u8()? {
        0 => false,
        1 => true,
        _ => return Err(Error::malformed("malformed mutability")),
    };
    Ok(FieldType { storage, mutable })
}

fn parse_value_types(reader: &mut Reader<'_>) -> Result<Vec<ValType>> {
    let count = reader.read_vector_length()?;
    let mut types = Vec::with_capacity(count as usize);
    for _ in 0..count {
        types.push(parse_value_type(reader)?);
    }
    Ok(types)
}

/// The abstract heap type a single byte encodes, both as a shorthand reference type (nullable) and as a heap type.
fn abstract_heap_type(byte: u8) -> Option<HeapType> {
    Some(match byte {
        0x70 => HeapType::Func,
        0x73 => HeapType::NoFunc,
        0x6f => HeapType::Extern,
        0x72 => HeapType::NoExtern,
        0x6e => HeapType::Any,
        0x6d => HeapType::Eq,
        0x6c => HeapType::I31,
        0x6b => HeapType::Struct,
        0x6a => HeapType::Array,
        0x71 => HeapType::None,
        0x69 => HeapType::Exn,
        0x74 => HeapType::NoExn,
        _ => return None,
    })
}

pub fn parse_heap_type(reader: &mut Reader<'_>) -> Result<HeapType> {
    if let Some(heap) = abstract_heap_type(reader.peek_u8()?) {
        reader.read_u8()?;
        return Ok(heap);
    }
    let index = reader.read_s33()?;
    if index < 0 {
        return Err(Error::malformed("malformed reference type"));
    }
    u32::try_from(index)
        .map(HeapType::Concrete)
        .map_err(|_| Error::malformed("malformed reference type"))
}

pub fn parse_reference_type(reader: &mut Reader<'_>) -> Result<RefType> {
    let byte = reader.read_u8()?;
    match byte {
        0x63 => Ok(RefType::new(true, parse_heap_type(reader)?)),
        0x64 => Ok(RefType::new(false, parse_heap_type(reader)?)),
        _ => abstract_heap_type(byte)
            .map(|heap| RefType::new(true, heap))
            .ok_or_else(|| Error::malformed("malformed reference type")),
    }
}

pub fn parse_value_type(reader: &mut Reader<'_>) -> Result<ValType> {
    let value = match reader.peek_u8()? {
        0x7f => ValType::I32,
        0x7e => ValType::I64,
        0x7d => ValType::F32,
        0x7c => ValType::F64,
        0x7b => ValType::V128,
        _ => return Ok(ValType::Ref(parse_reference_type(reader)?)),
    };
    reader.read_u8()?;
    Ok(value)
}

fn parse_limits(reader: &mut Reader<'_>, is_memory: bool) -> Result<Limits> {
    let flags = reader.read_u8()?;
    let valid_flags = if is_memory { 0x07 } else { 0x05 };
    if flags & !valid_flags != 0 {
        return Err(Error::malformed(if is_memory {
            "malformed memory limits flags"
        } else {
            "malformed table limits flags"
        }));
    }
    let address_type = if flags & 0x04 != 0 {
        AddressType::I64
    } else {
        AddressType::I32
    };
    let read = |reader: &mut Reader<'_>| match address_type {
        AddressType::I32 => reader.read_u32().map(u64::from),
        AddressType::I64 => reader.read_u64(),
    };
    let min = read(reader)?;
    let max = if flags & 0x01 != 0 { Some(read(reader)?) } else { None };
    Ok(Limits {
        min,
        max,
        address_type,
        shared: flags & 0x02 != 0,
    })
}

fn parse_table_type(reader: &mut Reader<'_>) -> Result<TableType> {
    let element = parse_reference_type(reader)?;
    let limits = parse_limits(reader, false)?;
    Ok(TableType { element, limits })
}

fn parse_table(reader: &mut Reader<'_>) -> Result<Table> {
    if reader.peek_u8()? == 0x40 {
        reader.read_u8()?;
        if reader.read_u8()? != 0x00 {
            return Err(Error::malformed("malformed table"));
        }
        let ty = parse_table_type(reader)?;
        let initializer = parse_constant_expression(reader)?;
        return Ok(Table {
            ty,
            initializer: Some(initializer),
        });
    }
    Ok(Table {
        ty: parse_table_type(reader)?,
        initializer: None,
    })
}

fn parse_memory_type(reader: &mut Reader<'_>) -> Result<MemoryType> {
    Ok(MemoryType {
        limits: parse_limits(reader, true)?,
    })
}

fn parse_global_type(reader: &mut Reader<'_>) -> Result<GlobalType> {
    let value = parse_value_type(reader)?;
    let mutable = match reader.read_u8()? {
        0 => false,
        1 => true,
        _ => return Err(Error::malformed("malformed mutability")),
    };
    Ok(GlobalType { value, mutable })
}

fn parse_tag_type(reader: &mut Reader<'_>) -> Result<TagType> {
    if reader.read_u8()? != 0x00 {
        return Err(Error::malformed("malformed tag attribute"));
    }
    Ok(TagType {
        type_index: reader.read_u32()?,
    })
}

fn parse_import_section(section: &mut Reader<'_>, module: &mut Module) -> Result<()> {
    let count = section.read_vector_length()?;
    for _ in 0..count {
        let module_name = section.read_name()?.to_string();
        let name = section.read_name()?.to_string();
        let description = match section.read_u8()? {
            0x00 => {
                let type_index = section.read_u32()?;
                module.function_space.push(type_index);
                module.imported_function_count += 1;
                ImportDescription::Function(type_index)
            }
            0x01 => {
                let table = parse_table_type(section)?;
                module.table_space.push(table);
                ImportDescription::Table(table)
            }
            0x02 => {
                let memory = parse_memory_type(section)?;
                module.memory_space.push(memory);
                ImportDescription::Memory(memory)
            }
            0x03 => {
                let global = parse_global_type(section)?;
                module.global_space.push(global);
                module.imported_global_count += 1;
                ImportDescription::Global(global)
            }
            0x04 => {
                let tag = parse_tag_type(section)?;
                module.tag_space.push(tag);
                ImportDescription::Tag(tag)
            }
            _ => return Err(Error::malformed("malformed import kind")),
        };
        module.imports.push(Import {
            module: module_name,
            name,
            description,
        });
    }
    Ok(())
}

/// Skip over the instructions of a constant expression up to and including its `end`. Only the instructions a
/// constant expression may contain are decoded; anything else is left for validation to reject.
pub fn parse_constant_expression(reader: &mut Reader<'_>) -> Result<ConstantExpression> {
    let start = reader.position() as u32;
    loop {
        let opcode = reader.read_u8()?;
        match opcode {
            opcode::END => break,
            opcode::I32_CONST => {
                reader.read_i32()?;
            }
            opcode::I64_CONST => {
                reader.read_i64()?;
            }
            opcode::F32_CONST => {
                reader.read_bytes(4)?;
            }
            opcode::F64_CONST => {
                reader.read_bytes(8)?;
            }
            opcode::REF_NULL => {
                parse_heap_type(reader)?;
            }
            opcode::REF_FUNC | opcode::GLOBAL_GET => {
                reader.read_u32()?;
            }
            opcode::I32_ADD
            | opcode::I32_SUB
            | opcode::I32_MUL
            | opcode::I64_ADD
            | opcode::I64_SUB
            | opcode::I64_MUL => {}
            opcode::PREFIX_SIMD => {
                let sub_opcode = reader.read_u32()?;
                if sub_opcode != opcode::simd::V128_CONST {
                    return Err(Error::invalid("constant expression required"));
                }
                reader.read_bytes(16)?;
            }
            opcode::PREFIX_GC => {
                let sub_opcode = reader.read_u32()?;
                match sub_opcode {
                    opcode::gc::STRUCT_NEW
                    | opcode::gc::STRUCT_NEW_DEFAULT
                    | opcode::gc::ARRAY_NEW
                    | opcode::gc::ARRAY_NEW_DEFAULT => {
                        reader.read_u32()?;
                    }
                    opcode::gc::ARRAY_NEW_FIXED => {
                        reader.read_u32()?;
                        reader.read_u32()?;
                    }
                    opcode::gc::REF_I31 | opcode::gc::ANY_CONVERT_EXTERN | opcode::gc::EXTERN_CONVERT_ANY => {}
                    _ => return Err(Error::invalid("constant expression required")),
                }
            }
            _ => return Err(Error::invalid("constant expression required")),
        }
    }
    Ok(ConstantExpression {
        code: start..reader.position() as u32,
    })
}

fn parse_function_indices(reader: &mut Reader<'_>) -> Result<ElementItems> {
    let count = reader.read_vector_length()?;
    let mut functions = Vec::with_capacity(count as usize);
    for _ in 0..count {
        functions.push(reader.read_u32()?);
    }
    Ok(ElementItems::Functions(functions))
}

fn parse_element_expressions(reader: &mut Reader<'_>) -> Result<ElementItems> {
    let count = reader.read_vector_length()?;
    let mut expressions = Vec::with_capacity(count as usize);
    for _ in 0..count {
        expressions.push(parse_constant_expression(reader)?);
    }
    Ok(ElementItems::Expressions(expressions))
}

/// Segments of function indices hold non-null function references.
const FUNCTION_INDEX_ELEMENT_TYPE: RefType = RefType {
    nullable: false,
    heap: HeapType::Func,
};

fn parse_element_kind(reader: &mut Reader<'_>) -> Result<RefType> {
    if reader.read_u8()? != 0x00 {
        return Err(Error::malformed("malformed element kind"));
    }
    Ok(FUNCTION_INDEX_ELEMENT_TYPE)
}

fn parse_element(reader: &mut Reader<'_>) -> Result<Element> {
    let flags = reader.read_u32()?;
    let (ty, mode, items) = match flags {
        0 => {
            let offset = parse_constant_expression(reader)?;
            let items = parse_function_indices(reader)?;
            (
                FUNCTION_INDEX_ELEMENT_TYPE,
                ElementMode::Active { table: 0, offset },
                items,
            )
        }
        1 => {
            let ty = parse_element_kind(reader)?;
            (ty, ElementMode::Passive, parse_function_indices(reader)?)
        }
        2 => {
            let table = reader.read_u32()?;
            let offset = parse_constant_expression(reader)?;
            let ty = parse_element_kind(reader)?;
            (
                ty,
                ElementMode::Active { table, offset },
                parse_function_indices(reader)?,
            )
        }
        3 => {
            let ty = parse_element_kind(reader)?;
            (ty, ElementMode::Declarative, parse_function_indices(reader)?)
        }
        4 => {
            let offset = parse_constant_expression(reader)?;
            let items = parse_element_expressions(reader)?;
            (RefType::FUNCREF, ElementMode::Active { table: 0, offset }, items)
        }
        5 => {
            let ty = parse_reference_type(reader)?;
            (ty, ElementMode::Passive, parse_element_expressions(reader)?)
        }
        6 => {
            let table = reader.read_u32()?;
            let offset = parse_constant_expression(reader)?;
            let ty = parse_reference_type(reader)?;
            (
                ty,
                ElementMode::Active { table, offset },
                parse_element_expressions(reader)?,
            )
        }
        7 => {
            let ty = parse_reference_type(reader)?;
            (ty, ElementMode::Declarative, parse_element_expressions(reader)?)
        }
        _ => return Err(Error::malformed("malformed elements segment kind")),
    };
    Ok(Element { ty, mode, items })
}

fn parse_data(reader: &mut Reader<'_>) -> Result<Data> {
    let flags = reader.read_u32()?;
    let mode = match flags {
        0 => DataMode::Active {
            memory: 0,
            offset: parse_constant_expression(reader)?,
        },
        1 => DataMode::Passive,
        2 => {
            let memory = reader.read_u32()?;
            DataMode::Active {
                memory,
                offset: parse_constant_expression(reader)?,
            }
        }
        _ => return Err(Error::malformed("malformed data segment kind")),
    };
    let length = reader.read_u32()?;
    let start = reader.position() as u32;
    reader.read_bytes(length as usize)?;
    Ok(Data {
        mode,
        bytes: start..start + length,
    })
}

fn parse_function_body(section: &mut Reader<'_>) -> Result<FunctionBody> {
    let size = section.read_u32()?;
    let mut body = section.sub_reader(size as usize)?;
    if size > MAX_FUNCTION_SIZE {
        return Err(Error::invalid("function body too large"));
    }
    let run_count = body.read_u32()?;
    let mut locals = Vec::new();
    let mut declared_local_count = 0u32;
    for _ in 0..run_count {
        let count = body.read_u32()?;
        declared_local_count = declared_local_count
            .checked_add(count)
            .ok_or_else(|| Error::malformed("too many locals"))?;
        let ty = parse_value_type(&mut body)?;
        locals.push((count, ty));
    }
    let start = body.position() as u32;
    if body.is_at_end() {
        return Err(Error::malformed("unexpected end of section or function"));
    }
    Ok(FunctionBody {
        locals,
        declared_local_count,
        code: start..body.end() as u32,
    })
}
