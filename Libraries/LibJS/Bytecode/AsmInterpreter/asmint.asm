# AsmInterpreter DSL source
# Each handler implements one bytecode instruction.
# Instructions not listed here fall through to the C++ fallback handler.
#
# For the full DSL instruction reference, see AsmIntGen/src/main.rs.
#
# Register conventions (pinned in callee-saved regs, survive C++ calls):
#   pc       = program counter (byte offset into bytecode, 32-bit)
#   pb       = bytecode base pointer (u8 const*)
#   values   = pointer to Value[] array (registers+constants+locals+arguments)
#   exec_ctx = running ExecutionContext*
#   dispatch = dispatch table base pointer (256 entries, 8 bytes each)
#
# Temporary registers are declared by name with `temp` (GPR) and
# `ftemp` (FPR) inside each handler or macro; the register allocator
# assigns them to physical registers from the platform's caller-saved
# pool. None of the temps survive C++ calls (`call_slow_path`,
# `call_helper`, `call_interp`, `call_raw_native`).
#
# NaN-boxing encoding:
#   Every JS Value is a 64-bit NaN-boxed value. The upper 16 bits encode the type tag.
#   If tag == 0x0000..0xFFF8, the value is a double (IEEE 754 double-precision).
#   Otherwise, the tag identifies the type:
#     INT32_TAG (0xFFF9)  - Lower 32 bits are a sign-extended int32
#     BOOLEAN_TAG         - Bit 0 = true/false
#     UNDEFINED_TAG       - undefined (0x7FFE)
#     NULL_TAG            - null (0x7FFF)
#     OBJECT_TAG          - Lower 48 bits are a sign-extended Object*
#     STRING_TAG          - Lower 48 bits are a sign-extended PrimitiveString*
#     SYMBOL_TAG          - Lower 48 bits are a sign-extended Symbol*
#     BIGINT_TAG          - Lower 48 bits are a sign-extended BigInt*
#     ACCESSOR_TAG        - Internal accessor marker
#     EMPTY_TAG           - Internal empty/hole marker (used for TDZ)
#   A double whose exponent+mantissa bits match CANON_NAN_BITS would collide
#   with the tag space, so all NaN values are canonicalized to CANON_NAN_BITS.
#   The *_SHIFTED constants (e.g. INT32_TAG_SHIFTED, UNDEFINED_SHIFTED) are
#   the tag shifted left by 48 bits, used for quick full-value comparisons.
#
# Instruction field references:
#   Inside a handler, m_fieldname (e.g. m_dst, m_src, m_lhs) resolves to the
#   byte offset of that field within the current handler's bytecode instruction.
#   Field offsets are computed from Bytecode.def by the asmintgen compiler.
#
# Handler structure:
#   Each handler follows a common pattern:
#   1. Load operands (load_operand)
#   2. Type-check via tag extraction (extract_tag + branch)
#   3. Fast path for int32 and/or double
#   4. Slow path fallback to C++ (call_slow_path)
#   call_slow_path is TERMINAL: control does not return to the handler.
#   call_helper and call_interp are NON-TERMINAL: the handler continues after.


# NOTE: extract_tag, unbox_int32, unbox_object, box_int32, and
# box_int32_clean are codegen instructions (not macros), allowing each
# backend to emit optimal platform-specific code.
#
# extract_tag dst, src       -- Extract upper 16-bit NaN-boxing tag.
# unbox_int32 dst, src       -- Sign-extend low 32 bits to 64.
# unbox_object dst, src      -- Zero-extend lower 48 bits (extract pointer).
# box_int32 dst, src         -- NaN-box a raw int32 (masks low 32, sets tag).
# box_int32_clean dst, src   -- NaN-box an already zero-extended int32.

# Check if a value is a double (not a NaN-boxed tagged value).
# All tagged types have (tag & NAN_BASE_TAG) == NAN_BASE_TAG in their upper 16 bits.
# Jumps to fail if not a double.
macro check_is_double(reg, fail)
    temp tag
    extract_tag tag, reg
    and tag, NAN_BASE_TAG
    branch_eq tag, NAN_BASE_TAG, fail
end

# Check if an already-extracted tag represents a non-double type.
# Clobbers the tag register. Jumps to fail if not a double.
macro check_tag_is_double(tag, fail)
    and tag, NAN_BASE_TAG
    branch_eq tag, NAN_BASE_TAG, fail
end

# Check if both values are doubles.
# Jumps to fail if either is not a number tag.
macro check_both_double(lhs, rhs, fail)
    temp lhs_tag, rhs_tag
    extract_tag lhs_tag, lhs
    and lhs_tag, NAN_BASE_TAG
    branch_eq lhs_tag, NAN_BASE_TAG, fail
    extract_tag rhs_tag, rhs
    and rhs_tag, NAN_BASE_TAG
    branch_eq rhs_tag, NAN_BASE_TAG, fail
end

# Coerce two operands to numeric types for arithmetic / comparison.
# If both are int32: jumps to both_int_label with lhs_int / rhs_int holding
# the sign-extended 64-bit ints.
# If one or both are double: falls through with lhs_dbl / rhs_dbl holding
# the values as doubles.
# If either is not a number (int32 or double): jumps to fail.
macro coerce_to_doubles(lhs, rhs, lhs_int, rhs_int, lhs_dbl, rhs_dbl, both_int_label, fail)
    temp tag
    extract_tag tag, lhs
    branch_ne tag, INT32_TAG, .lhs_not_int
    extract_tag tag, rhs
    branch_ne tag, INT32_TAG, .int_rhs_maybe_double
    # Both int32
    unbox_int32 lhs_int, lhs
    unbox_int32 rhs_int, rhs
    jmp both_int_label
.int_rhs_maybe_double:
    # tag already has rhs tag, known != INT32_TAG
    check_tag_is_double tag, fail
    unbox_int32 lhs_int, lhs
    int_to_double lhs_dbl, lhs_int
    fp_mov rhs_dbl, rhs
    jmp .coerced
.lhs_not_int:
    # tag already has lhs tag, known != INT32_TAG
    check_tag_is_double tag, fail
    extract_tag tag, rhs
    branch_eq tag, INT32_TAG, .double_rhs_int
    # tag already has rhs tag, known != INT32_TAG
    check_tag_is_double tag, fail
    fp_mov lhs_dbl, lhs
    fp_mov rhs_dbl, rhs
    jmp .coerced
.double_rhs_int:
    fp_mov lhs_dbl, lhs
    unbox_int32 rhs_int, rhs
    int_to_double rhs_dbl, rhs_int
.coerced:
end

# NOTE: canonicalize_nan is a codegen instruction, not a macro.
# canonicalize_nan dst_gpr, src_fpr
# If src_fpr is NaN, writes CANON_NAN_BITS to dst_gpr.
# Otherwise bitwise-copies src_fpr to dst_gpr.

# Box a double result as a JS::Value, preferring Int32 when the double is a
# whole number in [INT32_MIN, INT32_MAX] and not -0.0. This mirrors the
# JS::Value(double) constructor so that downstream int32 fast paths fire.
# dst: destination GPR for the boxed value.
# src_fpr: source FPR containing the double result.
# The macro uses a macro-local temp; the allocator will pick a register
# that doesn't conflict with the caller's live values.
macro box_double_or_int32(dst, src_fpr)
    temp int_value
    double_to_int32 int_value, src_fpr, .bdi_not_int
    branch_zero int_value, .bdi_check_neg_zero
    box_int32 dst, int_value
    jmp .bdi_done
.bdi_check_neg_zero:
    fp_mov dst, src_fpr
    branch_negative dst, .bdi_not_int
    box_int32 dst, int_value
    jmp .bdi_done
.bdi_not_int:
    canonicalize_nan dst, src_fpr
.bdi_done:
end

# Shared same-tag equality dispatch.
# lhs_tag is known equal to rhs's tag. For int32, boolean, object, symbol,
# undefined, null: bitwise compare. For string: pointer shortcut, else
# slow. For bigint: always slow. Falls through to .double_compare for
# doubles. The macro destroys lhs_tag (clobbers it during the
# undefined/null check).
macro equality_same_tag(lhs, rhs, lhs_tag, equal_label, not_equal_label, slow_label)
    branch_any_eq lhs_tag, INT32_TAG, BOOLEAN_TAG, .fast_compare
    branch_any_eq lhs_tag, OBJECT_TAG, SYMBOL_TAG, .fast_compare
    branch_eq lhs_tag, STRING_TAG, .string_compare
    branch_eq lhs_tag, BIGINT_TAG, slow_label
    # Check undefined/null: (tag & 0xFFFE) == UNDEFINED_TAG matches both.
    # Safe to clobber lhs_tag here since every other tagged type is
    # already routed.
    and lhs_tag, 0xFFFE
    branch_eq lhs_tag, UNDEFINED_TAG, .fast_compare
    # Must be a double
    jmp .double_compare
.string_compare:
    branch_eq lhs, rhs, equal_label
    jmp slow_label
.fast_compare:
    branch_eq lhs, rhs, equal_label
    jmp not_equal_label
end

# Compare lhs/rhs as doubles with NaN awareness.
# Defines .double_compare label (referenced by equality_same_tag).
macro double_equality_compare(lhs, rhs, equal_label, not_equal_label)
    ftemp lhs_dbl, rhs_dbl
.double_compare:
    fp_mov lhs_dbl, lhs
    fp_mov rhs_dbl, rhs
    branch_fp_unordered lhs_dbl, rhs_dbl, not_equal_label
    branch_fp_equal lhs_dbl, rhs_dbl, equal_label
    jmp not_equal_label
end

# Strict equality check core logic.
# Jumps to equal_label if definitely equal, not_equal_label if definitely
# not, or slow_label if we can't determine quickly. Handles: int32,
# boolean, undefined/null, object, symbol (bitwise compare), string
# (pointer shortcut), bigint (slow), doubles.
macro strict_equality_core(lhs, rhs, equal_label, not_equal_label, slow_label)
    temp lhs_tag, rhs_tag, lhs_int
    ftemp lhs_dbl, rhs_dbl
    extract_tag lhs_tag, lhs
    extract_tag rhs_tag, rhs
    branch_ne lhs_tag, rhs_tag, .diff_tags
    equality_same_tag lhs, rhs, lhs_tag, equal_label, not_equal_label, slow_label
    double_equality_compare lhs, rhs, equal_label, not_equal_label
.diff_tags:
    # Different tags but possibly equal: int32(1) === double(1.0) is true.
    # Handle int32 vs double inline; all other tag mismatches are not equal.
    branch_eq lhs_tag, INT32_TAG, .lhs_int32_diff
    branch_eq rhs_tag, INT32_TAG, .rhs_int32_diff
    # Neither is int32. If both are doubles, compare. Otherwise not equal.
    check_tag_is_double lhs_tag, not_equal_label
    check_tag_is_double rhs_tag, not_equal_label
    jmp .double_compare
.lhs_int32_diff:
    check_tag_is_double rhs_tag, not_equal_label
    unbox_int32 lhs_int, lhs
    int_to_double lhs_dbl, lhs_int
    fp_mov rhs_dbl, rhs
    branch_fp_equal lhs_dbl, rhs_dbl, equal_label
    jmp not_equal_label
.rhs_int32_diff:
    check_tag_is_double lhs_tag, not_equal_label
    fp_mov lhs_dbl, lhs
    unbox_int32 lhs_int, rhs
    int_to_double rhs_dbl, lhs_int
    branch_fp_equal lhs_dbl, rhs_dbl, equal_label
    jmp not_equal_label
end

# Loose equality check core logic.
# Same as strict_equality_core but with null==undefined cross-type handling.
macro loose_equality_core(lhs, rhs, equal_label, not_equal_label, slow_label)
    temp lhs_tag, rhs_tag
    extract_tag lhs_tag, lhs
    extract_tag rhs_tag, rhs
    branch_ne lhs_tag, rhs_tag, .diff_tags
    equality_same_tag lhs, rhs, lhs_tag, equal_label, not_equal_label, slow_label
    double_equality_compare lhs, rhs, equal_label, not_equal_label
.diff_tags:
    # null == undefined (and vice versa): (tag & 0xFFFE) == UNDEFINED_TAG
    and lhs_tag, 0xFFFE
    branch_ne lhs_tag, UNDEFINED_TAG, .try_double
    and rhs_tag, 0xFFFE
    branch_eq rhs_tag, UNDEFINED_TAG, equal_label
    jmp slow_label
.try_double:
    check_both_double lhs, rhs, slow_label
    jmp .double_compare
end

# Numeric compare with coercion (for jump variants).
# Uses coerce_to_doubles to handle mixed int32+double operands.
# int_cc: signed comparison branch for int32 (branch_lt_signed, etc.)
# double_cc: unsigned comparison branch for doubles (branch_fp_less, etc.)
# Jumps to true_label / false_label / slow_label.
macro numeric_compare_coerce(lhs, rhs, int_cc, double_cc, true_label, false_label, slow_label)
    temp lhs_int, rhs_int
    ftemp lhs_dbl, rhs_dbl
    coerce_to_doubles lhs, rhs, lhs_int, rhs_int, lhs_dbl, rhs_dbl, .both_int, slow_label
    branch_fp_unordered lhs_dbl, rhs_dbl, false_label
    double_cc lhs_dbl, rhs_dbl, true_label
    jmp false_label
.both_int:
    int_cc lhs_int, rhs_int, true_label
    jmp false_label
end

# Numeric compare without coercion (for non-jump variants).
# Only handles both-int32 or both-double fast paths.
macro numeric_compare(lhs, rhs, int_cc, double_cc, true_label, false_label, slow_label)
    temp tag, lhs_int, rhs_int
    ftemp lhs_dbl, rhs_dbl
    extract_tag tag, lhs
    branch_ne tag, INT32_TAG, .try_double
    extract_tag tag, rhs
    branch_ne tag, INT32_TAG, slow_label
    unbox_int32 lhs_int, lhs
    unbox_int32 rhs_int, rhs
    int_cc lhs_int, rhs_int, true_label
    jmp false_label
