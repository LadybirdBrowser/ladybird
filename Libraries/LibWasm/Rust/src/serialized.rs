/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::CompiledFunction;
use crate::CraneliftInsn;
use crate::CraneliftTrap;
use crate::HelperReloc;
use crate::RuntimeLayout;
use crate::SERIALIZED_CODE_ALIGNMENT;
use crate::compile_to_bytes;
use std::mem::align_of;
use std::mem::size_of;
use std::mem::size_of_val;

#[repr(C)]
#[derive(Clone, Copy)]
struct InputHeader {
    function_count: u32,
    layout_offset: u32,
    outcome_return: u64,
    output_size: u64,
    total_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InputFunctionEntry {
    insn_offset: u32,
    insn_count: u32,
    result_arity: u32,
    num_locals: u32,
    locals_offset: u32,
    num_params: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct OutputHeader {
    function_count: u32,
    _pad: u32,
    code_base_offset: u64,
    reloc_region_start: u64,
    total_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct OutputFunctionEntry {
    code_offset: u64,
    code_size: u32,
    compiled: u32,
    reloc_offset: u64,
    reloc_count: u32,
    _padding_after_reloc_count: u32,
    trap_offset: u64,
    trap_count: u32,
    _padding: u32,
}

fn align_up(value: usize, alignment: usize) -> Result<usize, &'static str> {
    value.checked_next_multiple_of(alignment).ok_or("alignment overflow")
}

fn as_bytes_slice<T>(value: &[T]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(value.as_ptr().cast::<u8>(), size_of_val(value)) }
}

fn read_pod<T: Copy>(base: &[u8], offset: usize) -> Result<T, &'static str> {
    let end = offset.checked_add(size_of::<T>()).ok_or("overflow")?;
    let bytes = base.get(offset..end).ok_or("out of bounds read")?;
    Ok(unsafe { (bytes.as_ptr().cast::<T>()).read_unaligned() })
}

fn parse_input(
    input: &[u8],
    output_size: usize,
) -> Result<(InputHeader, Vec<InputFunctionEntry>, RuntimeLayout), &'static str> {
    let header: InputHeader = read_pod(input, 0)?;
    if usize::try_from(header.total_size).map_err(|_| "total_size overflow")? != input.len() {
        return Err("input buffer size mismatch");
    }
    if usize::try_from(header.output_size).map_err(|_| "output_size overflow")? != output_size {
        return Err("output buffer size mismatch");
    }

    let func_count = usize::try_from(header.function_count).map_err(|_| "function_count overflow")?;
    let entries_offset = size_of::<InputHeader>();
    let entries_size = func_count
        .checked_mul(size_of::<InputFunctionEntry>())
        .ok_or("input entries size overflow")?;
    let entries_end = entries_offset
        .checked_add(entries_size)
        .ok_or("input entries size overflow")?;
    if entries_end > input.len() {
        return Err("input entries are truncated");
    }

    let mut entries = Vec::with_capacity(func_count);
    let mut total_insn_count = 0usize;
    let mut total_locals_size = 0usize;
    for i in 0..func_count {
        let entry_offset = i
            .checked_mul(size_of::<InputFunctionEntry>())
            .and_then(|offset| entries_offset.checked_add(offset))
            .ok_or("input entry offset overflow")?;
        let entry: InputFunctionEntry = read_pod(input, entry_offset)?;
        total_insn_count = total_insn_count
            .checked_add(entry.insn_count as usize)
            .ok_or("instruction count overflow")?;
        total_locals_size = total_locals_size
            .checked_add(entry.num_locals as usize)
            .ok_or("locals size overflow")?;
        entries.push(entry);
    }

    let insn_region_offset = align_up(entries_end, align_of::<CraneliftInsn>())?;
    let insn_region_size = total_insn_count
        .checked_mul(size_of::<CraneliftInsn>())
        .ok_or("instruction region size overflow")?;
    let locals_region_offset = insn_region_offset
        .checked_add(insn_region_size)
        .ok_or("locals region offset overflow")?;
    let layout_offset = align_up(
        locals_region_offset
            .checked_add(total_locals_size)
            .ok_or("layout offset overflow")?,
        align_of::<RuntimeLayout>(),
    )?;
    let expected_size = layout_offset
        .checked_add(size_of::<RuntimeLayout>())
        .ok_or("input size overflow")?;
    if expected_size != input.len()
        || usize::try_from(header.layout_offset).map_err(|_| "layout_offset overflow")? != layout_offset
    {
        return Err("input regions are not canonical");
    }

    let mut insn_cursor = insn_region_offset;
    let mut locals_cursor = locals_region_offset;
    for entry in &entries {
        if entry.insn_offset as usize != insn_cursor || entry.locals_offset as usize != locals_cursor {
            return Err("input regions are not canonical");
        }
        insn_cursor = insn_cursor
            .checked_add(
                (entry.insn_count as usize)
                    .checked_mul(size_of::<CraneliftInsn>())
                    .ok_or("instruction region size overflow")?,
            )
            .ok_or("instruction region offset overflow")?;
        locals_cursor = locals_cursor
            .checked_add(entry.num_locals as usize)
            .ok_or("locals region offset overflow")?;
    }

    let layout = read_pod(input, layout_offset)?;
    Ok((header, entries, layout))
}