.try_double:
    # tag already has lhs tag
    check_tag_is_double tag, slow_label
    check_is_double rhs, slow_label
    fp_mov lhs_dbl, lhs
    fp_mov rhs_dbl, rhs
    branch_fp_unordered lhs_dbl, rhs_dbl, false_label
    double_cc lhs_dbl, rhs_dbl, true_label
    jmp false_label
end

# Epilogue for comparison/equality handlers that produce a boolean result.
# Defines .store_true, .store_false, and .slow labels.
macro boolean_result_epilogue(slow_path_func)
    temp result
.store_true:
    mov result, BOOLEAN_TRUE
    store_operand m_dst, result
    dispatch_next
.store_false:
    mov result, BOOLEAN_FALSE
    store_operand m_dst, result
    dispatch_next
.slow:
    call_slow_path slow_path_func
end

# Epilogue for jump comparison/equality handlers.
# Defines .take_true, .take_false, and .slow labels.
macro jump_binary_epilogue(slow_path_func)
    temp target
.slow:
    call_slow_path slow_path_func
.take_true:
    load_label target, m_true_target
    goto_handler target
.take_false:
    load_label target, m_false_target
    goto_handler target
end

# Coerce two operands to int32 for bitwise operations.
# On success: lhs_int / rhs_int hold the sign-extended int32 values.
# If either operand is not a number (int32, boolean, or double):
# jumps to fail.
macro coerce_to_int32s(lhs, rhs, lhs_int, rhs_int, fail)
    temp tag
    ftemp fp_scratch
    extract_tag tag, lhs
    branch_any_eq tag, INT32_TAG, BOOLEAN_TAG, .lhs_is_int
    check_tag_is_double tag, fail
    fp_mov fp_scratch, lhs
    js_to_int32 lhs_int, fp_scratch, fail
    jmp .lhs_done
.lhs_is_int:
    unbox_int32 lhs_int, lhs
.lhs_done:
    extract_tag tag, rhs
    branch_any_eq tag, INT32_TAG, BOOLEAN_TAG, .rhs_is_int
    check_tag_is_double tag, fail
    fp_mov fp_scratch, rhs
    js_to_int32 rhs_int, fp_scratch, fail
    jmp .rhs_done
.rhs_is_int:
    unbox_int32 rhs_int, rhs
.rhs_done:
end

# Fast path for bitwise binary operations on int32/boolean/double operands.
# op_insn: the bitwise instruction to apply (xor, and, or).
macro bitwise_op(op_insn, slow_path_func)
    temp lhs, rhs, lhs_int, rhs_int, dst
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    coerce_to_int32s lhs, rhs, lhs_int, rhs_int, .slow
    op_insn lhs_int, rhs_int
    box_int32 dst, lhs_int
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path slow_path_func
end

# Validate that the callee still points at the expected builtin function.
# Jumps to fail if the call target has been replaced or is not a function.
macro validate_callee_builtin(expected_builtin, fail)
    temp callee, scratch
    load_operand callee, m_callee
    extract_tag scratch, callee
    branch_ne scratch, OBJECT_TAG, fail
    unbox_object callee, callee
    load8 scratch, [callee, OBJECT_FLAGS]
    and scratch, OBJECT_FLAG_IS_FUNCTION
    branch_zero scratch, fail
    load8 scratch, [callee, FUNCTION_OBJECT_BUILTIN_HAS_VALUE]
    branch_zero scratch, fail
    load8 scratch, [callee, FUNCTION_OBJECT_BUILTIN_VALUE]
    branch_ne scratch, expected_builtin, fail
end

# Load a UTF-16 code unit from a primitive string with resident UTF-16 data.
# string is the PrimitiveString*; index is the non-negative code-unit
# index; code_unit is the destination GPR for the zero-extended result.
# Jumps to out_of_bounds if index >= string length.
# Jumps to fail if the string would require resolving deferred data.
macro load_primitive_string_utf16_code_unit(string, index, code_unit, out_of_bounds, fail)
    temp scratch, utf16_data
    load8 scratch, [string, PRIMITIVE_STRING_DEFERRED_KIND]
    branch_ne scratch, PRIMITIVE_STRING_DEFERRED_KIND_NONE, fail

    load64 utf16_data, [string, PRIMITIVE_STRING_UTF16_STRING]
    branch_zero utf16_data, fail

    load8 scratch, [string, PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG]
    and scratch, UTF16_SHORT_STRING_FLAG
    branch_zero scratch, .long_storage

    load8 scratch, [string, PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG]
    shr scratch, UTF16_SHORT_STRING_BYTE_COUNT_SHIFT_COUNT
    branch_ge_unsigned index, scratch, out_of_bounds
    mov code_unit, string
    add code_unit, PRIMITIVE_STRING_UTF16_SHORT_STRING_STORAGE
    load8 code_unit, [code_unit, index]
    jmp .done

.long_storage:
    load64 scratch, [utf16_data, UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS]
    branch_negative scratch, .utf16_storage
    branch_ge_unsigned index, scratch, out_of_bounds
    add utf16_data, UTF16_STRING_DATA_STRING_STORAGE
    load8 code_unit, [utf16_data, index]
    jmp .done

.utf16_storage:
    shl scratch, 1
    shr scratch, 1
    branch_ge_unsigned index, scratch, out_of_bounds
    mov code_unit, index
    add code_unit, index
    add utf16_data, UTF16_STRING_DATA_STRING_STORAGE
    load16 code_unit, [utf16_data, code_unit]

.done:
end

# Dispatch the instruction at current pc (without advancing).
# Clobbers t0.
macro dispatch_current()
    goto_handler pc
end

# Walk the environment chain using a cached EnvironmentCoordinate.
# Input: m_cache_field is the offset of the EnvironmentCoordinate inside
# the bytecode instruction.
# Output: target_env points at the resolved environment, bind_index holds
# the binding index within it.
# On failure (invalid cache, screwed by eval): jumps to fail_label.
macro walk_env_chain(m_cache_field, target_env, bind_index, fail_label)
    temp coord_addr, hops, sentinel, screw
    lea coord_addr, [pb, pc]
    add coord_addr, m_cache_field
    load_pair32 hops, bind_index, [coord_addr, ENVIRONMENT_COORDINATE_HOPS], [coord_addr, ENVIRONMENT_COORDINATE_INDEX]
    mov sentinel, ENVIRONMENT_COORDINATE_INVALID
    branch_eq hops, sentinel, fail_label
    load64 target_env, [exec_ctx, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT]
    assert_nonzero target_env
    branch_zero hops, .walk_done
.walk_loop:
    load8 screw, [target_env, ENVIRONMENT_SCREWED_BY_EVAL]
    branch_nonzero screw, fail_label
    load64 target_env, [target_env, ENVIRONMENT_OUTER]
    assert_nonzero target_env
    sub hops, 1
    branch_nonzero hops, .walk_loop
.walk_done:
    assert_nonzero target_env
    load8 screw, [target_env, ENVIRONMENT_SCREWED_BY_EVAL]
    branch_nonzero screw, fail_label
end

# Pop an inline frame and resume the caller without bouncing through C++.
# The asm-managed JS-to-JS call fast path currently only inlines Call, never
# CallConstruct, so caller_is_construct is always false for asm-managed inline
# frames.
#
# This mirrors VM::pop_inline_frame():
#   1. Read the caller's destination register from the callee frame.
#   2. Publish the caller's resume pc and returned value.
#   3. Deallocate the callee by rewinding InterpreterStack::top to exec_ctx.
#   4. Make the caller the running execution context again.
#   5. Advance execution_generation so WeakRef and similar observers still see
#      the same boundary they would have seen through the C++ helper.
#
# The macro expects exec_ctx/pb/values/pc to still describe the callee frame.
# Input:
#   caller_frame = ExecutionContext* of the caller
#   value_reg = NaN-boxed return value
macro pop_inline_frame_and_resume(caller_frame, value_reg)
    temp ret_pc, dst_idx, value_addr, vm, exe
    load_pair32 ret_pc, dst_idx, [exec_ctx, EXECUTION_CONTEXT_CALLER_RETURN_PC], [exec_ctx, EXECUTION_CONTEXT_CALLER_DST_RAW]
    store32 [caller_frame, EXECUTION_CONTEXT_PROGRAM_COUNTER], ret_pc
    lea value_addr, [caller_frame, SIZEOF_EXECUTION_CONTEXT]
    store64 [value_addr, dst_idx, 8], value_reg

    load_vm vm
    store64 [vm, VM_RUNNING_EXECUTION_CONTEXT], caller_frame
    store64 [vm, VM_INTERPRETER_STACK_TOP], exec_ctx
    inc32_mem [vm, VM_EXECUTION_GENERATION]

    mov exec_ctx, caller_frame
    load64 exe, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    assert_nonzero exe
    load64 pb, [exe, EXECUTABLE_BYTECODE_DATA]
    assert_nonzero pb
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    mov pc, ret_pc
    dispatch_current
end

# ============================================================================
# Simple data movement
# ============================================================================

handler Mov
    temp value
    load_operand value, m_src
    store_operand m_dst, value
    dispatch_next
end

handler Mov2
    temp v1, v2
    load_operand v1, m_src1
    store_operand m_dst1, v1
    load_operand v2, m_src2
    store_operand m_dst2, v2
    dispatch_next
end

handler Mov3
    temp v1, v2, v3
    load_operand v1, m_src1
    store_operand m_dst1, v1
    load_operand v2, m_src2
    store_operand m_dst2, v2
    load_operand v3, m_src3
    store_operand m_dst3, v3
    dispatch_next
end

# ============================================================================
# Arithmetic
# ============================================================================

# Arithmetic fast path: try int32, check overflow, fall back to double, then slow path.
# The coerce_to_doubles macro handles mixed int32+double coercion.
# On int32 overflow, we convert both operands to double and retry.
# box_double_or_int32 re-boxes double results as Int32 when possible,
# mirroring JS::Value(double), so downstream int32 fast paths can fire.
handler Add
    temp lhs, rhs, lhs_int, rhs_int, dst
    ftemp lhs_dbl, rhs_dbl
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    coerce_to_doubles lhs, rhs, lhs_int, rhs_int, lhs_dbl, rhs_dbl, .both_int, .slow
    fp_add lhs_dbl, rhs_dbl
    box_double_or_int32 dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.both_int:
    # 32-bit add with hardware overflow detection
    add32_overflow lhs_int, rhs_int, .overflow
    box_int32_clean dst, lhs_int
    store_operand m_dst, dst
    dispatch_next
.overflow:
    # Int32 overflow: convert both to double and redo the operation
    unbox_int32 lhs_int, lhs
    unbox_int32 rhs_int, rhs
    int_to_double lhs_dbl, lhs_int
    int_to_double rhs_dbl, rhs_int
    fp_add lhs_dbl, rhs_dbl
    fp_mov dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_add
end

# Same pattern as Add but with subtraction.
handler Sub
    temp lhs, rhs, lhs_int, rhs_int, dst
    ftemp lhs_dbl, rhs_dbl
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    coerce_to_doubles lhs, rhs, lhs_int, rhs_int, lhs_dbl, rhs_dbl, .both_int, .slow
    fp_sub lhs_dbl, rhs_dbl
    box_double_or_int32 dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.both_int:
    sub32_overflow lhs_int, rhs_int, .overflow
    box_int32_clean dst, lhs_int
    store_operand m_dst, dst
    dispatch_next
.overflow:
    unbox_int32 lhs_int, lhs
    unbox_int32 rhs_int, rhs
    int_to_double lhs_dbl, lhs_int
    int_to_double rhs_dbl, rhs_int
    fp_sub lhs_dbl, rhs_dbl
    fp_mov dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_sub
end

# Same pattern as Add but with multiplication.
# Extra complexity: 0 * negative = -0.0 (must produce negative zero double).
handler Mul
    temp lhs, rhs, lhs_int, rhs_int, dst, sign_check
    ftemp lhs_dbl, rhs_dbl
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    coerce_to_doubles lhs, rhs, lhs_int, rhs_int, lhs_dbl, rhs_dbl, .both_int, .slow
    fp_mul lhs_dbl, rhs_dbl
    box_double_or_int32 dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.both_int:
    mul32_overflow lhs_int, rhs_int, .overflow
    branch_nonzero lhs_int, .store_int
    # Result is 0: check if either operand was negative -> -0.0
    unbox_int32 sign_check, lhs
    or sign_check, rhs_int
    branch_negative sign_check, .negative_zero
.store_int:
    box_int32_clean dst, lhs_int
    store_operand m_dst, dst
    dispatch_next
.negative_zero:
    mov dst, NEGATIVE_ZERO
    store_operand m_dst, dst
    dispatch_next
.overflow:
    unbox_int32 lhs_int, lhs
    unbox_int32 rhs_int, rhs
    int_to_double lhs_dbl, lhs_int
    int_to_double rhs_dbl, rhs_int
    fp_mul lhs_dbl, rhs_dbl
    fp_mov dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_mul
end

# ============================================================================
# Control flow
# ============================================================================

handler Jump
    temp target
    load_label target, m_target
    goto_handler target
end

# Conditional jumps: check boolean first (most common), then int32, then slow path.
# For JumpIf/JumpTrue/JumpFalse, a boolean's truth value is just bit 0.
# For int32, any nonzero low 32 bits means truthy.
handler JumpIf
    temp condition, tag, truthy, target
    load_operand condition, m_condition
    extract_tag tag, condition
    # Boolean fast path
    branch_eq tag, BOOLEAN_TAG, .is_bool
    # Int32 fast path
    branch_eq tag, INT32_TAG, .is_int32
    # Slow path: call helper to convert to boolean
    call_helper asm_helper_to_boolean, condition, truthy
    branch_nonzero truthy, .take_true
    jmp .take_false
.is_bool:
    branch_bits_set condition, 1, .take_true
    jmp .take_false
.is_int32:
    branch_nonzero32 condition, .take_true
    jmp .take_false
.take_true:
    load_label target, m_true_target
    goto_handler target
.take_false:
    load_label target, m_false_target
    goto_handler target
end

handler JumpTrue
    temp condition, tag, truthy, target
    load_operand condition, m_condition
    extract_tag tag, condition
    branch_eq tag, BOOLEAN_TAG, .is_bool
    branch_eq tag, INT32_TAG, .is_int32
    call_helper asm_helper_to_boolean, condition, truthy
    branch_nonzero truthy, .take
    dispatch_next
.is_bool:
    branch_bits_set condition, 1, .take
    dispatch_next
.is_int32:
    branch_nonzero32 condition, .take
    dispatch_next
.take:
    load_label target, m_target
    goto_handler target
end

handler JumpFalse
    temp condition, tag, truthy, target
    load_operand condition, m_condition
    extract_tag tag, condition
    branch_eq tag, BOOLEAN_TAG, .is_bool
    branch_eq tag, INT32_TAG, .is_int32
    call_helper asm_helper_to_boolean, condition, truthy
    branch_zero truthy, .take
    dispatch_next
.is_bool:
    branch_bits_clear condition, 1, .take
    dispatch_next
.is_int32:
    branch_zero32 condition, .take
    dispatch_next
.take:
    load_label target, m_target
    goto_handler target
end

# Nullish check: undefined and null tags differ only in bit 0,
# so (tag & 0xFFFE) == UNDEFINED_TAG matches both.
handler JumpNullish
    temp condition, tag, target
    load_operand condition, m_condition
    # Nullish: (tag & 0xFFFE) == 0x7FFE (matches undefined=0x7FFE and null=0x7FFF)
    extract_tag tag, condition
    and tag, 0xFFFE
    branch_eq tag, UNDEFINED_TAG, .nullish
    load_label target, m_false_target
    goto_handler target
.nullish:
    load_label target, m_true_target
    goto_handler target
end

handler JumpUndefined
    temp condition, undef, target
    load_operand condition, m_condition
    mov undef, UNDEFINED_SHIFTED
    branch_eq condition, undef, .is_undefined
    load_label target, m_false_target
    goto_handler target
.is_undefined:
    load_label target, m_true_target
    goto_handler target
end


# Jump comparison handlers: use numeric_compare_coerce (handles mixed int32+double)
# combined with jump_binary_epilogue (provides .take_true, .take_false, .slow labels).
handler JumpLessThan
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare_coerce lhs, rhs, branch_lt_signed, branch_fp_less, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_less_than
end

handler JumpGreaterThan
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare_coerce lhs, rhs, branch_gt_signed, branch_fp_greater, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_greater_than
end

handler JumpLessThanEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare_coerce lhs, rhs, branch_le_signed, branch_fp_less_or_equal, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_less_than_equals
end

handler JumpGreaterThanEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare_coerce lhs, rhs, branch_ge_signed, branch_fp_greater_or_equal, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_greater_than_equals
end

handler JumpLooselyEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    loose_equality_core lhs, rhs, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_loosely_equals
end

handler JumpLooselyInequals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    loose_equality_core lhs, rhs, .take_false, .take_true, .slow
    jump_binary_epilogue asm_slow_path_jump_loosely_inequals
end

handler JumpStrictlyEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    strict_equality_core lhs, rhs, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_strictly_equals
end

handler JumpStrictlyInequals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    strict_equality_core lhs, rhs, .take_false, .take_true, .slow
    jump_binary_epilogue asm_slow_path_jump_strictly_inequals
end

# Fast path for ++x: int32 + 1 with overflow check.
# On overflow, convert to double and add 1.0.
handler Increment
    temp value, tag, int_value, dst
    ftemp result_dbl, one_dbl
    load_operand value, m_dst
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .slow
    unbox_int32 int_value, value
    add32_overflow int_value, 1, .overflow
    box_int32_clean dst, int_value
    store_operand m_dst, dst
    dispatch_next
.overflow:
    unbox_int32 int_value, value
    int_to_double result_dbl, int_value
    mov dst, DOUBLE_ONE
    fp_mov one_dbl, dst
    fp_add result_dbl, one_dbl
    fp_mov dst, result_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_increment
end

# Fast path for --x: int32 - 1 with overflow check.
handler Decrement
    temp value, tag, int_value, dst
    ftemp result_dbl, one_dbl
    load_operand value, m_dst
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .slow
    unbox_int32 int_value, value
    sub32_overflow int_value, 1, .overflow
    box_int32_clean dst, int_value
    store_operand m_dst, dst
    dispatch_next
.overflow:
    unbox_int32 int_value, value
    int_to_double result_dbl, int_value
    mov dst, DOUBLE_ONE
    fp_mov one_dbl, dst
    fp_sub result_dbl, one_dbl
    fp_mov dst, result_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_decrement
end

handler Not
    temp value, tag, nullish_check, truthy, result
    load_operand value, m_src
    extract_tag tag, value
    # Boolean fast path
    branch_eq tag, BOOLEAN_TAG, .is_bool
    # Int32 fast path
    branch_eq tag, INT32_TAG, .is_int32
    # Undefined/null -> !nullish = true
    mov nullish_check, tag
    and nullish_check, 0xFFFE
    branch_eq nullish_check, UNDEFINED_TAG, .store_true
    # Slow path for remaining types (object, string, etc)
    # NB: Objects go through slow path to handle [[IsHTMLDDA]]
    call_helper asm_helper_to_boolean, value, truthy
    branch_zero truthy, .store_true
    jmp .store_false
.is_bool:
    branch_bits_clear value, 1, .store_true
    jmp .store_false
.is_int32:
    branch_zero32 value, .store_true
    jmp .store_false
.store_true:
    mov result, BOOLEAN_TRUE
    store_operand m_dst, result
    dispatch_next
.store_false:
    mov result, BOOLEAN_FALSE
    store_operand m_dst, result
    dispatch_next
end

# ============================================================================
# Return / function call
# ============================================================================

handler Return
    # Empty is the internal "no explicit value" marker. Returning it from
    # bytecode means "return undefined" at the JS level.
    temp value, empty_tag, caller_frame
    load_operand value, m_value
    mov empty_tag, EMPTY_TAG_SHIFTED
    branch_ne value, empty_tag, .value_ready
    mov value, UNDEFINED_SHIFTED
.value_ready:
    # Inline JS-to-JS calls resume the caller directly from asm. Top-level
    # returns instead exit back to the outer interpreter entry point.
    load64 caller_frame, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    branch_zero caller_frame, .top_level
    pop_inline_frame_and_resume caller_frame, value
.top_level:
    # Top-level return matches VM::run_executable(): write return_value,
    # clear the exception slot, and leave the asm interpreter entirely.
    # values[3] = return_value, values[1] = empty (clear exception)
    store64 [values, 24], value
    store64 [values, 8], empty_tag
    exit
end

# Like Return, but does not clear the exception register (values[1]).
# Used at the end of a function body (after all user code).
handler End
    # End shares the same inline-frame unwind logic as Return. The only
    # top-level difference is that End preserves the current exception slot.
    temp value, empty_tag, caller_frame
    load_operand value, m_value
    mov empty_tag, EMPTY_TAG_SHIFTED
    branch_ne value, empty_tag, .value_ready
    mov value, UNDEFINED_SHIFTED
.value_ready:
    # Inline frame: resume the caller immediately.
    load64 caller_frame, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    branch_zero caller_frame, .top_level
    pop_inline_frame_and_resume caller_frame, value
.top_level:
    # Top-level end: publish the return value and exit without touching
    # values[1], since End does not model a user-visible `return` opcode.
    store64 [values, 24], value
    exit
end

# Loads running_execution_context().lexical_environment into dst,
# NaN-boxed as a cell pointer.
handler GetLexicalEnvironment
    temp env, tag
    load64 env, [exec_ctx, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT]
    assert_nonzero env
    mov tag, CELL_TAG_SHIFTED
    or env, tag
    store_operand m_dst, env
    dispatch_next
end

handler SetLexicalEnvironment
    call_slow_path asm_slow_path_set_lexical_environment
end

# ============================================================================
# Environment / binding access
# ============================================================================

# Inline environment chain walk + binding value load with TDZ check.
handler GetBinding
    temp env, idx, binding, init, value
    walk_env_chain m_cache, env, idx, .slow
    load64 binding, [env, BINDINGS_DATA_PTR]
    assert_nonzero binding
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    # Check binding is initialized (TDZ)
    load8 init, [binding, BINDING_INITIALIZED]
    branch_zero init, .slow
    load64 value, [binding, BINDING_VALUE]
    store_operand m_dst, value
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_binding
end

# Inline environment chain walk + direct binding value load.
handler GetInitializedBinding
    temp env, idx, binding, value
    walk_env_chain m_cache, env, idx, .slow
    load64 binding, [env, BINDINGS_DATA_PTR]
    assert_nonzero binding
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    load64 value, [binding, BINDING_VALUE]
    store_operand m_dst, value
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_initialized_binding
end

# Inline environment chain walk + initialize binding (set value + initialized=true).
handler InitializeLexicalBinding
    temp env, idx, binding, value, one
    walk_env_chain m_cache, env, idx, .slow
    load64 binding, [env, BINDINGS_DATA_PTR]
    assert_nonzero binding
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    load_operand value, m_src
    store64 [binding, BINDING_VALUE], value
    # Set initialized = true
    mov one, 1
    store8 [binding, BINDING_INITIALIZED], one
    dispatch_next
.slow:
    call_slow_path asm_slow_path_initialize_lexical_binding
end

# Inline environment chain walk + set mutable binding.
handler SetLexicalBinding
    temp env, idx, binding, flag, value
    walk_env_chain m_cache, env, idx, .slow
    load64 binding, [env, BINDINGS_DATA_PTR]
    assert_nonzero binding
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    # Check initialized (TDZ)
    load8 flag, [binding, BINDING_INITIALIZED]
    branch_zero flag, .slow
    # Check mutable
    load8 flag, [binding, BINDING_MUTABLE]
    branch_zero flag, .slow
    load_operand value, m_src
    store64 [binding, BINDING_VALUE], value
    dispatch_next
.slow:
    call_slow_path asm_slow_path_set_lexical_binding
end

# x++: save original to dst first, then increment src in-place.
handler PostfixIncrement
    temp value, tag, int_value, dst
    ftemp result_dbl, one_dbl
    load_operand value, m_src
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .slow
    # Save original value to dst (the "postfix" part)
    store_operand m_dst, value
    # Increment in-place: src = src + 1
    unbox_int32 int_value, value
    add32_overflow int_value, 1, .overflow_after_store
    box_int32_clean dst, int_value
    store_operand m_src, dst
    dispatch_next
.overflow_after_store:
    unbox_int32 int_value, value
    int_to_double result_dbl, int_value
    mov dst, DOUBLE_ONE
    fp_mov one_dbl, dst
    fp_add result_dbl, one_dbl
    fp_mov dst, result_dbl
    store_operand m_src, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_postfix_increment
end

# Division result is stored as int32 when representable (e.g. 6/3 = 2),
# matching the Value(double) constructor's behavior. We don't use
# coerce_to_doubles here because we never need the both-int32 branch --
# both operands go straight to FP regs.
handler Div
    temp lhs, rhs, tag, scratch_int, dst
    ftemp lhs_dbl, rhs_dbl
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    extract_tag tag, lhs
    branch_eq tag, INT32_TAG, .lhs_is_int32
    # tag already has lhs tag
    check_tag_is_double tag, .slow
    fp_mov lhs_dbl, lhs
    jmp .lhs_ok
.lhs_is_int32:
    unbox_int32 scratch_int, lhs
    int_to_double lhs_dbl, scratch_int
.lhs_ok:
    extract_tag tag, rhs
    branch_eq tag, INT32_TAG, .rhs_is_int32
    # tag already has rhs tag
    check_tag_is_double tag, .slow
    fp_mov rhs_dbl, rhs
    jmp .do_div
.rhs_is_int32:
    unbox_int32 scratch_int, rhs
    int_to_double rhs_dbl, scratch_int
.do_div:
    fp_div lhs_dbl, rhs_dbl
    # Try to store result as int32 if it's an integer in i32 range.
    # NB: We can't use js_to_int32 here because fjcvtzs applies modular
    # reduction (e.g. 2^33 -> 0) which is wrong -- we need a strict
    # round-trip check: truncate to int32, convert back, compare.
    double_to_int32 dst, lhs_dbl, .store_double
    # Exclude negative zero: -0.0 truncates to 0 but must stay double.
    branch_nonzero dst, .store_int
    fp_mov dst, lhs_dbl
    branch_negative dst, .store_double
.store_int:
    box_int32 dst, dst
    store_operand m_dst, dst
    dispatch_next
.store_double:
    canonicalize_nan dst, lhs_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_div
end

# Numeric comparison handlers: use numeric_compare macro for both-int32 and
# both-double fast paths, fall back to slow path for mixed/non-numeric types.
# The boolean_result_epilogue macro provides .store_true, .store_false, .slow labels.
handler LessThan
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare lhs, rhs, branch_lt_signed, branch_fp_less, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_less_than
end

handler LessThanEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare lhs, rhs, branch_le_signed, branch_fp_less_or_equal, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_less_than_equals
end

handler GreaterThan
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare lhs, rhs, branch_gt_signed, branch_fp_greater, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_greater_than
end

handler GreaterThanEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    numeric_compare lhs, rhs, branch_ge_signed, branch_fp_greater_or_equal, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_greater_than_equals
end

# Bitwise ops accept int32 and boolean, fall back to slow path for anything else.
handler BitwiseXor
    bitwise_op xor, asm_slow_path_bitwise_xor
end

# ============================================================================
# Unary operators
# ============================================================================

# Fast path for numeric values: +x is a no-op for int32 and double.
handler UnaryPlus
    temp value, tag
    load_operand value, m_src
    # Check if int32
    extract_tag tag, value
    branch_eq tag, INT32_TAG, .done
    # tag already holds value's tag; check if double
    check_tag_is_double tag, .slow
.done:
    store_operand m_dst, value
    dispatch_next
.slow:
    call_slow_path asm_slow_path_unary_plus
end