fn select_compiled_functions(
    compiled_chunks: Vec<Vec<(usize, CompiledFunction)>>,
    code_base_offset: usize,
    output_size: usize,
) -> (Vec<(usize, CompiledFunction)>, usize, usize) {
    let mut compiled_functions = Vec::new();
    let mut code_size = 0usize;
    let mut reloc_size = 0usize;
    for (index, compiled) in compiled_chunks.into_iter().flatten() {
        let Some(aligned_code_size) = align_up(compiled.code.len(), SERIALIZED_CODE_ALIGNMENT).ok() else {
            continue;
        };
        let Some(function_reloc_size) = compiled
            .relocs
            .len()
            .checked_mul(size_of::<HelperReloc>())
            .and_then(|size| {
                compiled
                    .traps
                    .len()
                    .checked_mul(size_of::<CraneliftTrap>())
                    .and_then(|trap_size| size.checked_add(trap_size))
            })
        else {
            continue;
        };
        let Some(candidate_code_size) = code_size.checked_add(aligned_code_size) else {
            continue;
        };
        let Some(candidate_reloc_size) = reloc_size.checked_add(function_reloc_size) else {
            continue;
        };
        let Some(candidate_total_size) = code_base_offset
            .checked_add(candidate_code_size)
            .and_then(|offset| align_up(offset, align_of::<HelperReloc>()).ok())
            .and_then(|offset| offset.checked_add(candidate_reloc_size))
        else {
            continue;
        };
        if candidate_total_size > output_size {
            continue;
        }

        code_size = candidate_code_size;
        reloc_size = candidate_reloc_size;
        compiled_functions.push((index, compiled));
    }

    (compiled_functions, code_size, reloc_size)
}