# Check if value is TDZ (empty). If not, just continue.
handler ThrowIfTDZ
    temp value, empty
    load_operand value, m_src
    mov empty, EMPTY_VALUE
    branch_eq value, empty, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_throw_if_tdz
end

# Check if value is an object. Only throws on non-object (rare).
handler ThrowIfNotObject
    temp value, tag
    load_operand value, m_src
    extract_tag tag, value
    branch_ne tag, OBJECT_TAG, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_throw_if_not_object
end

# Check if value is nullish (undefined or null). Only throws on nullish (rare).
handler ThrowIfNullish
    temp value, tag
    load_operand value, m_src
    extract_tag tag, value
    and tag, 0xFFFE
    branch_eq tag, UNDEFINED_TAG, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_throw_if_nullish
end

# Fast path for int32: ~value
handler BitwiseNot
    temp value, tag, dst
    load_operand value, m_src
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .slow
    # NOT the low 32 bits (not32 zeros upper 32), then re-box
    mov dst, value
    not32 dst
    box_int32_clean dst, dst
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_bitwise_not
end

handler BitwiseAnd
    bitwise_op and, asm_slow_path_bitwise_and
end

handler BitwiseOr
    bitwise_op or, asm_slow_path_bitwise_or
end

# Shift ops: int32-only fast path, shift count masked to 0-31 per spec.
handler LeftShift
    temp lhs, rhs, lhs_tag, rhs_tag, lhs_int, count, dst
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    extract_tag lhs_tag, lhs
    branch_ne lhs_tag, INT32_TAG, .slow
    extract_tag rhs_tag, rhs
    branch_ne rhs_tag, INT32_TAG, .slow
    unbox_int32 lhs_int, lhs
    unbox_int32 count, rhs
    and count, 31
    shl lhs_int, count
    box_int32 dst, lhs_int
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_left_shift
end

handler RightShift
    temp lhs, rhs, lhs_tag, rhs_tag, lhs_int, count, dst
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    extract_tag lhs_tag, lhs
    branch_ne lhs_tag, INT32_TAG, .slow
    extract_tag rhs_tag, rhs
    branch_ne rhs_tag, INT32_TAG, .slow
    unbox_int32 lhs_int, lhs
    unbox_int32 count, rhs
    and count, 31
    sar lhs_int, count
    box_int32 dst, lhs_int
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_right_shift
end

# Unsigned right shift: result is always unsigned, so values > INT32_MAX
# must be stored as double (can't fit in a signed int32 NaN-box).
handler UnsignedRightShift
    temp lhs, rhs, lhs_tag, rhs_tag, value, count, dst
    ftemp dst_dbl
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    extract_tag lhs_tag, lhs
    branch_ne lhs_tag, INT32_TAG, .slow
    extract_tag rhs_tag, rhs
    branch_ne rhs_tag, INT32_TAG, .slow
    # u32 result = (u32)lhs >> (rhs % 32)
    mov value, lhs
    and value, 0xFFFFFFFF
    unbox_int32 count, rhs
    and count, 31
    shr value, count
    # If result > INT32_MAX, store as double
    branch_bit_set value, 31, .as_double
    box_int32_clean dst, value
    store_operand m_dst, dst
    dispatch_next
.as_double:
    int_to_double dst_dbl, value
    fp_mov dst, dst_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_unsigned_right_shift
end

# Modulo: int32-only fast path for non-negative dividend.
# Negative dividend falls to slow path to handle -0 and INT_MIN correctly.
handler Mod
    temp lhs, rhs, lhs_tag, rhs_tag, lhs_int, rhs_int, quot, rem, dst
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    extract_tag lhs_tag, lhs
    branch_ne lhs_tag, INT32_TAG, .slow
    extract_tag rhs_tag, rhs
    branch_ne rhs_tag, INT32_TAG, .slow
    unbox_int32 lhs_int, lhs
    unbox_int32 rhs_int, rhs
    # Check d == 0
    branch_zero rhs_int, .slow
    # Check n >= 0 (positive fast path avoids INT_MIN/-1 and negative zero)
    branch_negative lhs_int, .slow
    divmod quot, rem, lhs_int, rhs_int
    box_int32 dst, rem
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_mod
end

# ============================================================================
# Equality and comparison
# ============================================================================

# Equality handlers use the strict/loose_equality_core macros which handle
# type-specific comparisons (int32, boolean, undefined/null bitwise compare,
# double with NaN awareness, string pointer shortcut, bigint -> slow path).
handler StrictlyEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    strict_equality_core lhs, rhs, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_strictly_equals
end

handler StrictlyInequals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    strict_equality_core lhs, rhs, .store_false, .store_true, .slow
    boolean_result_epilogue asm_slow_path_strictly_inequals
end

# Inline environment chain walk + get callee and this.
handler GetCalleeAndThisFromEnvironment
    temp env, idx, binding, init, value
    walk_env_chain m_cache, env, idx, .slow
    load64 binding, [env, BINDINGS_DATA_PTR]
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    # TDZ state lives in Binding.initialized; the value slot itself starts as
    # undefined, so checking for EMPTY would miss cached second-hit calls.
    load8 init, [binding, BINDING_INITIALIZED]
    branch_zero init, .slow
    load64 value, [binding, BINDING_VALUE]
    store_operand m_callee, value
    # this = undefined (DeclarativeEnvironment.with_base_object() always returns nullptr)
    mov value, UNDEFINED_SHIFTED
    store_operand m_this_value, value
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_callee_and_this
end

handler LooselyEquals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    loose_equality_core lhs, rhs, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_loosely_equals
end

handler LooselyInequals
    temp lhs, rhs
    load_operand lhs, m_lhs
    load_operand rhs, m_rhs
    loose_equality_core lhs, rhs, .store_false, .store_true, .slow
    boolean_result_epilogue asm_slow_path_loosely_inequals
end

handler UnaryMinus
    temp value, tag, int_value, dst
    ftemp dst_dbl
    load_operand value, m_src
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .try_double
    unbox_int32 int_value, value
    # -0 check: if value is 0, result is -0.0 (double)
    branch_zero int_value, .negative_zero
    # 32-bit negate with overflow detection (INT32_MIN)
    neg32_overflow int_value, .overflow
    box_int32_clean dst, int_value
    store_operand m_dst, dst
    dispatch_next
.negative_zero:
    mov dst, NEGATIVE_ZERO
    store_operand m_dst, dst
    dispatch_next
.overflow:
    # INT32_MIN: -(-2147483648) = 2147483648.0
    int_to_double dst_dbl, int_value
    fp_mov dst, dst_dbl
    store_operand m_dst, dst
    dispatch_next
.try_double:
    # tag already has the lhs tag
    check_tag_is_double tag, .slow
    # Negate double: flip sign bit (bit 63)
    toggle_bit value, 63
    store_operand m_dst, value
    dispatch_next
.slow:
    call_slow_path asm_slow_path_unary_minus
end

# x--: save original to dst first, then decrement src in-place.
handler PostfixDecrement
    temp value, tag, int_value, dst
    ftemp result_dbl, one_dbl
    load_operand value, m_src
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .slow
    # Save original value to dst (the "postfix" part)
    store_operand m_dst, value
    # Decrement in-place: src = src - 1
    unbox_int32 int_value, value
    sub32_overflow int_value, 1, .overflow_after_store
    box_int32_clean dst, int_value
    store_operand m_src, dst
    dispatch_next
.overflow_after_store:
    unbox_int32 int_value, value
    int_to_double result_dbl, int_value
    mov dst, DOUBLE_ONE
    fp_mov one_dbl, dst
    fp_sub result_dbl, one_dbl
    fp_mov dst, result_dbl
    store_operand m_src, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_postfix_decrement
end

handler ToInt32
    temp value, tag, tag_copy, dst
    ftemp value_dbl
    load_operand value, m_value
    extract_tag tag, value
    branch_ne tag, INT32_TAG, .try_double
    # Already int32, just copy
    store_operand m_dst, value
    dispatch_next
.try_double:
    # `tag` already has the value's tag; check if double (copy first because
    # check_tag_is_double clobbers its argument and `tag` is needed again
    # at .try_boolean for the boolean check).
    mov tag_copy, tag
    check_tag_is_double tag_copy, .try_boolean
    # Convert double to int32 using JS ToInt32 semantics.
    # With FEAT_JSCVT: fjcvtzs handles everything in one instruction.
    # Without: truncate + round-trip check, slow path on mismatch.
    fp_mov value_dbl, value
    js_to_int32 dst, value_dbl, .slow
    box_int32_clean dst, dst
    store_operand m_dst, dst
    dispatch_next
.try_boolean:
    branch_ne tag, BOOLEAN_TAG, .slow
    # Convert boolean to int32: false -> 0, true -> 1
    and value, 1
    box_int32_clean dst, value
    store_operand m_dst, dst
    dispatch_next
.slow:
    # Slow path handles other types (string, object, nullish, etc) and uncommon cases.
    call_slow_path asm_slow_path_to_int32
end

# ============================================================================
# Property access (indexed + named + inline caches)
# ============================================================================