pub fn compile_serialized_buffer(input: &[u8], output: &mut [u8]) -> Result<usize, &'static str> {
    let (header, entries, layout) = parse_input(input, output.len())?;
    let func_count = usize::try_from(header.function_count).map_err(|_| "function_count overflow")?;

    let thread_count = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .max(1);
    let chunk_size = func_count.div_ceil(thread_count.max(1));
    let mapped_ref: &[u8] = input;
    let layout_ref = &layout;
    let outcome_return = header.outcome_return;

    // Compile into temporary per-function allocations first so the serialized
    // output contains only bytes that Cranelift actually produced.
    let compiled_chunks = std::thread::scope(|scope| {
        let mut handles = Vec::with_capacity(thread_count);
        for chunk_idx in 0..thread_count {
            let start = chunk_idx * chunk_size;
            if start >= func_count {
                break;
            }
            let end = (start + chunk_size).min(func_count);
            let chunk_entries = &entries[start..end];
            handles.push(scope.spawn(move || {
                let mut out: Vec<(usize, CompiledFunction)> = Vec::with_capacity(end - start);
                for (offset_in_chunk, entry) in chunk_entries.iter().enumerate() {
                    let i = start + offset_in_chunk;
                    if entry.insn_count == 0 {
                        continue;
                    }
                    let Ok(insn_offset) = usize::try_from(entry.insn_offset) else {
                        continue;
                    };
                    let insn_count = entry.insn_count as usize;
                    let Some(insn_bytes_len) = insn_count.checked_mul(size_of::<CraneliftInsn>()) else {
                        continue;
                    };
                    let Some(insn_end) = insn_offset.checked_add(insn_bytes_len) else {
                        continue;
                    };
                    let Some(insn_bytes) = mapped_ref.get(insn_offset..insn_end) else {
                        continue;
                    };
                    if insn_offset % align_of::<CraneliftInsn>() != 0 {
                        continue;
                    }
                    let (prefix, insns, suffix) = unsafe { insn_bytes.align_to::<CraneliftInsn>() };
                    if !prefix.is_empty() || !suffix.is_empty() || insns.len() != insn_count {
                        continue;
                    }
                    let num_locals = entry.num_locals as usize;
                    let local_types = usize::try_from(entry.locals_offset)
                        .ok()
                        .and_then(|offset| offset.checked_add(num_locals).map(|end| (offset, end)))
                        .and_then(|(offset, end)| mapped_ref.get(offset..end))
                        .unwrap_or(&[]);
                    if let Ok(compiled) = compile_to_bytes(
                        insns,
                        layout_ref,
                        outcome_return,
                        entry.result_arity,
                        entry.num_locals,
                        entry.num_params,
                        local_types,
                    ) {
                        out.push((i, compiled));
                    }
                }
                out
            }));
        }
        handles
            .into_iter()
            .map(|handle| handle.join().map_err(|_| "compiler worker panicked"))
            .collect::<Result<Vec<_>, _>>()
    })?;

    // Pack the results as a header and function table followed by compact code
    // and relocation regions.
    let out_entries_offset = size_of::<OutputHeader>();
    let output_entries_size = func_count
        .checked_mul(size_of::<OutputFunctionEntry>())
        .ok_or("output entries size overflow")?;
    let code_base_offset = align_up(
        out_entries_offset
            .checked_add(output_entries_size)
            .ok_or("code base offset overflow")?,
        SERIALIZED_CODE_ALIGNMENT,
    )?;

    if code_base_offset > output.len() {
        return Err("output metadata out of bounds");
    }

    // Keep the successfully compiled functions that fit in the supplied output
    // buffer. A single unusually code-dense function should fall back to the
    // interpreter without discarding the rest of the batch.
    let (compiled_functions, code_size, reloc_size) =
        select_compiled_functions(compiled_chunks, code_base_offset, output.len());

    let reloc_region_start = align_up(
        code_base_offset
            .checked_add(code_size)
            .ok_or("relocation region offset overflow")?,
        align_of::<HelperReloc>(),
    )?;
    let total_size = reloc_region_start
        .checked_add(reloc_size)
        .ok_or("output size overflow")?;
    debug_assert!(total_size <= output.len());

    output
        .get_mut(0..code_base_offset)
        .ok_or("output metadata out of bounds")?
        .fill(0);
    let output_header = OutputHeader {
        function_count: header.function_count,
        _pad: 0,
        code_base_offset: u64::try_from(code_base_offset).map_err(|_| "code base offset overflow")?,
        reloc_region_start: u64::try_from(reloc_region_start).map_err(|_| "relocation region offset overflow")?,
        total_size: u64::try_from(total_size).map_err(|_| "output size overflow")?,
    };
    let output_header_bytes = as_bytes_slice(std::slice::from_ref(&output_header));
    output
        .get_mut(0..output_header_bytes.len())
        .ok_or("output header out of bounds")?
        .copy_from_slice(output_header_bytes);

    let mut code_cursor = 0usize;
    let mut reloc_cursor = 0usize;
    for (i, compiled) in compiled_functions {
        let code = compiled.code;
        let relocs = compiled.relocs;
        let traps = compiled.traps;
        let aligned = align_up(code.len(), SERIALIZED_CODE_ALIGNMENT).map_err(|_| "code alignment overflow")?;
        let reloc_bytes_len = relocs
            .len()
            .checked_mul(size_of::<HelperReloc>())
            .ok_or("relocation size overflow")?;
        let trap_bytes_len = traps
            .len()
            .checked_mul(size_of::<CraneliftTrap>())
            .ok_or("trap size overflow")?;
        let Some(code_end) = code_cursor.checked_add(aligned) else {
            continue;
        };
        if code_end > code_size {
            continue;
        }
        let Some(reloc_end) = reloc_cursor
            .checked_add(reloc_bytes_len)
            .and_then(|end| end.checked_add(trap_bytes_len))
        else {
            continue;
        };
        if reloc_end > reloc_size {
            continue;
        }
        let code_offset = code_cursor;
        let code_dst = code_base_offset
            .checked_add(code_offset)
            .ok_or("code destination overflow")?;
        let code_dst_end = code_dst.checked_add(code.len()).ok_or("code destination overflow")?;
        output
            .get_mut(code_dst..code_dst_end)
            .ok_or("code destination out of bounds")?
            .copy_from_slice(&code);

        let reloc_offset = reloc_cursor;
        if !relocs.is_empty() {
            let reloc_dst = reloc_region_start
                .checked_add(reloc_offset)
                .ok_or("relocation destination overflow")?;
            let reloc_dst_end = reloc_dst
                .checked_add(reloc_bytes_len)
                .ok_or("relocation destination overflow")?;
            output
                .get_mut(reloc_dst..reloc_dst_end)
                .ok_or("relocation destination out of bounds")?
                .copy_from_slice(as_bytes_slice(&relocs));
        }

        let trap_offset = reloc_cursor
            .checked_add(reloc_bytes_len)
            .ok_or("trap offset overflow")?;
        if !traps.is_empty() {
            let trap_dst = reloc_region_start
                .checked_add(trap_offset)
                .ok_or("trap destination overflow")?;
            let trap_dst_end = trap_dst
                .checked_add(trap_bytes_len)
                .ok_or("trap destination overflow")?;
            output
                .get_mut(trap_dst..trap_dst_end)
                .ok_or("trap destination out of bounds")?
                .copy_from_slice(as_bytes_slice(&traps));
        }

        let entry = OutputFunctionEntry {
            code_offset: u64::try_from(code_offset).map_err(|_| "code offset overflow")?,
            code_size: u32::try_from(code.len()).map_err(|_| "code size overflow")?,
            compiled: 1,
            reloc_offset: u64::try_from(reloc_offset).map_err(|_| "reloc offset overflow")?,
            reloc_count: u32::try_from(relocs.len()).map_err(|_| "reloc count overflow")?,
            _padding_after_reloc_count: 0,
            trap_offset: u64::try_from(trap_offset).map_err(|_| "trap offset overflow")?,
            trap_count: u32::try_from(traps.len()).map_err(|_| "trap count overflow")?,
            _padding: 0,
        };
        let entry_dst = i
            .checked_mul(size_of::<OutputFunctionEntry>())
            .and_then(|offset| out_entries_offset.checked_add(offset))
            .ok_or("output entry offset overflow")?;
        let entry_dst_end = entry_dst
            .checked_add(size_of::<OutputFunctionEntry>())
            .ok_or("output entry offset overflow")?;
        let entry_bytes = as_bytes_slice(std::slice::from_ref(&entry));
        output
            .get_mut(entry_dst..entry_dst_end)
            .ok_or("output entry out of bounds")?
            .copy_from_slice(entry_bytes);

        code_cursor = code_end;
        reloc_cursor = reloc_end;
    }

    Ok(total_size)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_function_entry_layout_has_no_implicit_padding() {
        assert_eq!(size_of::<OutputFunctionEntry>(), 48);
        assert_eq!(std::mem::offset_of!(OutputFunctionEntry, trap_offset), 32);
    }

    #[test]
    fn rejects_truncated_header() {
        assert_eq!(
            compile_serialized_buffer(&[], &mut []).unwrap_err(),
            "out of bounds read"
        );
    }

    #[test]
    fn rejects_buffer_size_mismatch() {
        let input = vec![0; size_of::<InputHeader>()];
        assert_eq!(
            compile_serialized_buffer(&input, &mut []).unwrap_err(),
            "input buffer size mismatch"
        );
    }

    #[test]
    fn rejects_function_count_larger_than_the_input() {
        let header = InputHeader {
            function_count: u32::MAX,
            layout_offset: 0,
            outcome_return: 0,
            output_size: 0,
            total_size: size_of::<InputHeader>() as u64,
        };
        let input = as_bytes_slice(std::slice::from_ref(&header));
        assert_eq!(
            compile_serialized_buffer(input, &mut []).unwrap_err(),
            "input entries are truncated"
        );
    }

    #[test]
    fn skips_only_compiled_functions_that_do_not_fit() {
        let compiled = vec![vec![
            (
                0,
                CompiledFunction {
                    code: vec![0; 16],
                    relocs: vec![],
                    traps: vec![],
                },
            ),
            (
                1,
                CompiledFunction {
                    code: vec![0; 64],
                    relocs: vec![],
                    traps: vec![],
                },
            ),
            (
                2,
                CompiledFunction {
                    code: vec![0; 16],
                    relocs: vec![],
                    traps: vec![],
                },
            ),
        ]];

        let (selected, code_size, reloc_size) = select_compiled_functions(compiled, 32, 64);
        assert_eq!(selected.iter().map(|(index, _)| *index).collect::<Vec<_>>(), vec![0, 2]);
        assert_eq!(code_size, 32);
        assert_eq!(reloc_size, 0);
    }
}