# Fast path for array[int32_index] = value with Packed/Holey indexed storage.
handler PutByValue
    temp kind, base, prop, base_tag, prop_tag, index, obj, flags, storage_kind, size, elements, src, capacity_addr, capacity, slot, empty_tag, kind_byte, addr, src_int32, max, result
    ftemp src_dbl
    # Only fast-path Normal puts (not Getter/Setter/Own)
    load8 kind, [pb, pc, m_kind]
    branch_ne kind, PUT_KIND_NORMAL, .slow
    load_operand base, m_base
    load_operand prop, m_property
    extract_tag base_tag, base
    branch_ne base_tag, OBJECT_TAG, .slow
    # Check property is non-negative int32
    extract_tag prop_tag, prop
    branch_ne prop_tag, INT32_TAG, .slow
    mov index, prop
    and index, 0xFFFFFFFF
    branch_bit_set index, 31, .slow
    unbox_object obj, base
    # Check IsTypedArray flag -- branch to typed-array path early.
    load8 flags, [obj, OBJECT_FLAGS]
    branch_bits_set flags, OBJECT_FLAG_IS_TYPED_ARRAY, .try_typed_array
    # Check !may_interfere_with_indexed_property_access
    branch_bits_set flags, OBJECT_FLAG_MAY_INTERFERE, .slow
    # Packed is the hot path: existing elements can be overwritten directly.
    load8 storage_kind, [obj, OBJECT_INDEXED_STORAGE_KIND]
    branch_ne storage_kind, INDEXED_STORAGE_KIND_PACKED, .not_packed
    load32 size, [obj, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned index, size, .slow
    load64 elements, [obj, OBJECT_INDEXED_ELEMENTS]
    assert_nonzero elements
    load_operand src, m_src
    store64 [elements, index, 8], src
    dispatch_next
.not_packed:
    branch_ne storage_kind, INDEXED_STORAGE_KIND_HOLEY, .slow
    # Holey arrays need a slot load to distinguish existing elements from holes.
    load32 size, [obj, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned index, size, .slow
    load64 elements, [obj, OBJECT_INDEXED_ELEMENTS]
    branch_zero elements, .try_holey_array_slow
    mov capacity_addr, elements
    sub capacity_addr, 8
    load32 capacity, [capacity_addr, 0]
    branch_ge_unsigned index, capacity, .try_holey_array_slow
    load64 slot, [elements, index, 8]
    mov empty_tag, EMPTY_TAG_SHIFTED
    branch_eq slot, empty_tag, .try_holey_array_slow
    load_operand src, m_src
    store64 [elements, index, 8], src
    dispatch_next
.try_holey_array_slow:
    call_interp asm_try_put_by_value_holey_array, result
    branch_nonzero result, .slow
    dispatch_next
.try_typed_array:
    # Load cached data pointer (pre-computed: buffer.data() + byte_offset)
    # nullptr means uncached -> C++ helper will resolve the access.
    load64 elements, [obj, TYPED_ARRAY_CACHED_DATA_PTR]
    branch_zero elements, .try_typed_array_slow
    # Cached pointers only exist for fixed-length typed arrays, so array_length
    # is known to hold a concrete u32 value here.
    load32 capacity, [obj, TYPED_ARRAY_ARRAY_LENGTH_VALUE]
    branch_ge_unsigned index, capacity, .slow
    load8 kind_byte, [obj, TYPED_ARRAY_KIND]
    load_operand src, m_src
    extract_tag base_tag, src
    branch_eq base_tag, INT32_TAG, .ta_store_int32
    # Non-int32 value: only handle float typed arrays with double sources.
    branch_eq kind_byte, TYPED_ARRAY_KIND_FLOAT32, .ta_store_float32
    branch_ne kind_byte, TYPED_ARRAY_KIND_FLOAT64, .try_typed_array_slow
    # Compute store address before check_is_double mangles its argument.
    mov addr, index
    shl addr, 3
    add addr, elements
    check_is_double src, .try_typed_array_slow
    store64 [addr, 0], src
    dispatch_next
.ta_store_float32:
    mov addr, index
    shl addr, 2
    add addr, elements
    check_is_double src, .try_typed_array_slow
    fp_mov src_dbl, src
    double_to_float src_dbl, src_dbl
    storef32 [addr, 0], src_dbl
    dispatch_next
.ta_store_int32:
    unbox_int32 src_int32, src
    branch_any_eq kind_byte, TYPED_ARRAY_KIND_INT32, TYPED_ARRAY_KIND_UINT32, .ta_put_int32
    branch_eq kind_byte, TYPED_ARRAY_KIND_FLOAT32, .ta_put_float32
    branch_eq kind_byte, TYPED_ARRAY_KIND_UINT8_CLAMPED, .ta_put_uint8_clamped
    branch_any_eq kind_byte, TYPED_ARRAY_KIND_UINT8, TYPED_ARRAY_KIND_INT8, .ta_put_uint8
    branch_any_eq kind_byte, TYPED_ARRAY_KIND_UINT16, TYPED_ARRAY_KIND_INT16, .ta_put_uint16
    jmp .try_typed_array_slow
.ta_put_int32:
    store32 [elements, index, 4], src_int32
    dispatch_next
.ta_put_float32:
    int_to_double src_dbl, src_int32
    double_to_float src_dbl, src_dbl
    mov addr, index
    shl addr, 2
    add addr, elements
    storef32 [addr, 0], src_dbl
    dispatch_next
.ta_put_uint8_clamped:
    branch_negative src_int32, .ta_put_uint8_clamped_zero
    mov max, 255
    branch_ge_unsigned src_int32, max, .ta_put_uint8_clamped_max
    store8 [elements, index], src_int32
    dispatch_next
.ta_put_uint8_clamped_zero:
    mov src_int32, 0
    store8 [elements, index], src_int32
    dispatch_next
.ta_put_uint8_clamped_max:
    mov src_int32, 255
    store8 [elements, index], src_int32
    dispatch_next
.ta_put_uint8:
    store8 [elements, index], src_int32
    dispatch_next
.ta_put_uint16:
    mov addr, index
    add addr, index
    add addr, elements
    store16 [addr, 0], src_int32
    dispatch_next
.try_typed_array_slow:
    call_interp asm_try_put_by_value_typed_array, result
    branch_nonzero result, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_put_by_value
end

# Inline cache fast path for property access (own + prototype chain).
handler GetById
    temp base, tag, obj, shape, plc, cache_shape, cache_proto, prop_offset, dict_gen, cur_dict_gen, props, value, result
    load_operand base, m_base
    extract_tag tag, base
    branch_ne tag, OBJECT_TAG, .try_cache
    unbox_object obj, base
    load64 shape, [obj, OBJECT_SHAPE]
    # Get PropertyLookupCache* (direct pointer from instruction stream)
    load64 plc, [pb, pc, m_cache]
    assert_nonzero plc
    load_pair64 cache_shape, cache_proto, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_ne cache_shape, shape, .try_cache
    branch_nonzero cache_proto, .proto
    # Check dictionary generation matches
    load_pair32 prop_offset, dict_gen, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 cur_dict_gen, [shape, SHAPE_DICTIONARY_GENERATION]
    branch_ne dict_gen, cur_dict_gen, .try_cache
    # IC hit! Load property value via get_direct (own property)
    load64 props, [obj, OBJECT_NAMED_PROPERTIES]
    assert_nonzero props
    load64 value, [props, prop_offset, 8]
    # Check value is not an accessor
    extract_tag tag, value
    branch_eq tag, ACCESSOR_TAG, .try_cache
    store_operand m_dst, value
    dispatch_next
.proto:
    # cache_proto = prototype Object*, shape = object's shape, plc = PLC base
    load64 prop_offset, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE_CHAIN_VALIDITY]
    branch_zero prop_offset, .try_cache
    load8 tag, [prop_offset, PROTOTYPE_CHAIN_VALIDITY_VALID]
    branch_zero tag, .try_cache
    load_pair32 prop_offset, dict_gen, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 cur_dict_gen, [shape, SHAPE_DICTIONARY_GENERATION]
    branch_ne dict_gen, cur_dict_gen, .try_cache
    load64 props, [cache_proto, OBJECT_NAMED_PROPERTIES]
    assert_nonzero props
    load64 value, [props, prop_offset, 8]
    extract_tag tag, value
    branch_eq tag, ACCESSOR_TAG, .try_cache
    store_operand m_dst, value
    dispatch_next
.try_cache:
    # Try all cache entries via C++ helper
    call_interp asm_try_get_by_id_cache, result
    branch_zero result, .done
.slow:
    call_slow_path asm_slow_path_get_by_id
.done:
    dispatch_next
end

# Inline cache fast path for own-property store (ChangeOwnProperty).
handler PutById
    temp base, tag, obj, shape, plc, cache_shape, cache_proto, prop_offset, dict_gen, cur_dict_gen, props, value, src, result
    load_operand base, m_base
    extract_tag tag, base
    branch_ne tag, OBJECT_TAG, .try_cache
    unbox_object obj, base
    load64 shape, [obj, OBJECT_SHAPE]
    load64 plc, [pb, pc, m_cache]
    assert_nonzero plc
    load_pair64 cache_shape, cache_proto, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_ne cache_shape, shape, .try_cache
    branch_nonzero cache_proto, .try_cache
    load_pair32 prop_offset, dict_gen, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 cur_dict_gen, [shape, SHAPE_DICTIONARY_GENERATION]
    branch_ne dict_gen, cur_dict_gen, .try_cache
    # Check current value at prop_offset is not an accessor
    load64 props, [obj, OBJECT_NAMED_PROPERTIES]
    assert_nonzero props
    load64 value, [props, prop_offset, 8]
    extract_tag tag, value
    branch_eq tag, ACCESSOR_TAG, .try_cache
    # IC hit! Store new value via put_direct.
    load_operand src, m_src
    store64 [props, prop_offset, 8], src
    dispatch_next
.try_cache:
    # Try all cache entries via C++ helper (handles AddOwnProperty)
    call_interp asm_try_put_by_id_cache, result
    branch_zero result, .done
.slow:
    call_slow_path asm_slow_path_put_by_id
.done:
    dispatch_next
end

# Fast path for array[int32_index] with Packed/Holey indexed storage.
handler GetByValue
    temp base, prop, base_tag, prop_tag, index, obj, flags, storage_kind, size, elements, slot, capacity_addr, capacity, kind_byte, raw, addr, neg_zero, dst, result
    ftemp slot_dbl
    load_operand base, m_base
    load_operand prop, m_property
    extract_tag base_tag, base
    branch_ne base_tag, OBJECT_TAG, .slow
    extract_tag prop_tag, prop
    branch_ne prop_tag, INT32_TAG, .slow
    mov index, prop
    and index, 0xFFFFFFFF
    branch_bit_set index, 31, .slow
    unbox_object obj, base
    load8 flags, [obj, OBJECT_FLAGS]
    branch_bits_set flags, OBJECT_FLAG_IS_TYPED_ARRAY, .try_typed_array
    branch_bits_set flags, OBJECT_FLAG_MAY_INTERFERE, .slow
    # Packed is the hot path: in-bounds elements are always present.
    load8 storage_kind, [obj, OBJECT_INDEXED_STORAGE_KIND]
    branch_ne storage_kind, INDEXED_STORAGE_KIND_PACKED, .not_packed
    load32 size, [obj, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned index, size, .slow
    load64 elements, [obj, OBJECT_INDEXED_ELEMENTS]
    assert_nonzero elements
    load64 dst, [elements, index, 8]
    # NB: No accessor check needed -- Packed/Holey storage
    #     can only hold default-attributed data properties.
    store_operand m_dst, dst
    dispatch_next
.not_packed:
    branch_ne storage_kind, INDEXED_STORAGE_KIND_HOLEY, .slow
    # Holey arrays need a slot load to distinguish present elements from holes.
    load32 size, [obj, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned index, size, .slow
    load64 elements, [obj, OBJECT_INDEXED_ELEMENTS]
    branch_zero elements, .slow
    assert_nonzero elements
    mov capacity_addr, elements
    sub capacity_addr, 8
    load32 capacity, [capacity_addr, 0]
    branch_ge_unsigned index, capacity, .slow
    load64 slot, [elements, index, 8]
    mov neg_zero, EMPTY_TAG_SHIFTED
    branch_eq slot, neg_zero, .slow
    store_operand m_dst, slot
    dispatch_next
.try_typed_array:
    load64 elements, [obj, TYPED_ARRAY_CACHED_DATA_PTR]
    branch_zero elements, .try_typed_array_slow
    # Cached pointers only exist for fixed-length typed arrays, so array_length
    # is known to hold a concrete u32 value here.
    load32 capacity, [obj, TYPED_ARRAY_ARRAY_LENGTH_VALUE]
    branch_ge_unsigned index, capacity, .try_typed_array_slow
    load8 kind_byte, [obj, TYPED_ARRAY_KIND]
    branch_eq kind_byte, TYPED_ARRAY_KIND_INT32, .ta_int32
    branch_any_eq kind_byte, TYPED_ARRAY_KIND_UINT8, TYPED_ARRAY_KIND_UINT8_CLAMPED, .ta_uint8
    branch_eq kind_byte, TYPED_ARRAY_KIND_UINT16, .ta_uint16
    branch_eq kind_byte, TYPED_ARRAY_KIND_INT8, .ta_int8
    branch_eq kind_byte, TYPED_ARRAY_KIND_INT16, .ta_int16
    branch_eq kind_byte, TYPED_ARRAY_KIND_UINT32, .ta_uint32
    branch_eq kind_byte, TYPED_ARRAY_KIND_FLOAT32, .ta_float32
    branch_eq kind_byte, TYPED_ARRAY_KIND_FLOAT64, .ta_float64
    jmp .try_typed_array_slow
.ta_int32:
    load32 raw, [elements, index, 4]
    jmp .ta_box_int32
.ta_uint8:
    load8 raw, [elements, index]
    jmp .ta_box_int32
.ta_uint16:
    mov addr, index
    add addr, index
    load16 raw, [elements, addr]
    jmp .ta_box_int32
.ta_int8:
    load8s raw, [elements, index]
    jmp .ta_box_int32
.ta_int16:
    mov addr, index
    add addr, index
    load16s raw, [elements, addr]
    jmp .ta_box_int32
.ta_float32:
    mov addr, index
    shl addr, 2
    add addr, elements
    loadf32 slot_dbl, [addr, 0]
    float_to_double slot_dbl, slot_dbl
    fp_mov slot, slot_dbl
    mov neg_zero, NEGATIVE_ZERO
    branch_eq slot, neg_zero, .ta_f64_as_double
    double_to_int32 raw, slot_dbl, .ta_f64_as_double
    branch_nonzero raw, .ta_f64_as_int
    jmp .ta_f64_as_int
.ta_float64:
    mov addr, index
    shl addr, 3
    add addr, elements
    load64 slot, [addr, 0]
    fp_mov slot_dbl, slot
    # Exclude negative zero early (slot gets clobbered by double_to_int32).
    mov neg_zero, NEGATIVE_ZERO
    branch_eq slot, neg_zero, .ta_f64_as_double
    double_to_int32 raw, slot_dbl, .ta_f64_as_double
    branch_nonzero raw, .ta_f64_as_int
    # double_to_int32 succeeded with 0 -- this is +0.0, box as int
.ta_f64_as_int:
    box_int32 dst, raw
    store_operand m_dst, dst
    dispatch_next
.ta_f64_as_double:
    canonicalize_nan dst, slot_dbl
    store_operand m_dst, dst
    dispatch_next
.ta_uint32:
    load32 raw, [elements, index, 4]
    branch_bit_set raw, 31, .ta_uint32_to_double
    jmp .ta_box_int32
.ta_uint32_to_double:
    int_to_double slot_dbl, raw
    fp_mov dst, slot_dbl
    store_operand m_dst, dst
    dispatch_next
.ta_box_int32:
    box_int32_clean dst, raw
    store_operand m_dst, dst
    dispatch_next
.try_typed_array_slow:
    call_interp asm_try_get_by_value_typed_array, result
    branch_nonzero result, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_by_value
end

# Fast path for Array.length (magical length property).
# Also includes IC fast path for non-array objects (same as GetById).
handler GetLength
    temp base, tag, obj, flags, shape, plc, cache_shape, cache_proto, prop_offset, dict_gen, cur_dict_gen, props, value, length, sign_check, dst
    ftemp length_dbl
    load_operand base, m_base
    extract_tag tag, base
    branch_ne tag, OBJECT_TAG, .slow
    unbox_object obj, base
    load8 flags, [obj, OBJECT_FLAGS]
    branch_bits_set flags, OBJECT_FLAG_HAS_MAGICAL_LENGTH, .magical_length
    # Non-magical length: IC fast path (same as GetById)
    load64 shape, [obj, OBJECT_SHAPE]
    load64 plc, [pb, pc, m_cache]
    assert_nonzero plc
    load_pair64 cache_shape, cache_proto, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_ne cache_shape, shape, .slow
    branch_nonzero cache_proto, .slow
    load_pair32 prop_offset, dict_gen, [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [plc, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 cur_dict_gen, [shape, SHAPE_DICTIONARY_GENERATION]
    branch_ne dict_gen, cur_dict_gen, .slow
    load64 props, [obj, OBJECT_NAMED_PROPERTIES]
    assert_nonzero props
    load64 value, [props, prop_offset, 8]
    extract_tag tag, value
    branch_eq tag, ACCESSOR_TAG, .slow
    store_operand m_dst, value
    dispatch_next
.magical_length:
    # Object.m_indexed_array_like_size (u32). Box as int32 when bit 31 is
    # clear; otherwise widen to a double so the value isn't reinterpreted
    # as a negative int32.
    load32 length, [obj, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    mov sign_check, length
    shr sign_check, 31
    branch_nonzero sign_check, .length_double
    box_int32 dst, length
    store_operand m_dst, dst
    dispatch_next
.length_double:
    int_to_double length_dbl, length
    fp_mov dst, length_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_length
end

# Inline cache fast path for global variable access via the global object.
handler GetGlobal
    temp realm, global_object, env, gvc, cache_serial, env_serial, shape, cache_shape, cur_dict_gen, prop_offset, dict_gen, props, value, tag, has_env, in_module, idx, binding, init, result
    load64 realm, [exec_ctx, EXECUTION_CONTEXT_REALM]
    load_pair64 global_object, env, [realm, REALM_GLOBAL_OBJECT], [realm, REALM_GLOBAL_DECLARATIVE_ENVIRONMENT]
    assert_nonzero global_object
    assert_nonzero env
    load64 gvc, [pb, pc, m_cache]
    assert_nonzero gvc
    load64 cache_serial, [gvc, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL]
    load64 env_serial, [env, DECLARATIVE_ENVIRONMENT_SERIAL]
    branch_ne cache_serial, env_serial, .slow
    # Shape-based fast path: check entries[0].shape matches global_object.shape
    # (falls through to env binding path on shape mismatch)
    load64 shape, [global_object, OBJECT_SHAPE]
    load64 cache_shape, [gvc, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne cache_shape, shape, .try_env_binding
    load32 cur_dict_gen, [shape, SHAPE_DICTIONARY_GENERATION]
    load_pair32 prop_offset, dict_gen, [gvc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [gvc, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    branch_ne dict_gen, cur_dict_gen, .try_env_binding
    # IC hit! Load property value via get_direct
    load64 props, [global_object, OBJECT_NAMED_PROPERTIES]
    assert_nonzero props
    load64 value, [props, prop_offset, 8]
    extract_tag tag, value
    branch_eq tag, ACCESSOR_TAG, .slow
    store_operand m_dst, value
    dispatch_next
.try_env_binding:
    load8 has_env, [gvc, GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING]
    branch_zero has_env, .slow
    # Bail to C++ for module environments (rare).
    load8 in_module, [gvc, GLOBAL_VARIABLE_CACHE_IN_MODULE_ENVIRONMENT]
    branch_nonzero in_module, .slow_env
    # Inline env binding: index into global_declarative_environment bindings.
    load32 idx, [gvc, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX]
    load64 binding, [env, BINDINGS_DATA_PTR]
    assert_nonzero binding
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    load8 init, [binding, BINDING_INITIALIZED]
    branch_zero init, .slow
    load64 value, [binding, BINDING_VALUE]
    store_operand m_dst, value
    dispatch_next
.slow_env:
    call_interp asm_try_get_global_env_binding, result
    branch_nonzero result, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_global
end

# Inline cache fast path for global variable store via the global object.
handler SetGlobal
    temp realm, global_object, env, gvc, cache_serial, env_serial, shape, cache_shape, cur_dict_gen, prop_offset, dict_gen, props, value, tag, has_env, in_module, idx, binding, flag, src, result
    load64 realm, [exec_ctx, EXECUTION_CONTEXT_REALM]
    load_pair64 global_object, env, [realm, REALM_GLOBAL_OBJECT], [realm, REALM_GLOBAL_DECLARATIVE_ENVIRONMENT]
    assert_nonzero global_object
    assert_nonzero env
    load64 gvc, [pb, pc, m_cache]
    assert_nonzero gvc
    load64 cache_serial, [gvc, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL]
    load64 env_serial, [env, DECLARATIVE_ENVIRONMENT_SERIAL]
    branch_ne cache_serial, env_serial, .slow
    load64 shape, [global_object, OBJECT_SHAPE]
    load64 cache_shape, [gvc, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne cache_shape, shape, .try_env_binding
    load32 cur_dict_gen, [shape, SHAPE_DICTIONARY_GENERATION]
    load_pair32 prop_offset, dict_gen, [gvc, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [gvc, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    branch_ne dict_gen, cur_dict_gen, .try_env_binding
    # IC hit! Load current value to check it's not an accessor.
    load64 props, [global_object, OBJECT_NAMED_PROPERTIES]
    assert_nonzero props
    load64 value, [props, prop_offset, 8]
    extract_tag tag, value
    branch_eq tag, ACCESSOR_TAG, .slow
    load_operand src, m_src
    store64 [props, prop_offset, 8], src
    dispatch_next
.try_env_binding:
    load8 has_env, [gvc, GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING]
    branch_zero has_env, .slow
    load8 in_module, [gvc, GLOBAL_VARIABLE_CACHE_IN_MODULE_ENVIRONMENT]
    branch_nonzero in_module, .slow_env
    load32 idx, [gvc, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX]
    load64 binding, [env, BINDINGS_DATA_PTR]
    assert_nonzero binding
    mul idx, idx, SIZEOF_BINDING
    add binding, idx
    load8 flag, [binding, BINDING_INITIALIZED]
    branch_zero flag, .slow
    load8 flag, [binding, BINDING_MUTABLE]
    branch_zero flag, .slow
    load_operand src, m_src
    store64 [binding, BINDING_VALUE], src
    dispatch_next
.slow_env:
    call_interp asm_try_set_global_env_binding, result
    branch_nonzero result, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_set_global
end

handler Call
    # Inline Call in asm for the two callee kinds that can stay in the
    # dispatch loop without taking the full Call slow path:
    #
    #  - ECMAScriptFunctionObject with inline-ready bytecode: build the
    #    callee frame here and dispatch at pc = 0 of the callee bytecode.
    #    Cases that need function-environment allocation or sloppy primitive
    #    this-boxing can't stay in pure asm but also don't want the full
    #    slow path (which would insert a run_executable() boundary and an
    #    observable microtask drain), so they detour through the
    #    asm_try_inline_call helper at .call_interp_inline.
    #
    #  - RawNativeFunction: build a callee ExecutionContext here, call the
    #    stored C++ function pointer directly via call_raw_native, and then
    #    tear the frame down on return. Exceptions go through a dedicated
    #    helper that unwinds the callee frame before dispatching to a JS
    #    handler (see .call_raw_native_exception).
    #
    # Everything else -- non-functions, NativeJavaScriptBackedFunction,
    # ECMAScript functions that can't inline, Proxies, ... -- falls through
    # to .call_slow, i.e. asm_slow_path_call.
    #
    # High-level flow of the ECMAScript fast path:
    #   1. Validate the callee and load its shared function metadata.
    #   2. Bind `this` inline when we can do so without allocations.
    #   3. Reserve an InterpreterStack frame and populate ExecutionContext.
    #   4. Materialize [registers | locals | constants | arguments].
    #   5. Swap VM state over to the callee frame and dispatch at pc = 0.
    temp callee, callee_value, flags, shared_data, exec_ptr, meta, this_value, tag, scratch, formal_count, passed_count, arg_count, total_slots, regs_locals_count, frame_bytes, vm_ptr, stack_limit, frame_base, value_tail, realm, lex_env, priv_env, empty_tag, som_src, som_lo, som_hi, return_pc, return_dst, base_pc, slot_offset, slot_end, const_count, const_data, const_idx, const_value, write_idx, arg_idx, arg_ops, arg_value, undef_slot, fill_end, native_func, variant, native_return, helper_arg, native_pc, exception_pc, native_total_bytes, after_pc, dst_offset, after_offset, result
    load_operand callee_value, m_callee
    extract_tag tag, callee_value
    branch_ne tag, OBJECT_TAG, .call_slow
    unbox_object callee, callee_value

    # Non-functions still go through the normal Call slow path for proper
    # error reporting. Non-ECMAScript function objects get a
    # RawNativeFunction fast path attempt before we fully give up.
    load8 flags, [callee, OBJECT_FLAGS]
    branch_bits_clear flags, OBJECT_FLAG_IS_FUNCTION, .call_slow
    branch_bits_clear flags, OBJECT_FLAG_IS_ECMASCRIPT_FUNCTION_OBJECT, .call_try_native

    load64 shared_data, [callee, ECMASCRIPT_FUNCTION_OBJECT_SHARED_DATA]
    assert_nonzero shared_data
    load_pair64 exec_ptr, meta, [shared_data, SHARED_FUNCTION_INSTANCE_DATA_EXECUTABLE], [shared_data, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA]
    branch_bits_clear meta, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_CAN_INLINE_CALL, .call_slow
    # NewFunctionEnvironment() allocation and lexical-this resolution both use
    # the C++ helper, instead of the pure asm path.
    branch_bits_set meta, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION, .call_interp_inline

    # Bind this without allocations. Sloppy primitive this-values still need
    # ToObject(), so they use the C++ inline-frame helper.
    #
    # this_value starts as "empty" to match the normal interpreter behavior
    # for callees that never observe `this`.
    mov this_value, EMPTY_TAG_SHIFTED
    branch_bits_clear meta, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_USES_THIS, .this_ready
    load_operand this_value, m_this_value
    branch_bits_set meta, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_STRICT, .this_ready

    # Sloppy null/undefined binds the callee realm's global object.
    # Sloppy primitive receivers need ToObject(), which may allocate
    # wrappers, so they go through the helper instead of the full slow path.
    extract_tag tag, this_value
    mov scratch, tag
    and scratch, 0xFFFE
    branch_eq scratch, UNDEFINED_TAG, .sloppy_global_this
    branch_eq tag, OBJECT_TAG, .this_ready
    jmp .call_interp_inline

.sloppy_global_this:
    load64 scratch, [callee, OBJECT_SHAPE]
    assert_nonzero scratch
    load64 scratch, [scratch, SHAPE_REALM]
    assert_nonzero scratch
    load64 scratch, [scratch, REALM_GLOBAL_ENVIRONMENT]
    assert_nonzero scratch
    load64 scratch, [scratch, GLOBAL_ENVIRONMENT_GLOBAL_THIS_VALUE]
    # Match Value(Object*): keep only the low 48 pointer bits before boxing.
    shl scratch, 16
    shr scratch, 16
    mov this_value, OBJECT_TAG_SHIFTED
    or this_value, scratch

.this_ready:
    # The low 32 bits of the packed metadata word hold the formal parameter count.
    and meta, 0xFFFFFFFF

    load32 passed_count, [pb, pc, m_argument_count]
    mov formal_count, meta
    branch_ge_unsigned formal_count, passed_count, .arg_count_ready
    mov formal_count, passed_count
.arg_count_ready:
    load_pair32 regs_locals_count, total_slots, [exec_ptr, EXECUTABLE_REGISTERS_AND_LOCALS_COUNT], [exec_ptr, EXECUTABLE_REGISTERS_AND_LOCALS_AND_CONSTANTS_COUNT]
    assert_nonzero exec_ptr
    assert_ge_unsigned total_slots, regs_locals_count

    # Inline InterpreterStack::allocate().
    add total_slots, formal_count
    mov frame_bytes, total_slots
    shl frame_bytes, 3
    add frame_bytes, SIZEOF_EXECUTION_CONTEXT

    load_vm vm_ptr
    lea vm_ptr, [vm_ptr, VM_INTERPRETER_STACK]
    load_pair64 frame_base, stack_limit, [vm_ptr, INTERPRETER_STACK_TOP], [vm_ptr, INTERPRETER_STACK_LIMIT]
    assert_nonzero frame_base
    assert_nonzero stack_limit
    add frame_bytes, frame_base
    branch_ge_unsigned stack_limit, frame_bytes, .stack_ok
    jmp .call_slow

.stack_ok:
    load_vm vm_ptr
    store64 [vm_ptr, VM_INTERPRETER_STACK_TOP], frame_bytes

    # Set up the callee ExecutionContext header exactly the way
    # VM::push_inline_frame() / run_executable() would see it.
    lea value_tail, [frame_base, SIZEOF_EXECUTION_CONTEXT]
    store_pair32 [frame_base, EXECUTION_CONTEXT_REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT], [frame_base, EXECUTION_CONTEXT_ARGUMENT_COUNT], total_slots, formal_count
    load32 scratch, [pb, pc, m_argument_count]
    store32 [frame_base, EXECUTION_CONTEXT_PASSED_ARGUMENT_COUNT], scratch

    load64 realm, [callee, OBJECT_SHAPE]
    load64 realm, [realm, SHAPE_REALM]
    assert_nonzero realm
    store_pair64 [frame_base, EXECUTION_CONTEXT_FUNCTION], [frame_base, EXECUTION_CONTEXT_REALM], callee, realm

    load_pair64 lex_env, priv_env, [callee, ECMASCRIPT_FUNCTION_OBJECT_ENVIRONMENT], [callee, ECMASCRIPT_FUNCTION_OBJECT_PRIVATE_ENVIRONMENT]
    assert_nonzero lex_env
    store_pair64 [frame_base, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT], [frame_base, EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT], lex_env, lex_env
    store64 [frame_base, EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT], priv_env
    store_pair64 [frame_base, EXECUTION_CONTEXT_THIS_VALUE], [frame_base, EXECUTION_CONTEXT_EXECUTABLE], this_value, exec_ptr

    mov empty_tag, EMPTY_TAG_SHIFTED
    store_pair64 [value_tail, ACCUMULATOR_REG_OFFSET], [value_tail, EXCEPTION_REG_OFFSET], empty_tag, empty_tag
    store64 [value_tail, THIS_VALUE_REG_OFFSET], this_value
    store_pair64 [value_tail, RETURN_VALUE_REG_OFFSET], [value_tail, SAVED_LEXICAL_ENVIRONMENT_REG_OFFSET], empty_tag, empty_tag

    # ScriptOrModule is a two-word Variant in ExecutionContext, so copy both
    # machine words explicitly.
    lea scratch, [frame_base, EXECUTION_CONTEXT_SCRIPT_OR_MODULE]
    lea som_src, [callee, ECMASCRIPT_FUNCTION_OBJECT_SCRIPT_OR_MODULE]
    load_pair64 som_lo, som_hi, [som_src, 0], [som_src, 8]
    store64 [scratch, 0], som_lo
    store64 [scratch, 8], som_hi

    store32 [frame_base, EXECUTION_CONTEXT_PROGRAM_COUNTER], 0
    store32 [frame_base, EXECUTION_CONTEXT_SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER], 0
    mov scratch, EXECUTION_CONTEXT_NO_YIELD_CONTINUATION
    store32 [frame_base, EXECUTION_CONTEXT_YIELD_CONTINUATION], scratch
    store8 [frame_base, EXECUTION_CONTEXT_YIELD_IS_AWAIT], 0
    store8 [frame_base, EXECUTION_CONTEXT_CALLER_IS_CONSTRUCT], 0
    store64 [frame_base, EXECUTION_CONTEXT_CALLER_FRAME], exec_ctx
    load_pair32 return_pc, return_dst, [pb, pc, m_length], [pb, pc, m_dst]
    lea base_pc, [pb, pc]
    sub base_pc, pb
    add return_pc, base_pc
    store_pair32 [frame_base, EXECUTION_CONTEXT_CALLER_RETURN_PC], [frame_base, EXECUTION_CONTEXT_CALLER_DST_RAW], return_pc, return_dst

    # values = [registers | locals | constants | arguments]
    # Walk value_tail with two cursors: slot_offset for the byte index and
    # write_idx for the element index when copying constants/arguments.
    mov slot_end, regs_locals_count
    shl slot_end, 3
    mov slot_offset, RESERVED_REGISTERS_SIZE
.clear_registers_and_locals:
    mov scratch, slot_offset
    add scratch, 8
    branch_ge_unsigned scratch, slot_end, .clear_registers_and_locals_tail
    store_pair64 [value_tail, slot_offset, 0], [value_tail, slot_offset, 8], empty_tag, empty_tag
    add slot_offset, 16
    jmp .clear_registers_and_locals

.clear_registers_and_locals_tail:
    branch_ge_unsigned slot_offset, slot_end, .copy_constants
    store64 [value_tail, slot_offset], empty_tag

.copy_constants:
    load64 const_data, [frame_base, EXECUTION_CONTEXT_EXECUTABLE]
    load_pair64 const_count, const_data, [const_data, EXECUTABLE_ASM_CONSTANTS_SIZE], [const_data, EXECUTABLE_ASM_CONSTANTS_DATA]
    mov write_idx, regs_locals_count
    xor const_idx, const_idx
.copy_constants_loop:
    branch_ge_unsigned const_idx, const_count, .copy_arguments
    assert_nonzero const_data
    load64 const_value, [const_data, const_idx, 8]
    store64 [value_tail, write_idx, 8], const_value
    add const_idx, 1
    add write_idx, 1
    jmp .copy_constants_loop

.copy_arguments:
    load32 arg_count, [pb, pc, m_argument_count]
    assert_ge_unsigned formal_count, arg_count
    mov write_idx, regs_locals_count
    add write_idx, const_count
    lea scratch, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    lea arg_ops, [pb, pc]
    add arg_ops, m_expression_string
    add arg_ops, 4
    xor arg_idx, arg_idx
.copy_arguments_loop:
    # The operand array in the bytecode stores caller register indices.
    branch_ge_unsigned arg_idx, arg_count, .fill_missing_arguments
    load32 arg_value, [arg_ops, arg_idx, 4]
    load64 arg_value, [scratch, arg_value, 8]
    store64 [value_tail, write_idx, 8], arg_value
    add arg_idx, 1
    add write_idx, 1
    jmp .copy_arguments_loop

.fill_missing_arguments:
    mov fill_end, write_idx
    add fill_end, formal_count
    sub fill_end, arg_count
    mov undef_slot, UNDEFINED_SHIFTED
.fill_missing_arguments_loop:
    branch_ge_unsigned write_idx, fill_end, .enter_callee
    store64 [value_tail, write_idx, 8], undef_slot
    add write_idx, 1
    jmp .fill_missing_arguments_loop

.enter_callee:
    load64 pb, [frame_base, EXECUTION_CONTEXT_EXECUTABLE]
    load64 pb, [pb, EXECUTABLE_BYTECODE_DATA]
    assert_nonzero pb
    load_vm vm_ptr
    store64 [vm_ptr, VM_RUNNING_EXECUTION_CONTEXT], frame_base
    mov exec_ctx, frame_base
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    xor pc, pc
    goto_handler pc
.call_interp_inline:
    # Shared escape hatch for the cases that need C++ help to build the
    # inline frame correctly but must not take the full Call slow path,
    # since that would insert a run_executable() boundary and observable
    # microtask drain.
    call_interp asm_try_inline_call, result
    branch_nonzero result, .call_slow
    load_vm vm_ptr
    load64 exec_ctx, [vm_ptr, VM_RUNNING_EXECUTION_CONTEXT]
    assert_nonzero exec_ctx
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    load64 scratch, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    assert_nonzero scratch
    load64 pb, [scratch, EXECUTABLE_BYTECODE_DATA]
    assert_nonzero pb
    xor pc, pc
    goto_handler pc
.call_try_native:
    # Fast path for RawNativeFunction: the callee is a plain C++ function
    # pointer with no JS-visible prologue, so we can build the callee frame
    # ourselves and jump straight at the entry point. NativeFunction
    # objects that still carry a callback (NativeJavaScriptBackedFunction)
    # do not have this flag set and fall through to .call_slow.
    load8 flags, [callee, OBJECT_FLAGS]
    branch_bits_clear flags, OBJECT_FLAG_IS_RAW_NATIVE_FUNCTION, .call_slow

    # Unlike the ECMAScript path we don't pad to the formal parameter count:
    # native functions read their arguments via the passed-count API, so we
    # only need space for the call-site arguments plus the EC header.
    load32 arg_count, [pb, pc, m_argument_count]
    mov native_total_bytes, arg_count
    shl native_total_bytes, 3
    add native_total_bytes, SIZEOF_EXECUTION_CONTEXT

    # Inline InterpreterStack::allocate(): bail to C++ if the interpreter
    # stack doesn't have room for the new frame.
    load_vm vm_ptr
    lea vm_ptr, [vm_ptr, VM_INTERPRETER_STACK]
    load_pair64 frame_base, stack_limit, [vm_ptr, INTERPRETER_STACK_TOP], [vm_ptr, INTERPRETER_STACK_LIMIT]
    assert_nonzero frame_base
    assert_nonzero stack_limit
    add native_total_bytes, frame_base
    branch_ge_unsigned stack_limit, native_total_bytes, .native_interpreter_stack_ok
    jmp .call_slow

.native_interpreter_stack_ok:
    # RawNativeFunctions run real C++ code on the host stack, so we also
    # have to check that we're not about to blow past the VM's reserved
    # stack limit. The ECMAScript path can skip this because it never
    # leaves asm.
    load_vm vm_ptr
    lea vm_ptr, [vm_ptr, VM_STACK_INFO]
    load64 stack_limit, [vm_ptr, STACK_INFO_BASE]
    add stack_limit, VM_STACK_SPACE_LIMIT
    branch_ge_unsigned fp, stack_limit, .native_stack_space_ok
    jmp .call_slow

.native_stack_space_ok:
    # Commit the new interpreter stack top.
    load_vm vm_ptr
    store64 [vm_ptr, VM_INTERPRETER_STACK_TOP], native_total_bytes

    # Populate the callee EC to match VM::push_execution_context plus
    # NativeFunction::internal_call. value_tail walks past the EC header to
    # the argument Value array.
    lea value_tail, [frame_base, SIZEOF_EXECUTION_CONTEXT]
    # For natives, argument_count and "registers+..." total are both just
    # the call-site argument count: there are no registers, locals, or
    # constants.
    store_pair32 [frame_base, EXECUTION_CONTEXT_REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT], [frame_base, EXECUTION_CONTEXT_ARGUMENT_COUNT], arg_count, arg_count
    store32 [frame_base, EXECUTION_CONTEXT_PASSED_ARGUMENT_COUNT], arg_count

    # Shape stores a Realm pointer; use it as the callee EC realm.
    load64 realm, [callee, OBJECT_SHAPE]
    load64 realm, [realm, SHAPE_REALM]
    assert_nonzero realm
    store_pair64 [frame_base, EXECUTION_CONTEXT_FUNCTION], [frame_base, EXECUTION_CONTEXT_REALM], callee, realm

    # Mirror NativeFunction::internal_call: a raw native has no environment
    # of its own, so lexical/variable/private environments are copied
    # straight from the caller frame.
    load_pair64 lex_env, scratch, [exec_ctx, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT], [exec_ctx, EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT]
    store_pair64 [frame_base, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT], [frame_base, EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT], lex_env, scratch
    load64 priv_env, [exec_ctx, EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT]
    store64 [frame_base, EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT], priv_env
    # |this| is forwarded unchanged. Native builtins do their own type
    # checks on the receiver where they need to.
    load_operand this_value, m_this_value
    store64 [frame_base, EXECUTION_CONTEXT_THIS_VALUE], this_value

    # Zero out the ScriptOrModule variant (two words) and Executable
    # pointer. Native frames don't belong to any script/module.
    xor scratch, scratch
    lea som_src, [frame_base, EXECUTION_CONTEXT_SCRIPT_OR_MODULE]
    store_pair64 [som_src, 0], [som_src, 8], scratch, scratch
    store64 [frame_base, EXECUTION_CONTEXT_EXECUTABLE], scratch
    store32 [frame_base, EXECUTION_CONTEXT_PROGRAM_COUNTER], 0
    store32 [frame_base, EXECUTION_CONTEXT_SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER], 0
    mov scratch, EXECUTION_CONTEXT_NO_YIELD_CONTINUATION
    store32 [frame_base, EXECUTION_CONTEXT_YIELD_CONTINUATION], scratch
    store8 [frame_base, EXECUTION_CONTEXT_YIELD_IS_AWAIT], 0
    store8 [frame_base, EXECUTION_CONTEXT_CALLER_IS_CONSTRUCT], 0

    # While asm runs, the authoritative program counter lives in the `pc`
    # register and the caller EC's stored program_counter is stale. Before
    # we leave asm to run native C++ that may throw, sync `pc` into the
    # caller EC as a bytecode offset (pc - pb). The exception helper and
    # VM::handle_exception both read from the caller EC after unwind.
    lea native_pc, [pb, pc]
    sub native_pc, pb
    store32 [exec_ctx, EXECUTION_CONTEXT_PROGRAM_COUNTER], native_pc
    store64 [frame_base, EXECUTION_CONTEXT_CALLER_FRAME], exec_ctx
    # CALLER_RETURN_PC is the bytecode offset of the instruction after
    # the Call. CALLER_DST_RAW records where the return value should
    # be written in the caller's value array.
    load32 after_pc, [pb, pc, m_length]
    add after_pc, native_pc
    store32 [frame_base, EXECUTION_CONTEXT_CALLER_RETURN_PC], after_pc
    load32 dst_offset, [pb, pc, m_dst]
    store32 [frame_base, EXECUTION_CONTEXT_CALLER_DST_RAW], dst_offset

    # Copy the call-site arguments from the caller's value array into the
    # callee frame's argument tail. scratch points at the caller's value
    # array, arg_ops at the Operand[] that trails the fixed Call instruction
    # fields. The Call layout ends with `m_expression_string:
    # Optional<StringTableIndex>` (4 bytes via the sentinel specialization)
    # followed by `m_arguments`, so base + offsetof(m_expression_string) + 4
    # is the operand array.
    lea scratch, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    lea arg_ops, [pb, pc]
    add arg_ops, m_expression_string
    add arg_ops, 4
    xor arg_idx, arg_idx
.copy_native_arguments_loop:
    branch_ge_unsigned arg_idx, arg_count, .enter_raw_native
    load32 arg_value, [arg_ops, arg_idx, 4]
    load64 arg_value, [scratch, arg_value, 8]
    store64 [value_tail, arg_idx, 8], arg_value
    add arg_idx, 1
    jmp .copy_native_arguments_loop

.enter_raw_native:
    # Swap the running ExecutionContext over to the callee and point the
    # asm `values` register at its argument array. After this, we look like
    # a normal inline frame from the VM's perspective.
    load_vm vm_ptr
    store64 [vm_ptr, VM_RUNNING_EXECUTION_CONTEXT], frame_base
    mov exec_ctx, frame_base
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]

    # Invoke the raw C++ function pointer. call_raw_native lowers to a
    # native call through the platform ABI and surfaces the returned
    # ThrowCompletionOr<Value> via (payload, variant). The variant low byte
    # is 0 for a Value, 1 for an ErrorValue; anything else means the native
    # threw and payload is the thrown Value, not a return value.
    load64 native_func, [callee, RAW_NATIVE_FUNCTION_NATIVE_FUNCTION]
    assert_nonzero native_func
    call_raw_native native_func, native_return, variant
    and variant, 0xFF
    branch_nonzero variant, .call_raw_native_exception

    # Normal return path: tear the callee frame off the interpreter stack,
    # restore the caller as the running ExecutionContext, write the return
    # value into the caller's m_dst operand, and dispatch the next insn.
    load64 frame_base, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    assert_nonzero frame_base
    load_vm vm_ptr
    store64 [vm_ptr, VM_RUNNING_EXECUTION_CONTEXT], frame_base
    store64 [vm_ptr, VM_INTERPRETER_STACK_TOP], exec_ctx
    mov exec_ctx, frame_base
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    store_operand m_dst, native_return
    load32 after_offset, [pb, pc, m_length]
    dispatch_variable after_offset

.call_raw_native_exception:
    # The native threw. Hand the thrown Value off to a C++ helper, which
    # unwinds the callee frame off the interpreter stack and calls through
    # to VM::handle_exception. Return value follows the standard asm
    # slow-path convention (see AsmInterpreter.cpp:127):
    #   >= 0 : an enclosing handler was found; the result is the new
    #          program counter to resume at inside the (post-unwind)
    #          running execution context.
    #    < 0 : no handler; bail out of the asm dispatch loop entirely.
    # native_return is pinned to rax by call_raw_native; helper_arg is
    # pinned to rcx by call_helper, so this mov is the explicit bridge
    # between the two ABIs.
    mov helper_arg, native_return
    call_helper asm_helper_handle_raw_native_exception, helper_arg, exception_pc
    branch_negative exception_pc, .call_exit_asm
    # Reload exec_ctx/values/pb/pc from the caller frame the helper left
    # us on, and resume dispatching at its program_counter (which the
    # helper already updated to the handler entry).
    load_vm vm_ptr
    load64 exec_ctx, [vm_ptr, VM_RUNNING_EXECUTION_CONTEXT]
    assert_nonzero exec_ctx
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    load64 scratch, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    assert_nonzero scratch
    load64 pb, [scratch, EXECUTABLE_BYTECODE_DATA]
    assert_nonzero pb
    load32 native_pc, [exec_ctx, EXECUTION_CONTEXT_PROGRAM_COUNTER]
    mov pc, native_pc
    goto_handler pc
.call_exit_asm:
    # No JS handler caught the native exception; bail out of the asm
    # dispatch loop and let the C++ caller of run_asm() see the throw.
    exit
.call_slow:
    call_slow_path asm_slow_path_call
end

# Fast paths for common Math builtins with a single double argument.
# Before using the fast path, we validate that the callee is still the
# original builtin function (user code may have reassigned e.g. Math.abs).
handler CallBuiltinMathAbs
    temp arg, tag, int_value, dst
    ftemp arg_dbl
    validate_callee_builtin BUILTIN_MATH_ABS, .slow
    load_operand arg, m_argument
    check_is_double arg, .try_abs_int32
    # abs(double) = clear sign bit (bit 63)
    clear_bit arg, 63
    store_operand m_dst, arg
    dispatch_next
.try_abs_int32:
    extract_tag tag, arg
    branch_ne tag, INT32_TAG, .slow
    # abs(int32): negate if negative
    unbox_int32 int_value, arg
    branch_not_negative int_value, .abs_positive
    neg32_overflow int_value, .abs_overflow
.abs_positive:
    box_int32_clean dst, int_value
    store_operand m_dst, dst
    dispatch_next
.abs_overflow:
    # INT32_MIN: abs(-2147483648) = 2147483648.0
    unbox_int32 int_value, arg
    neg int_value
    int_to_double arg_dbl, int_value
    fp_mov dst, arg_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_abs
end

handler CallBuiltinMathFloor
    temp arg, dst
    ftemp arg_dbl
    validate_callee_builtin BUILTIN_MATH_FLOOR, .slow
    load_operand arg, m_argument
    check_is_double arg, .slow
    fp_mov arg_dbl, arg
    fp_floor arg_dbl, arg_dbl
    box_double_or_int32 dst, arg_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_floor
end

handler CallBuiltinMathCeil
    temp arg, dst
    ftemp arg_dbl
    validate_callee_builtin BUILTIN_MATH_CEIL, .slow
    load_operand arg, m_argument
    check_is_double arg, .slow
    fp_mov arg_dbl, arg
    fp_ceil arg_dbl, arg_dbl
    box_double_or_int32 dst, arg_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_ceil
end

handler CallBuiltinMathSqrt
    temp arg, dst
    ftemp arg_dbl
    validate_callee_builtin BUILTIN_MATH_SQRT, .slow
    load_operand arg, m_argument
    check_is_double arg, .slow
    fp_mov arg_dbl, arg
    fp_sqrt arg_dbl, arg_dbl
    box_double_or_int32 dst, arg_dbl
    store_operand m_dst, dst
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_sqrt
end

handler CallBuiltinMathExp
    temp arg, result
    ftemp arg_dbl
    validate_callee_builtin BUILTIN_MATH_EXP, .slow
    load_operand arg, m_argument
    check_is_double arg, .slow
    fp_mov arg_dbl, arg
    call_helper asm_helper_math_exp, arg, result
    store_operand m_dst, result
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_exp
end

handler CallBuiltinMathLog
    call_slow_path asm_slow_path_call_builtin_math_log
end

handler CallBuiltinMathPow
    call_slow_path asm_slow_path_call_builtin_math_pow
end

handler CallBuiltinMathImul
    call_slow_path asm_slow_path_call_builtin_math_imul
end

handler CallBuiltinMathRandom
    call_slow_path asm_slow_path_call_builtin_math_random
end

handler CallBuiltinMathRound
    call_slow_path asm_slow_path_call_builtin_math_round
end

handler CallBuiltinMathSin
    call_slow_path asm_slow_path_call_builtin_math_sin
end

handler CallBuiltinMathCos
    call_slow_path asm_slow_path_call_builtin_math_cos
end

handler CallBuiltinMathTan
    call_slow_path asm_slow_path_call_builtin_math_tan
end

handler CallBuiltinRegExpPrototypeExec
    call_slow_path asm_slow_path_call_builtin_regexp_prototype_exec
end

handler CallBuiltinRegExpPrototypeReplace
    call_slow_path asm_slow_path_call_builtin_regexp_prototype_replace
end

handler CallBuiltinRegExpPrototypeSplit
    call_slow_path asm_slow_path_call_builtin_regexp_prototype_split
end

handler CallBuiltinOrdinaryHasInstance
    call_slow_path asm_slow_path_call_builtin_ordinary_has_instance
end

handler CallBuiltinArrayIteratorPrototypeNext
    call_slow_path asm_slow_path_call_builtin_array_iterator_prototype_next
end

handler CallBuiltinMapIteratorPrototypeNext
    call_slow_path asm_slow_path_call_builtin_map_iterator_prototype_next
end

handler CallBuiltinSetIteratorPrototypeNext
    call_slow_path asm_slow_path_call_builtin_set_iterator_prototype_next
end

handler CallBuiltinStringIteratorPrototypeNext
    call_slow_path asm_slow_path_call_builtin_string_iterator_prototype_next
end

handler CallBuiltinStringFromCharCode
    temp arg, tag, code_unit, result
    validate_callee_builtin BUILTIN_STRING_FROM_CHAR_CODE, .slow

    load_operand arg, m_argument
    extract_tag tag, arg
    branch_ne tag, INT32_TAG, .slow
    unbox_int32 code_unit, arg
    and code_unit, 0xffff
    branch_ge_unsigned code_unit, 0x80, .single_code_unit

    call_helper asm_helper_single_ascii_character_string, code_unit, result
    extract_tag tag, result
    assert_eq tag, STRING_TAG
    store_operand m_dst, result
    dispatch_next

.single_code_unit:
    assert_lt_unsigned code_unit, 0x10000
    call_helper asm_helper_single_utf16_code_unit_string, code_unit, result
    extract_tag tag, result
    assert_eq tag, STRING_TAG
    store_operand m_dst, result
    dispatch_next

.slow:
    call_slow_path asm_slow_path_call_builtin_string_from_char_code
end

handler CallBuiltinStringPrototypeCharCodeAt
    temp this_value, tag, string, arg, index, code_unit, dst
    validate_callee_builtin BUILTIN_STRING_PROTOTYPE_CHAR_CODE_AT, .slow

    load_operand this_value, m_this_value
    extract_tag tag, this_value
    branch_ne tag, STRING_TAG, .slow
    unbox_object string, this_value

    load_operand arg, m_argument
    extract_tag tag, arg
    branch_ne tag, INT32_TAG, .slow
    unbox_int32 index, arg
    branch_negative index, .out_of_bounds

    load_primitive_string_utf16_code_unit string, index, code_unit, .out_of_bounds, .slow
    box_int32_clean dst, code_unit
    store_operand m_dst, dst
    dispatch_next

.out_of_bounds:
    mov dst, CANON_NAN_BITS
    store_operand m_dst, dst
    dispatch_next

.slow:
    call_slow_path asm_slow_path_call_builtin_string_prototype_char_code_at
end

handler CallBuiltinStringPrototypeCharAt
    temp this_value, tag, string, arg, index, code_unit, zero, result
    validate_callee_builtin BUILTIN_STRING_PROTOTYPE_CHAR_AT, .slow

    load_operand this_value, m_this_value
    extract_tag tag, this_value
    branch_ne tag, STRING_TAG, .slow
    unbox_object string, this_value

    load_operand arg, m_argument
    extract_tag tag, arg
    branch_ne tag, INT32_TAG, .slow
    unbox_int32 index, arg
    branch_negative index, .empty

    load_primitive_string_utf16_code_unit string, index, code_unit, .empty, .slow
    branch_ge_unsigned code_unit, 0x80, .slow
    assert_lt_unsigned code_unit, 0x80

    call_helper asm_helper_single_ascii_character_string, code_unit, result
    extract_tag tag, result
    assert_eq tag, STRING_TAG
    store_operand m_dst, result
    dispatch_next

.empty:
    mov zero, 0
    call_helper asm_helper_empty_string, zero, result
    extract_tag tag, result
    assert_eq tag, STRING_TAG
    store_operand m_dst, result
    dispatch_next

.slow:
    call_slow_path asm_slow_path_call_builtin_string_prototype_char_at
end

# ============================================================================
# Slow-path-only handlers
# ============================================================================

# Handlers below are pure slow-path delegations: no fast path is worthwhile
# because the operation is inherently complex (object allocation, prototype
# chain walks, etc). Having them here avoids the generic fallback handler's
# overhead of saving/restoring all temporaries.

handler GetObjectPropertyIterator
    call_slow_path asm_slow_path_get_object_property_iterator
end

handler ObjectPropertyIteratorNext
    temp it_value, tag, iterator, fast_path, expected, cache, cached_shape, receiver, current_shape, is_dict, cur_dict_gen, dict_gen, storage_kind, packed_kind, size, expected_size, validity, valid, indexed_count, next_indexed, key, key_index, named_index, named_size, named_data, exhausted, packed_kind_byte, slow_kind, scratch
    load_operand it_value, m_iterator_object
    extract_tag tag, it_value
    branch_ne tag, OBJECT_TAG, .slow
    unbox_object iterator, it_value

    load8 fast_path, [iterator, PROPERTY_NAME_ITERATOR_FAST_PATH]
    mov expected, OBJECT_PROPERTY_ITERATOR_FAST_PATH_NONE
    branch_eq fast_path, expected, .slow

    # These guards mirror PropertyNameIterator::fast_path_still_valid(). If
    # the receiver or prototype chain no longer matches the cached snapshot,
    # we drop to C++ and continue in deoptimized mode for the rest of the
    # enumeration.
    load_pair64 cache, cached_shape, [iterator, PROPERTY_NAME_ITERATOR_PROPERTY_CACHE], [iterator, PROPERTY_NAME_ITERATOR_SHAPE]
    load64 receiver, [iterator, PROPERTY_NAME_ITERATOR_OBJECT]
    load64 current_shape, [receiver, OBJECT_SHAPE]
    branch_ne current_shape, cached_shape, .slow

    load8 is_dict, [iterator, PROPERTY_NAME_ITERATOR_SHAPE_IS_DICTIONARY]
    branch_zero is_dict, .check_receiver
    load32 cur_dict_gen, [current_shape, SHAPE_DICTIONARY_GENERATION]
    load32 dict_gen, [iterator, PROPERTY_NAME_ITERATOR_SHAPE_DICTIONARY_GENERATION]
    branch_ne cur_dict_gen, dict_gen, .slow

.check_receiver:
    mov packed_kind_byte, OBJECT_PROPERTY_ITERATOR_FAST_PATH_PACKED_INDEXED
    branch_ne fast_path, packed_kind_byte, .check_proto
    load8 storage_kind, [receiver, OBJECT_INDEXED_STORAGE_KIND]
    mov packed_kind, INDEXED_STORAGE_KIND_PACKED
    branch_ne storage_kind, packed_kind, .slow
    load32 size, [receiver, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    load32 expected_size, [iterator, PROPERTY_NAME_ITERATOR_INDEXED_PROPERTY_COUNT]
    branch_ne size, expected_size, .slow

.check_proto:
    load64 validity, [iterator, PROPERTY_NAME_ITERATOR_PROTOTYPE_CHAIN_VALIDITY]
    branch_zero validity, .next_key
    load8 valid, [validity, PROTOTYPE_CHAIN_VALIDITY_VALID]
    branch_zero valid, .slow

.next_key:
    # property_values is laid out as:
    #   [receiver packed index keys..., flattened named keys...]
    load_pair32 indexed_count, next_indexed, [iterator, PROPERTY_NAME_ITERATOR_INDEXED_PROPERTY_COUNT], [iterator, PROPERTY_NAME_ITERATOR_NEXT_INDEXED_PROPERTY]
    branch_ge_unsigned next_indexed, indexed_count, .named
    load64 key, [cache, OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA]
    load64 key, [key, next_indexed, 8]
    add next_indexed, 1
    store32 [iterator, PROPERTY_NAME_ITERATOR_NEXT_INDEXED_PROPERTY], next_indexed
    store_operand m_dst_value, key
    mov scratch, BOOLEAN_FALSE
    store_operand m_dst_done, scratch
    dispatch_next

.named:
    load64 named_index, [iterator, PROPERTY_NAME_ITERATOR_NEXT_PROPERTY]
    load64 named_size, [cache, OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_SIZE]
    assert_ge_unsigned named_size, indexed_count
    sub named_size, indexed_count
    branch_ge_unsigned named_index, named_size, .done
    mov key_index, named_index
    add key_index, indexed_count
    load64 named_data, [cache, OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA]
    load64 key, [named_data, key_index, 8]
    add named_index, 1
    store64 [iterator, PROPERTY_NAME_ITERATOR_NEXT_PROPERTY], named_index
    store_operand m_dst_value, key
    mov scratch, BOOLEAN_FALSE
    store_operand m_dst_done, scratch
    dispatch_next

.done:
    load64 exhausted, [iterator, PROPERTY_NAME_ITERATOR_ITERATOR_CACHE_SLOT]
    branch_zero exhausted, .store_done
    # Return the exhausted iterator object to the bytecode-site cache so the
    # next execution of this loop can reset and reuse it.
    mov scratch, 0
    store64 [iterator, PROPERTY_NAME_ITERATOR_OBJECT], scratch
    store64 [exhausted, OBJECT_PROPERTY_ITERATOR_CACHE_REUSABLE_PROPERTY_NAME_ITERATOR], iterator
    store64 [iterator, PROPERTY_NAME_ITERATOR_ITERATOR_CACHE_SLOT], scratch

.store_done:
    mov scratch, BOOLEAN_TRUE
    store_operand m_dst_done, scratch
    dispatch_next

.slow:
    call_slow_path asm_slow_path_object_property_iterator_next
end

handler CallConstruct
    call_slow_path asm_slow_path_call_construct
end

handler NewObject
    call_slow_path asm_slow_path_new_object
end

handler CacheObjectShape
    call_slow_path asm_slow_path_cache_object_shape
end

handler InitObjectLiteralProperty
    call_slow_path asm_slow_path_init_object_literal_property
end

handler NewArray
    call_slow_path asm_slow_path_new_array
end

handler InstanceOf
    call_slow_path asm_slow_path_instance_of
end

# Fast path: if this_value register is already cached (non-empty), skip the slow path.
handler ResolveThisBinding
    temp this_value, empty
    load64 this_value, [values, THIS_VALUE_REG_OFFSET]
    mov empty, EMPTY_VALUE
    branch_eq this_value, empty, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_resolve_this_binding
end

handler GetPrivateById
    call_slow_path asm_slow_path_get_private_by_id
end

handler PutPrivateById
    call_slow_path asm_slow_path_put_private_by_id
end
