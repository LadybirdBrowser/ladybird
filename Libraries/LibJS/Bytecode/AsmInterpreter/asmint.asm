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
# Temporary registers (caller-saved, clobbered by C++ calls):
#   t0-t8    = general-purpose scratch
#   ft0-ft3  = floating-point scratch (scalar double)
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
# Clobbers t3. Jumps to fail if not a double.
macro check_is_double(reg, fail)
    extract_tag t3, reg
    and t3, NAN_BASE_TAG
    branch_eq t3, NAN_BASE_TAG, fail
end

# Check if an already-extracted tag represents a non-double type.
# Clobbers the tag register. Jumps to fail if not a double.
macro check_tag_is_double(tag, fail)
    and tag, NAN_BASE_TAG
    branch_eq tag, NAN_BASE_TAG, fail
end

# Check if both values are doubles.
# Clobbers t3, t4. Jumps to fail if either is not a double.
macro check_both_double(lhs, rhs, fail)
    extract_tag t3, lhs
    and t3, NAN_BASE_TAG
    branch_eq t3, NAN_BASE_TAG, fail
    extract_tag t4, rhs
    and t4, NAN_BASE_TAG
    branch_eq t4, NAN_BASE_TAG, fail
end

# Coerce two operands (already in t1/t2) to numeric types for arithmetic/comparison.
# If both are int32: jumps to both_int_label with t3=sign-extended lhs, t4=sign-extended rhs.
# If one or both are double: falls through with ft0=lhs as double, ft1=rhs as double.
# If either is not a number (int32 or double): jumps to fail.
# Clobbers t3, t4.
macro coerce_to_doubles(both_int_label, fail)
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .lhs_not_int
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .int_rhs_maybe_double
    # Both int32
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    jmp both_int_label
.int_rhs_maybe_double:
    # t4 already has rhs tag, known != INT32_TAG
    check_tag_is_double t4, fail
    unbox_int32 t3, t1
    int_to_double ft0, t3
    fp_mov ft1, t2
    jmp .coerced
.lhs_not_int:
    # t3 already has lhs tag, known != INT32_TAG
    check_tag_is_double t3, fail
    extract_tag t4, t2
    branch_eq t4, INT32_TAG, .double_rhs_int
    # t4 already has rhs tag, known != INT32_TAG
    check_tag_is_double t4, fail
    fp_mov ft0, t1
    fp_mov ft1, t2
    jmp .coerced
.double_rhs_int:
    fp_mov ft0, t1
    unbox_int32 t4, t2
    int_to_double ft1, t4
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
# Clobbers: t1 (x86-64), t3, ft3 (x86-64).
macro box_double_or_int32(dst, src_fpr)
    double_to_int32 t3, src_fpr, .bdi_not_int
    branch_zero t3, .bdi_check_neg_zero
    box_int32 dst, t3
    jmp .bdi_done
.bdi_check_neg_zero:
    fp_mov dst, src_fpr
    branch_negative dst, .bdi_not_int
    box_int32 dst, t3
    jmp .bdi_done
.bdi_not_int:
    canonicalize_nan dst, src_fpr
.bdi_done:
end

# Shared same-tag equality dispatch.
# Expects t3=lhs_tag (known equal to rhs_tag), t1=lhs, t2=rhs.
# For int32, boolean, object, symbol, undefined, null: bitwise compare.
# For string: pointer shortcut, else slow. For bigint: always slow.
# Falls through to .double_compare for doubles.
macro equality_same_tag(equal_label, not_equal_label, slow_label)
    branch_any_eq t3, INT32_TAG, BOOLEAN_TAG, .fast_compare
    branch_any_eq t3, OBJECT_TAG, SYMBOL_TAG, .fast_compare
    branch_eq t3, STRING_TAG, .string_compare
    branch_eq t3, BIGINT_TAG, slow_label
    # Check undefined/null: (tag & 0xFFFE) == UNDEFINED_TAG matches both.
    # Safe to clobber t3 here since all other tagged types are handled above.
    and t3, 0xFFFE
    branch_eq t3, UNDEFINED_TAG, .fast_compare
    # Must be a double
    jmp .double_compare
.string_compare:
    branch_eq t1, t2, equal_label
    jmp slow_label
.fast_compare:
    branch_eq t1, t2, equal_label
    jmp not_equal_label
end

# Compare t1/t2 as doubles with NaN awareness.
# Defines .double_compare label (referenced by equality_same_tag).
macro double_equality_compare(equal_label, not_equal_label)
.double_compare:
    fp_mov ft0, t1
    fp_mov ft1, t2
    branch_fp_unordered ft0, ft1, not_equal_label
    branch_fp_equal ft0, ft1, equal_label
    jmp not_equal_label
end

# Strict equality check core logic.
# Expects t1=lhs, t2=rhs. Extracts tags into t3/t4.
# Jumps to equal_label if definitely equal, not_equal_label if definitely not,
# or slow_label if we can't determine quickly.
# Handles: int32, boolean, undefined/null, object, symbol (bitwise compare),
# string (pointer shortcut), bigint (slow), doubles.
macro strict_equality_core(equal_label, not_equal_label, slow_label)
    extract_tag t3, t1
    extract_tag t4, t2
    branch_ne t3, t4, .diff_tags
    equality_same_tag equal_label, not_equal_label, slow_label
    double_equality_compare equal_label, not_equal_label
.diff_tags:
    # Different tags but possibly equal: int32(1) === double(1.0) is true.
    # Handle int32 vs double inline; all other tag mismatches are not equal.
    branch_eq t3, INT32_TAG, .lhs_int32_diff
    branch_eq t4, INT32_TAG, .rhs_int32_diff
    # Neither is int32. If both are doubles, compare. Otherwise not equal.
    # t3/t4 already have the tags, check them directly.
    check_tag_is_double t3, not_equal_label
    check_tag_is_double t4, not_equal_label
    jmp .double_compare
.lhs_int32_diff:
    # t4 already has rhs tag
    check_tag_is_double t4, not_equal_label
    unbox_int32 t3, t1
    int_to_double ft0, t3
    fp_mov ft1, t2
    branch_fp_equal ft0, ft1, equal_label
    jmp not_equal_label
.rhs_int32_diff:
    # t3 already has lhs tag
    check_tag_is_double t3, not_equal_label
    fp_mov ft0, t1
    unbox_int32 t4, t2
    int_to_double ft1, t4
    branch_fp_equal ft0, ft1, equal_label
    jmp not_equal_label
end

# Loose equality check core logic.
# Same as strict_equality_core but with null==undefined cross-type handling.
macro loose_equality_core(equal_label, not_equal_label, slow_label)
    extract_tag t3, t1
    extract_tag t4, t2
    branch_ne t3, t4, .diff_tags
    equality_same_tag equal_label, not_equal_label, slow_label
    double_equality_compare equal_label, not_equal_label
.diff_tags:
    # null == undefined (and vice versa): (tag & 0xFFFE) == UNDEFINED_TAG
    and t3, 0xFFFE
    branch_ne t3, UNDEFINED_TAG, .try_double
    and t4, 0xFFFE
    branch_eq t4, UNDEFINED_TAG, equal_label
    jmp slow_label
.try_double:
    check_both_double t1, t2, slow_label
    jmp .double_compare
end

# Numeric compare with coercion (for jump variants).
# Uses coerce_to_doubles to handle mixed int32+double operands.
# Expects t1=lhs, t2=rhs (NaN-boxed values).
# int_cc: signed comparison branch for int32 (branch_lt_signed, etc.)
# double_cc: unsigned comparison branch for doubles (branch_fp_less, etc.)
# Jumps to true_label/false_label/slow_label.
macro numeric_compare_coerce(int_cc, double_cc, true_label, false_label, slow_label)
    coerce_to_doubles .both_int, slow_label
    branch_fp_unordered ft0, ft1, false_label
    double_cc ft0, ft1, true_label
    jmp false_label
.both_int:
    int_cc t3, t4, true_label
    jmp false_label
end

# Numeric compare without coercion (for non-jump variants).
# Only handles both-int32 or both-double fast paths.
# Expects t1=lhs, t2=rhs (NaN-boxed values).
macro numeric_compare(int_cc, double_cc, true_label, false_label, slow_label)
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .try_double
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, slow_label
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    int_cc t3, t4, true_label
    jmp false_label
.try_double:
    # t3 already has lhs tag
    check_tag_is_double t3, slow_label
    check_is_double t2, slow_label
    fp_mov ft0, t1
    fp_mov ft1, t2
    branch_fp_unordered ft0, ft1, false_label
    double_cc ft0, ft1, true_label
    jmp false_label
end

# Epilogue for comparison/equality handlers that produce a boolean result.
# Defines .store_true, .store_false, and .slow labels.
macro boolean_result_epilogue(slow_path_func)
.store_true:
    mov t0, BOOLEAN_TRUE
    store_operand m_dst, t0
    dispatch_next
.store_false:
    mov t0, BOOLEAN_FALSE
    store_operand m_dst, t0
    dispatch_next
.slow:
    call_slow_path slow_path_func
end

# Epilogue for jump comparison/equality handlers.
# Defines .take_true, .take_false, and .slow labels.
macro jump_binary_epilogue(slow_path_func)
.slow:
    call_slow_path slow_path_func
.take_true:
    load_label t0, m_true_target
    goto_handler t0
.take_false:
    load_label t0, m_false_target
    goto_handler t0
end

# Coerce two operands (already in t1/t2) to int32 for bitwise operations.
# On success: t3 = lhs as int32, t4 = rhs as int32. Falls through.
# If either operand is not a number (int32, boolean, or double): jumps to fail.
# Clobbers t1 (on x86_64, js_to_int32 clobbers rcx=t1), t3, t4.
macro coerce_to_int32s(fail)
    extract_tag t3, t1
    branch_any_eq t3, INT32_TAG, BOOLEAN_TAG, .lhs_is_int
    check_tag_is_double t3, fail
    fp_mov ft0, t1
    js_to_int32 t3, ft0, fail
    jmp .lhs_done
.lhs_is_int:
    unbox_int32 t3, t1
.lhs_done:
    extract_tag t4, t2
    branch_any_eq t4, INT32_TAG, BOOLEAN_TAG, .rhs_is_int
    check_tag_is_double t4, fail
    fp_mov ft0, t2
    js_to_int32 t4, ft0, fail
    jmp .rhs_done
.rhs_is_int:
    unbox_int32 t4, t2
.rhs_done:
end

# Fast path for bitwise binary operations on int32/boolean/double operands.
# op_insn: the bitwise instruction to apply (xor, and, or).
macro bitwise_op(op_insn, slow_path_func)
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    coerce_to_int32s .slow
    op_insn t3, t4
    box_int32 t4, t3
    store_operand m_dst, t4
    dispatch_next
.slow:
    call_slow_path slow_path_func
end

# Validate that the callee still points at the expected builtin function.
# Jumps to fail if the call target has been replaced or is not a function.
macro validate_callee_builtin(expected_builtin, fail)
    load_operand t2, m_callee
    extract_tag t3, t2
    branch_ne t3, OBJECT_TAG, fail
    unbox_object t2, t2
    load8 t3, [t2, OBJECT_FLAGS]
    and t3, OBJECT_FLAG_IS_FUNCTION
    branch_zero t3, fail
    load8 t3, [t2, FUNCTION_OBJECT_BUILTIN_HAS_VALUE]
    branch_zero t3, fail
    load8 t3, [t2, FUNCTION_OBJECT_BUILTIN_VALUE]
    branch_ne t3, expected_builtin, fail
end

# Load a UTF-16 code unit from a primitive string with resident UTF-16 data.
# Input:
#   t2 = PrimitiveString*
#   t4 = non-negative code-unit index
# Output:
#   t0 = zero-extended code unit
# Clobbers:
#   t3, t5
# Jumps to out_of_bounds if index >= string length.
# Jumps to fail if the string would require resolving deferred data.
macro load_primitive_string_utf16_code_unit(out_of_bounds, fail)
    load8 t3, [t2, PRIMITIVE_STRING_DEFERRED_KIND]
    branch_ne t3, PRIMITIVE_STRING_DEFERRED_KIND_NONE, fail

    load64 t5, [t2, PRIMITIVE_STRING_UTF16_STRING]
    branch_zero t5, fail

    load8 t3, [t2, PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG]
    and t3, UTF16_SHORT_STRING_FLAG
    branch_zero t3, .long_storage

    load8 t3, [t2, PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG]
    shr t3, UTF16_SHORT_STRING_BYTE_COUNT_SHIFT_COUNT
    branch_ge_unsigned t4, t3, out_of_bounds
    mov t0, t2
    add t0, PRIMITIVE_STRING_UTF16_SHORT_STRING_STORAGE
    load8 t0, [t0, t4]
    jmp .done

.long_storage:
    load64 t3, [t5, UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS]
    branch_negative t3, .utf16_storage
    branch_ge_unsigned t4, t3, out_of_bounds
    add t5, UTF16_STRING_DATA_STRING_STORAGE
    load8 t0, [t5, t4]
    jmp .done

.utf16_storage:
    shl t3, 1
    shr t3, 1
    branch_ge_unsigned t4, t3, out_of_bounds
    mov t0, t4
    add t0, t4
    add t5, UTF16_STRING_DATA_STRING_STORAGE
    load16 t0, [t5, t0]

.done:
end

# Dispatch the instruction at current pc (without advancing).
# Clobbers t0.
macro dispatch_current()
    load8 t0, [pb, pc]
    jmp [dispatch, t0, 8]
end

# Walk the environment chain using a cached EnvironmentCoordinate.
# Input: m_cache field offset for the EnvironmentCoordinate in the instruction.
# Output: t3 = target environment, t2 = binding index.
# On failure (invalid cache, screwed by eval): jumps to fail_label.
# Clobbers t0, t1, t2, t3, t4.
macro walk_env_chain(m_cache_field, fail_label)
    lea t0, [pb, pc]
    add t0, m_cache_field
    load_pair32 t1, t2, [t0, ENVIRONMENT_COORDINATE_HOPS], [t0, ENVIRONMENT_COORDINATE_INDEX]
    mov t4, ENVIRONMENT_COORDINATE_INVALID
    branch_eq t1, t4, fail_label
    load64 t3, [exec_ctx, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT]
    branch_zero t1, .walk_done
.walk_loop:
    load8 t0, [t3, ENVIRONMENT_SCREWED_BY_EVAL]
    branch_nonzero t0, fail_label
    load64 t3, [t3, ENVIRONMENT_OUTER]
    sub t1, 1
    branch_nonzero t1, .walk_loop
.walk_done:
    load8 t0, [t3, ENVIRONMENT_SCREWED_BY_EVAL]
    branch_nonzero t0, fail_label
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
# Clobbers:
#   t2, t3, t4
macro pop_inline_frame_and_resume(caller_frame, value_reg)
    load_pair32 t2, t4, [exec_ctx, EXECUTION_CONTEXT_CALLER_RETURN_PC], [exec_ctx, EXECUTION_CONTEXT_CALLER_DST_RAW]
    store32 [caller_frame, EXECUTION_CONTEXT_PROGRAM_COUNTER], t2
    lea t3, [caller_frame, SIZEOF_EXECUTION_CONTEXT]
    store64 [t3, t4, 8], value_reg

    load_vm t3
    store64 [t3, VM_RUNNING_EXECUTION_CONTEXT], caller_frame
    store64 [t3, VM_INTERPRETER_STACK_TOP], exec_ctx
    inc32_mem [t3, VM_EXECUTION_GENERATION]

    mov exec_ctx, caller_frame
    load64 t3, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    load64 pb, [t3, EXECUTABLE_BYTECODE_DATA]
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    mov pc, t2
    dispatch_current
end

# ============================================================================
# Simple data movement
# ============================================================================

handler Mov
    load_operand t1, m_src
    store_operand m_dst, t1
    dispatch_next
end

handler Mov2
    load_operand t1, m_src1
    store_operand m_dst1, t1
    load_operand t2, m_src2
    store_operand m_dst2, t2
    dispatch_next
end

handler Mov3
    load_operand t1, m_src1
    store_operand m_dst1, t1
    load_operand t2, m_src2
    store_operand m_dst2, t2
    load_operand t3, m_src3
    store_operand m_dst3, t3
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
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    coerce_to_doubles .both_int, .slow
    # One or both doubles: ft0=lhs, ft1=rhs
    fp_add ft0, ft1
    box_double_or_int32 t5, ft0
    store_operand m_dst, t5
    dispatch_next
.both_int:
    # t3=lhs (sign-extended), t4=rhs (sign-extended)
    # 32-bit add with hardware overflow detection
    add32_overflow t3, t4, .overflow
    box_int32_clean t5, t3
    store_operand m_dst, t5
    dispatch_next
.overflow:
    # Int32 overflow: convert both to double and redo the operation
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    int_to_double ft0, t3
    int_to_double ft1, t4
    fp_add ft0, ft1
    fp_mov t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_add
end

# Same pattern as Add but with subtraction.
handler Sub
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    coerce_to_doubles .both_int, .slow
    # One or both doubles: ft0=lhs, ft1=rhs
    fp_sub ft0, ft1
    box_double_or_int32 t5, ft0
    store_operand m_dst, t5
    dispatch_next
.both_int:
    # t3=lhs (sign-extended), t4=rhs (sign-extended)
    sub32_overflow t3, t4, .overflow
    box_int32_clean t5, t3
    store_operand m_dst, t5
    dispatch_next
.overflow:
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    int_to_double ft0, t3
    int_to_double ft1, t4
    fp_sub ft0, ft1
    fp_mov t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_sub
end

# Same pattern as Add but with multiplication.
# Extra complexity: 0 * negative = -0.0 (must produce negative zero double).
handler Mul
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    coerce_to_doubles .both_int, .slow
    # One or both doubles: ft0=lhs, ft1=rhs
    fp_mul ft0, ft1
    box_double_or_int32 t5, ft0
    store_operand m_dst, t5
    dispatch_next
.both_int:
    # t3=lhs (sign-extended), t4=rhs (sign-extended)
    mul32_overflow t3, t4, .overflow
    branch_nonzero t3, .store_int
    # Result is 0: check if either operand was negative -> -0.0
    unbox_int32 t5, t1
    or t5, t4
    branch_negative t5, .negative_zero
.store_int:
    box_int32_clean t5, t3
    store_operand m_dst, t5
    dispatch_next
.negative_zero:
    mov t5, NEGATIVE_ZERO
    store_operand m_dst, t5
    dispatch_next
.overflow:
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    int_to_double ft0, t3
    int_to_double ft1, t4
    fp_mul ft0, ft1
    fp_mov t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_mul
end

# ============================================================================
# Control flow
# ============================================================================

handler Jump
    load_label t0, m_target
    goto_handler t0
end

# Conditional jumps: check boolean first (most common), then int32, then slow path.
# For JumpIf/JumpTrue/JumpFalse, a boolean's truth value is just bit 0.
# For int32, any nonzero low 32 bits means truthy.
handler JumpIf
    load_operand t1, m_condition
    extract_tag t2, t1
    # Boolean fast path
    branch_eq t2, BOOLEAN_TAG, .is_bool
    # Int32 fast path
    branch_eq t2, INT32_TAG, .is_int32
    # Slow path: call helper to convert to boolean
    call_helper asm_helper_to_boolean
    branch_nonzero t0, .take_true
    jmp .take_false
.is_bool:
    branch_bits_set t1, 1, .take_true
    jmp .take_false
.is_int32:
    branch_nonzero32 t1, .take_true
    jmp .take_false
.take_true:
    load_label t0, m_true_target
    goto_handler t0
.take_false:
    load_label t0, m_false_target
    goto_handler t0
end

handler JumpTrue
    load_operand t1, m_condition
    extract_tag t2, t1
    branch_eq t2, BOOLEAN_TAG, .is_bool
    branch_eq t2, INT32_TAG, .is_int32
    call_helper asm_helper_to_boolean
    branch_nonzero t0, .take
    dispatch_next
.is_bool:
    branch_bits_set t1, 1, .take
    dispatch_next
.is_int32:
    branch_nonzero32 t1, .take
    dispatch_next
.take:
    load_label t0, m_target
    goto_handler t0
end

handler JumpFalse
    load_operand t1, m_condition
    extract_tag t2, t1
    branch_eq t2, BOOLEAN_TAG, .is_bool
    branch_eq t2, INT32_TAG, .is_int32
    call_helper asm_helper_to_boolean
    branch_zero t0, .take
    dispatch_next
.is_bool:
    branch_bits_clear t1, 1, .take
    dispatch_next
.is_int32:
    branch_zero32 t1, .take
    dispatch_next
.take:
    load_label t0, m_target
    goto_handler t0
end

# Nullish check: undefined and null tags differ only in bit 0,
# so (tag & 0xFFFE) == UNDEFINED_TAG matches both.
handler JumpNullish
    load_operand t1, m_condition
    # Nullish: (tag & 0xFFFE) == 0x7FFE (matches undefined=0x7FFE and null=0x7FFF)
    extract_tag t2, t1
    and t2, 0xFFFE
    branch_eq t2, UNDEFINED_TAG, .nullish
    load_label t0, m_false_target
    goto_handler t0
.nullish:
    load_label t0, m_true_target
    goto_handler t0
end

handler JumpUndefined
    load_operand t1, m_condition
    mov t0, UNDEFINED_SHIFTED
    branch_eq t1, t0, .is_undefined
    load_label t0, m_false_target
    goto_handler t0
.is_undefined:
    load_label t0, m_true_target
    goto_handler t0
end


# Jump comparison handlers: use numeric_compare_coerce (handles mixed int32+double)
# combined with jump_binary_epilogue (provides .take_true, .take_false, .slow labels).
handler JumpLessThan
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare_coerce branch_lt_signed, branch_fp_less, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_less_than
end

handler JumpGreaterThan
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare_coerce branch_gt_signed, branch_fp_greater, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_greater_than
end

handler JumpLessThanEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare_coerce branch_le_signed, branch_fp_less_or_equal, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_less_than_equals
end

handler JumpGreaterThanEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare_coerce branch_ge_signed, branch_fp_greater_or_equal, .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_greater_than_equals
end

handler JumpLooselyEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    loose_equality_core .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_loosely_equals
end

handler JumpLooselyInequals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    loose_equality_core .take_false, .take_true, .slow
    jump_binary_epilogue asm_slow_path_jump_loosely_inequals
end

handler JumpStrictlyEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    strict_equality_core .take_true, .take_false, .slow
    jump_binary_epilogue asm_slow_path_jump_strictly_equals
end

handler JumpStrictlyInequals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    strict_equality_core .take_false, .take_true, .slow
    jump_binary_epilogue asm_slow_path_jump_strictly_inequals
end

# Fast path for ++x: int32 + 1 with overflow check.
# On overflow, convert to double and add 1.0.
handler Increment
    load_operand t1, m_dst
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .slow
    unbox_int32 t3, t1
    add32_overflow t3, 1, .overflow
    box_int32_clean t4, t3
    store_operand m_dst, t4
    dispatch_next
.overflow:
    unbox_int32 t3, t1
    int_to_double ft0, t3
    mov t0, DOUBLE_ONE
    fp_mov ft1, t0
    fp_add ft0, ft1
    fp_mov t4, ft0
    store_operand m_dst, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_increment
end

# Fast path for --x: int32 - 1 with overflow check.
handler Decrement
    load_operand t1, m_dst
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .slow
    unbox_int32 t3, t1
    sub32_overflow t3, 1, .overflow
    box_int32_clean t4, t3
    store_operand m_dst, t4
    dispatch_next
.overflow:
    unbox_int32 t3, t1
    int_to_double ft0, t3
    mov t0, DOUBLE_ONE
    fp_mov ft1, t0
    fp_sub ft0, ft1
    fp_mov t4, ft0
    store_operand m_dst, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_decrement
end

handler Not
    load_operand t1, m_src
    extract_tag t2, t1
    # Boolean fast path
    branch_eq t2, BOOLEAN_TAG, .is_bool
    # Int32 fast path
    branch_eq t2, INT32_TAG, .is_int32
    # Undefined/null -> !nullish = true
    mov t3, t2
    and t3, 0xFFFE
    branch_eq t3, UNDEFINED_TAG, .store_true
    # Slow path for remaining types (object, string, etc)
    # NB: Objects go through slow path to handle [[IsHTMLDDA]]
    call_helper asm_helper_to_boolean
    branch_zero t0, .store_true
    jmp .store_false
.is_bool:
    branch_bits_clear t1, 1, .store_true
    jmp .store_false
.is_int32:
    branch_zero32 t1, .store_true
    jmp .store_false
.store_true:
    mov t0, BOOLEAN_TRUE
    store_operand m_dst, t0
    dispatch_next
.store_false:
    mov t0, BOOLEAN_FALSE
    store_operand m_dst, t0
    dispatch_next
end

# ============================================================================
# Return / function call
# ============================================================================

handler Return
    # Empty is the internal "no explicit value" marker. Returning it from
    # bytecode means "return undefined" at the JS level.
    load_operand t0, m_value
    mov t2, EMPTY_TAG_SHIFTED
    branch_ne t0, t2, .value_ready
    mov t0, UNDEFINED_SHIFTED
.value_ready:
    # Inline JS-to-JS calls resume the caller directly from asm. Top-level
    # returns instead exit back to the outer interpreter entry point.
    load64 t1, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    branch_zero t1, .top_level
    pop_inline_frame_and_resume t1, t0
.top_level:
    # Top-level return matches VM::run_executable(): write return_value,
    # clear the exception slot, and leave the asm interpreter entirely.
    # values[3] = return_value, values[1] = empty (clear exception)
    store64 [values, 24], t0
    store64 [values, 8], t2
    exit
end

# Like Return, but does not clear the exception register (values[1]).
# Used at the end of a function body (after all user code).
handler End
    # End shares the same inline-frame unwind logic as Return. The only
    # top-level difference is that End preserves the current exception slot.
    load_operand t0, m_value
    mov t2, EMPTY_TAG_SHIFTED
    branch_ne t0, t2, .value_ready
    mov t0, UNDEFINED_SHIFTED
.value_ready:
    # Inline frame: resume the caller immediately.
    load64 t1, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    branch_zero t1, .top_level
    pop_inline_frame_and_resume t1, t0
.top_level:
    # Top-level end: publish the return value and exit without touching
    # values[1], since End does not model a user-visible `return` opcode.
    store64 [values, 24], t0
    exit
end

# Loads running_execution_context().lexical_environment into dst,
# NaN-boxed as a cell pointer.
handler GetLexicalEnvironment
    load64 t0, [exec_ctx, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT]
    mov t1, CELL_TAG_SHIFTED
    or t0, t1
    store_operand m_dst, t0
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
    walk_env_chain m_cache, .slow
    # t3 = target environment, t2 = binding index
    load64 t0, [t3, BINDINGS_DATA_PTR]
    mul t2, t2, SIZEOF_BINDING
    add t0, t2
    # Check binding is initialized (TDZ)
    load8 t1, [t0, BINDING_INITIALIZED]
    branch_zero t1, .slow
    load64 t1, [t0, BINDING_VALUE]
    store_operand m_dst, t1
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_binding
end

# Inline environment chain walk + direct binding value load.
handler GetInitializedBinding
    walk_env_chain m_cache, .slow
    # t3 = target environment, t2 = binding index
    load64 t0, [t3, BINDINGS_DATA_PTR]
    mul t2, t2, SIZEOF_BINDING
    add t0, t2
    load64 t1, [t0, BINDING_VALUE]
    store_operand m_dst, t1
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_initialized_binding
end

# Inline environment chain walk + initialize binding (set value + initialized=true).
handler InitializeLexicalBinding
    walk_env_chain m_cache, .slow
    # t3 = target environment, t2 = binding index
    load64 t3, [t3, BINDINGS_DATA_PTR]
    mul t2, t2, SIZEOF_BINDING
    add t3, t2
    # Store source value into binding
    # NB: load_operand clobbers t0, so binding address must be in t3.
    load_operand t1, m_src
    store64 [t3, BINDING_VALUE], t1
    # Set initialized = true
    mov t1, 1
    store8 [t3, BINDING_INITIALIZED], t1
    dispatch_next
.slow:
    call_slow_path asm_slow_path_initialize_lexical_binding
end

# Inline environment chain walk + set mutable binding.
handler SetLexicalBinding
    walk_env_chain m_cache, .slow
    # t3 = target environment, t2 = binding index
    load64 t3, [t3, BINDINGS_DATA_PTR]
    mul t2, t2, SIZEOF_BINDING
    add t3, t2
    # Check initialized (TDZ)
    load8 t1, [t3, BINDING_INITIALIZED]
    branch_zero t1, .slow
    # Check mutable
    load8 t1, [t3, BINDING_MUTABLE]
    branch_zero t1, .slow
    # Store source value into binding
    # NB: load_operand clobbers t0, so binding address must be in t3.
    load_operand t1, m_src
    store64 [t3, BINDING_VALUE], t1
    dispatch_next
.slow:
    call_slow_path asm_slow_path_set_lexical_binding
end

# x++: save original to dst first, then increment src in-place.
handler PostfixIncrement
    load_operand t1, m_src
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .slow
    # Save original value to dst (the "postfix" part)
    store_operand m_dst, t1
    # Increment in-place: src = src + 1
    unbox_int32 t3, t1
    add32_overflow t3, 1, .overflow_after_store
    box_int32_clean t4, t3
    store_operand m_src, t4
    dispatch_next
.overflow_after_store:
    unbox_int32 t3, t1
    int_to_double ft0, t3
    mov t0, DOUBLE_ONE
    fp_mov ft1, t0
    fp_add ft0, ft1
    fp_mov t4, ft0
    store_operand m_src, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_postfix_increment
end

# Division result is stored as int32 when representable (e.g. 6/3 = 2),
# matching the Value(double) constructor's behavior. We don't use
# coerce_to_doubles here because we never need the both-int32 branch --
# both operands go straight to FP regs.
handler Div
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    extract_tag t3, t1
    branch_eq t3, INT32_TAG, .lhs_is_int32
    # t3 already has lhs tag
    check_tag_is_double t3, .slow
    fp_mov ft0, t1
    jmp .lhs_ok
.lhs_is_int32:
    unbox_int32 t3, t1
    int_to_double ft0, t3
.lhs_ok:
    # ft0 = lhs as double
    extract_tag t3, t2
    branch_eq t3, INT32_TAG, .rhs_is_int32
    # t3 already has rhs tag
    check_tag_is_double t3, .slow
    fp_mov ft1, t2
    jmp .do_div
.rhs_is_int32:
    unbox_int32 t3, t2
    int_to_double ft1, t3
.do_div:
    # ft0 = lhs, ft1 = rhs
    fp_div ft0, ft1
    # Try to store result as int32 if it's an integer in i32 range.
    # NB: We can't use js_to_int32 here because fjcvtzs applies modular
    # reduction (e.g. 2^33 -> 0) which is wrong -- we need a strict
    # round-trip check: truncate to int32, convert back, compare.
    double_to_int32 t5, ft0, .store_double
    # Exclude negative zero: -0.0 truncates to 0 but must stay double.
    branch_nonzero t5, .store_int
    fp_mov t5, ft0
    branch_negative t5, .store_double
.store_int:
    box_int32 t5, t5
    store_operand m_dst, t5
    dispatch_next
.store_double:
    canonicalize_nan t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_div
end

# Numeric comparison handlers: use numeric_compare macro for both-int32 and
# both-double fast paths, fall back to slow path for mixed/non-numeric types.
# The boolean_result_epilogue macro provides .store_true, .store_false, .slow labels.
handler LessThan
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare branch_lt_signed, branch_fp_less, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_less_than
end

handler LessThanEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare branch_le_signed, branch_fp_less_or_equal, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_less_than_equals
end

handler GreaterThan
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare branch_gt_signed, branch_fp_greater, .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_greater_than
end

handler GreaterThanEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    numeric_compare branch_ge_signed, branch_fp_greater_or_equal, .store_true, .store_false, .slow
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
    load_operand t1, m_src
    # Check if int32
    extract_tag t0, t1
    branch_eq t0, INT32_TAG, .done
    # t0 already has tag; check if double
    check_tag_is_double t0, .slow
.done:
    store_operand m_dst, t1
    dispatch_next
.slow:
    call_slow_path asm_slow_path_unary_plus
end

# Check if value is TDZ (empty). If not, just continue.
handler ThrowIfTDZ
    load_operand t1, m_src
    mov t0, EMPTY_VALUE
    branch_eq t1, t0, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_throw_if_tdz
end

# Check if value is an object. Only throws on non-object (rare).
handler ThrowIfNotObject
    load_operand t1, m_src
    extract_tag t0, t1
    branch_ne t0, OBJECT_TAG, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_throw_if_not_object
end

# Check if value is nullish (undefined or null). Only throws on nullish (rare).
handler ThrowIfNullish
    load_operand t1, m_src
    extract_tag t0, t1
    and t0, 0xFFFE
    branch_eq t0, UNDEFINED_TAG, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_throw_if_nullish
end

# Fast path for int32: ~value
handler BitwiseNot
    load_operand t1, m_src
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .slow
    # NOT the low 32 bits (not32 zeros upper 32), then re-box
    mov t3, t1
    not32 t3
    box_int32_clean t3, t3
    store_operand m_dst, t3
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
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .slow
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    and t4, 31
    shl t3, t4
    box_int32 t4, t3
    store_operand m_dst, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_left_shift
end

handler RightShift
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .slow
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    and t4, 31
    sar t3, t4
    box_int32 t4, t3
    store_operand m_dst, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_right_shift
end

# Unsigned right shift: result is always unsigned, so values > INT32_MAX
# must be stored as double (can't fit in a signed int32 NaN-box).
handler UnsignedRightShift
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .slow
    # u32 result = (u32)lhs >> (rhs % 32)
    mov t3, t1
    and t3, 0xFFFFFFFF
    unbox_int32 t4, t2
    and t4, 31
    shr t3, t4
    # If result > INT32_MAX, store as double
    branch_bit_set t3, 31, .as_double
    box_int32_clean t3, t3
    store_operand m_dst, t3
    dispatch_next
.as_double:
    int_to_double ft0, t3
    fp_mov t3, ft0
    store_operand m_dst, t3
    dispatch_next
.slow:
    call_slow_path asm_slow_path_unsigned_right_shift
end

# Modulo: int32-only fast path for non-negative dividend.
# Negative dividend falls to slow path to handle -0 and INT_MIN correctly.
handler Mod
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .slow
    unbox_int32 t3, t1
    unbox_int32 t4, t2
    # Check d == 0
    branch_zero t4, .slow
    # Check n >= 0 (positive fast path avoids INT_MIN/-1 and negative zero)
    branch_negative t3, .slow
    # divmod: quotient in t0, remainder in t2
    divmod t0, t2, t3, t4
    box_int32 t2, t2
    store_operand m_dst, t2
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
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    strict_equality_core .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_strictly_equals
end

handler StrictlyInequals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    strict_equality_core .store_false, .store_true, .slow
    boolean_result_epilogue asm_slow_path_strictly_inequals
end

# Inline environment chain walk + get callee and this.
handler GetCalleeAndThisFromEnvironment
    walk_env_chain m_cache, .slow
    # t3 = target environment, t2 = binding index
    load64 t0, [t3, BINDINGS_DATA_PTR]
    mul t2, t2, SIZEOF_BINDING
    add t0, t2
    # TDZ state lives in Binding.initialized; the value slot itself starts as
    # undefined, so checking for EMPTY would miss cached second-hit calls.
    load8 t1, [t0, BINDING_INITIALIZED]
    branch_zero t1, .slow
    load64 t1, [t0, BINDING_VALUE]
    store_operand m_callee, t1
    # this = undefined (DeclarativeEnvironment.with_base_object() always returns nullptr)
    mov t0, UNDEFINED_SHIFTED
    store_operand m_this_value, t0
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_callee_and_this
end

handler LooselyEquals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    loose_equality_core .store_true, .store_false, .slow
    boolean_result_epilogue asm_slow_path_loosely_equals
end

handler LooselyInequals
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    loose_equality_core .store_false, .store_true, .slow
    boolean_result_epilogue asm_slow_path_loosely_inequals
end

handler UnaryMinus
    load_operand t1, m_src
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .try_double
    unbox_int32 t3, t1
    # -0 check: if value is 0, result is -0.0 (double)
    branch_zero t3, .negative_zero
    # 32-bit negate with overflow detection (INT32_MIN)
    neg32_overflow t3, .overflow
    box_int32_clean t4, t3
    store_operand m_dst, t4
    dispatch_next
.negative_zero:
    mov t0, NEGATIVE_ZERO
    store_operand m_dst, t0
    dispatch_next
.overflow:
    # INT32_MIN: -(-2147483648) = 2147483648.0
    int_to_double ft0, t3
    fp_mov t4, ft0
    store_operand m_dst, t4
    dispatch_next
.try_double:
    # t2 already has tag
    check_tag_is_double t2, .slow
    # Negate double: flip sign bit (bit 63)
    toggle_bit t1, 63
    store_operand m_dst, t1
    dispatch_next
.slow:
    call_slow_path asm_slow_path_unary_minus
end

# x--: save original to dst first, then decrement src in-place.
handler PostfixDecrement
    load_operand t1, m_src
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .slow
    # Save original value to dst (the "postfix" part)
    store_operand m_dst, t1
    # Decrement in-place: src = src - 1
    unbox_int32 t3, t1
    sub32_overflow t3, 1, .overflow_after_store
    box_int32_clean t4, t3
    store_operand m_src, t4
    dispatch_next
.overflow_after_store:
    unbox_int32 t3, t1
    int_to_double ft0, t3
    mov t0, DOUBLE_ONE
    fp_mov ft1, t0
    fp_sub ft0, ft1
    fp_mov t4, ft0
    store_operand m_src, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_postfix_decrement
end

handler ToInt32
    load_operand t1, m_value
    extract_tag t2, t1
    branch_ne t2, INT32_TAG, .try_double
    # Already int32, just copy
    store_operand m_dst, t1
    dispatch_next
.try_double:
    # t2 already has tag; check if double (copy first, t2 needed at .try_boolean)
    mov t3, t2
    check_tag_is_double t3, .try_boolean
    # Convert double to int32 using JS ToInt32 semantics.
    # With FEAT_JSCVT: fjcvtzs handles everything in one instruction.
    # Without: truncate + round-trip check, slow path on mismatch.
    fp_mov ft0, t1
    js_to_int32 t2, ft0, .slow
    box_int32_clean t2, t2
    store_operand m_dst, t2
    dispatch_next
.try_boolean:
    branch_ne t2, BOOLEAN_TAG, .slow
    # Convert boolean to int32: false -> 0, true -> 1
    and t1, 1
    box_int32_clean t1, t1
    store_operand m_dst, t1
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
    # Only fast-path Normal puts (not Getter/Setter/Own)
    load8 t0, [pb, pc, m_kind]
    branch_ne t0, PUT_KIND_NORMAL, .slow
    load_operand t1, m_base
    load_operand t2, m_property
    # Check base is an object
    extract_tag t3, t1
    branch_ne t3, OBJECT_TAG, .slow
    # Check property is non-negative int32
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .slow
    mov t4, t2
    and t4, 0xFFFFFFFF
    # Check high bit (negative int32)
    branch_bit_set t4, 31, .slow
    # Extract Object*
    unbox_object t3, t1
    # Check IsTypedArray flag -- branch to C++ helper early
    load8 t0, [t3, OBJECT_FLAGS]
    branch_bits_set t0, OBJECT_FLAG_IS_TYPED_ARRAY, .try_typed_array
    # Check !may_interfere_with_indexed_property_access
    branch_bits_set t0, OBJECT_FLAG_MAY_INTERFERE, .slow
    # Packed is the hot path: existing elements can be overwritten directly.
    load8 t0, [t3, OBJECT_INDEXED_STORAGE_KIND]
    branch_ne t0, INDEXED_STORAGE_KIND_PACKED, .not_packed
    # Check index vs array_like_size
    load32 t5, [t3, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned t4, t5, .slow
    load64 t5, [t3, OBJECT_INDEXED_ELEMENTS]
    load_operand t1, m_src
    store64 [t5, t4, 8], t1
    dispatch_next
.not_packed:
    branch_ne t0, INDEXED_STORAGE_KIND_HOLEY, .slow
    # Holey arrays need a slot load to distinguish existing elements from holes.
    load32 t5, [t3, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned t4, t5, .slow
    load64 t5, [t3, OBJECT_INDEXED_ELEMENTS]
    branch_zero t5, .try_holey_array_slow
    mov t0, t5
    sub t0, 8
    load32 t0, [t0, 0]
    branch_ge_unsigned t4, t0, .try_holey_array_slow
    load64 t1, [t5, t4, 8]
    mov t0, EMPTY_TAG_SHIFTED
    branch_eq t1, t0, .try_holey_array_slow
    load_operand t1, m_src
    store64 [t5, t4, 8], t1
    dispatch_next
.try_holey_array_slow:
    call_interp asm_try_put_by_value_holey_array
    branch_nonzero t0, .slow
    dispatch_next
.try_typed_array:
    # t3 = Object*, t4 = index (u32, non-negative)
    # Load cached data pointer (pre-computed: buffer.data() + byte_offset)
    # nullptr means uncached -> C++ helper will resolve the access.
    load64 t5, [t3, TYPED_ARRAY_CACHED_DATA_PTR]
    branch_zero t5, .try_typed_array_slow
    # Cached pointers only exist for fixed-length typed arrays, so array_length
    # is known to hold a concrete u32 value here.
    load32 t0, [t3, TYPED_ARRAY_ARRAY_LENGTH_VALUE]
    branch_ge_unsigned t4, t0, .slow
    # t5 = data base pointer, t4 = index
    # Load kind into t2 before load_operand clobbers t0
    load8 t2, [t3, TYPED_ARRAY_KIND]
    # Load source value into t1 (clobbers t0)
    load_operand t1, m_src
    # Check if source is int32
    extract_tag t0, t1
    branch_eq t0, INT32_TAG, .ta_store_int32
    # Non-int32 value: only handle float typed arrays with double sources
    branch_eq t2, TYPED_ARRAY_KIND_FLOAT32, .ta_store_float32
    branch_ne t2, TYPED_ARRAY_KIND_FLOAT64, .try_typed_array_slow
    # Compute store address before check_is_double clobbers t4
    mov t0, t4
    shl t0, 3
    add t0, t5
    # Verify value is actually a double
    check_is_double t1, .try_typed_array_slow
    # Float64Array: store raw double bits
    store64 [t0, 0], t1
    dispatch_next
.ta_store_float32:
    mov t0, t4
    shl t0, 2
    add t0, t5
    check_is_double t1, .try_typed_array_slow
    fp_mov ft0, t1
    double_to_float ft0, ft0
    storef32 [t0, 0], ft0
    dispatch_next
.ta_store_int32:
    # t1 = NaN-boxed int32, sign-extend it into t0
    unbox_int32 t0, t1
    # Dispatch on kind (in t2)
    branch_any_eq t2, TYPED_ARRAY_KIND_INT32, TYPED_ARRAY_KIND_UINT32, .ta_put_int32
    branch_eq t2, TYPED_ARRAY_KIND_FLOAT32, .ta_put_float32
    branch_eq t2, TYPED_ARRAY_KIND_UINT8_CLAMPED, .ta_put_uint8_clamped
    branch_any_eq t2, TYPED_ARRAY_KIND_UINT8, TYPED_ARRAY_KIND_INT8, .ta_put_uint8
    branch_any_eq t2, TYPED_ARRAY_KIND_UINT16, TYPED_ARRAY_KIND_INT16, .ta_put_uint16
    jmp .try_typed_array_slow
.ta_put_int32:
    store32 [t5, t4, 4], t0
    dispatch_next
.ta_put_float32:
    int_to_double ft0, t0
    double_to_float ft0, ft0
    mov t3, t4
    shl t3, 2
    add t3, t5
    storef32 [t3, 0], ft0
    dispatch_next
.ta_put_uint8_clamped:
    branch_negative t0, .ta_put_uint8_clamped_zero
    mov t3, 255
    branch_ge_unsigned t0, t3, .ta_put_uint8_clamped_max
    store8 [t5, t4], t0
    dispatch_next
.ta_put_uint8_clamped_zero:
    mov t0, 0
    store8 [t5, t4], t0
    dispatch_next
.ta_put_uint8_clamped_max:
    mov t0, 255
    store8 [t5, t4], t0
    dispatch_next
.ta_put_uint8:
    store8 [t5, t4], t0
    dispatch_next
.ta_put_uint16:
    mov t3, t4
    add t3, t4
    add t3, t5
    store16 [t3, 0], t0
    dispatch_next
.try_typed_array_slow:
    call_interp asm_try_put_by_value_typed_array
    branch_nonzero t0, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_put_by_value
end

# Inline cache fast path for property access (own + prototype chain).
handler GetById
    load_operand t1, m_base
    # Check base is an object
    extract_tag t2, t1
    branch_ne t2, OBJECT_TAG, .try_cache
    # Extract Object* from NaN-boxed value (sign-extend lower 48 bits)
    unbox_object t3, t1
    # Load Object.m_shape
    load64 t4, [t3, OBJECT_SHAPE]
    # Get PropertyLookupCache* (direct pointer from instruction stream)
    load64 t5, [pb, pc, m_cache]
    # Check entry[0].shape and entry[0].prototype.
    load_pair64 t1, t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_ne t1, t4, .try_cache
    branch_nonzero t0, .proto
    # Check dictionary generation matches
    load_pair32 t1, t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .try_cache
    # IC hit! Load property value via get_direct (own property)
    load64 t5, [t3, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t1, 8]
    # Check value is not an accessor
    extract_tag t2, t0
    branch_eq t2, ACCESSOR_TAG, .try_cache
    store_operand m_dst, t0
    dispatch_next
.proto:
    # t0 = prototype Object*, t4 = object's shape, t5 = PLC base
    # Check prototype chain validity (direct pointer, null = invalid)
    load64 t1, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE_CHAIN_VALIDITY]
    branch_zero t1, .try_cache
    load8 t2, [t1, PROTOTYPE_CHAIN_VALIDITY_VALID]
    branch_zero t2, .try_cache
    # Check dictionary generation matches
    load_pair32 t2, t1, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t4, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t1, t4, .try_cache
    # IC hit! Load property value via get_direct (from prototype)
    load64 t1, [t0, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t1, t2, 8]
    # Check value is not an accessor
    extract_tag t1, t0
    branch_eq t1, ACCESSOR_TAG, .try_cache
    store_operand m_dst, t0
    dispatch_next
.try_cache:
    # Try all cache entries via C++ helper
    call_interp asm_try_get_by_id_cache
    branch_zero t0, .done
.slow:
    call_slow_path asm_slow_path_get_by_id
.done:
    dispatch_next
end

# Inline cache fast path for own-property store (ChangeOwnProperty).
handler PutById
    load_operand t1, m_base
    # Check base is an object
    extract_tag t2, t1
    branch_ne t2, OBJECT_TAG, .try_cache
    # Extract Object* from NaN-boxed value (sign-extend lower 48 bits)
    unbox_object t3, t1
    # Load Object.m_shape
    load64 t4, [t3, OBJECT_SHAPE]
    # Get PropertyLookupCache* (direct pointer from instruction stream)
    load64 t5, [pb, pc, m_cache]
    # Check entry[0].shape and entry[0].prototype.
    load_pair64 t1, t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_ne t1, t4, .try_cache
    branch_nonzero t0, .try_cache
    # Check dictionary generation matches
    load_pair32 t1, t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .try_cache
    # Check current value at property_offset is not an accessor
    load64 t5, [t3, OBJECT_NAMED_PROPERTIES]
    load64 t2, [t5, t1, 8]
    extract_tag t4, t2
    branch_eq t4, ACCESSOR_TAG, .try_cache
    # IC hit! Store new value via put_direct
    # Save property offset in t4 before load_operand clobbers t0 (rax)
    mov t4, t1
    load_operand t1, m_src
    store64 [t5, t4, 8], t1
    dispatch_next
.try_cache:
    # Try all cache entries via C++ helper (handles AddOwnProperty)
    call_interp asm_try_put_by_id_cache
    branch_zero t0, .done
.slow:
    call_slow_path asm_slow_path_put_by_id
.done:
    dispatch_next
end

# Fast path for array[int32_index] with Packed/Holey indexed storage.
handler GetByValue
    load_operand t1, m_base
    load_operand t2, m_property
    # Check base is an object
    extract_tag t3, t1
    branch_ne t3, OBJECT_TAG, .slow
    # Check property is non-negative int32
    extract_tag t4, t2
    branch_ne t4, INT32_TAG, .slow
    mov t4, t2
    and t4, 0xFFFFFFFF
    # t4 = index (zero-extended u32)
    # Check high bit (negative int32)
    branch_bit_set t4, 31, .slow
    # Extract Object*
    unbox_object t3, t1
    # Check IsTypedArray flag -- branch to C++ helper early
    load8 t0, [t3, OBJECT_FLAGS]
    branch_bits_set t0, OBJECT_FLAG_IS_TYPED_ARRAY, .try_typed_array
    # Check !may_interfere_with_indexed_property_access
    branch_bits_set t0, OBJECT_FLAG_MAY_INTERFERE, .slow
    # Packed is the hot path: in-bounds elements are always present.
    load8 t0, [t3, OBJECT_INDEXED_STORAGE_KIND]
    branch_ne t0, INDEXED_STORAGE_KIND_PACKED, .not_packed
    # Check index < array_like_size
    load32 t5, [t3, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned t4, t5, .slow
    load64 t5, [t3, OBJECT_INDEXED_ELEMENTS]
    load64 t0, [t5, t4, 8]
    # NB: No accessor check needed -- Packed/Holey storage
    #     can only hold default-attributed data properties.
    store_operand m_dst, t0
    dispatch_next
.not_packed:
    branch_ne t0, INDEXED_STORAGE_KIND_HOLEY, .slow
    # Holey arrays need a slot load to distinguish present elements from holes.
    load32 t5, [t3, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    branch_ge_unsigned t4, t5, .slow
    load64 t5, [t3, OBJECT_INDEXED_ELEMENTS]
    branch_zero t5, .slow
    mov t0, t5
    sub t0, 8
    load32 t0, [t0, 0]
    branch_ge_unsigned t4, t0, .slow
    load64 t0, [t5, t4, 8]
    mov t5, EMPTY_TAG_SHIFTED
    branch_eq t0, t5, .slow
    store_operand m_dst, t0
    dispatch_next
.try_typed_array:
    # t3 = Object*, t4 = index (u32, non-negative)
    # Load cached data pointer (pre-computed: buffer.data() + byte_offset)
    # nullptr means uncached -> C++ helper will resolve the access.
    load64 t5, [t3, TYPED_ARRAY_CACHED_DATA_PTR]
    branch_zero t5, .try_typed_array_slow
    # Cached pointers only exist for fixed-length typed arrays, so array_length
    # is known to hold a concrete u32 value here.
    load32 t0, [t3, TYPED_ARRAY_ARRAY_LENGTH_VALUE]
    branch_ge_unsigned t4, t0, .try_typed_array_slow
    # t5 = data base pointer, t4 = index
    # Dispatch on kind
    load8 t0, [t3, TYPED_ARRAY_KIND]
    branch_eq t0, TYPED_ARRAY_KIND_INT32, .ta_int32
    branch_any_eq t0, TYPED_ARRAY_KIND_UINT8, TYPED_ARRAY_KIND_UINT8_CLAMPED, .ta_uint8
    branch_eq t0, TYPED_ARRAY_KIND_UINT16, .ta_uint16
    branch_eq t0, TYPED_ARRAY_KIND_INT8, .ta_int8
    branch_eq t0, TYPED_ARRAY_KIND_INT16, .ta_int16
    branch_eq t0, TYPED_ARRAY_KIND_UINT32, .ta_uint32
    branch_eq t0, TYPED_ARRAY_KIND_FLOAT32, .ta_float32
    branch_eq t0, TYPED_ARRAY_KIND_FLOAT64, .ta_float64
    jmp .try_typed_array_slow
.ta_int32:
    load32 t0, [t5, t4, 4]
    jmp .ta_box_int32
.ta_uint8:
    load8 t0, [t5, t4]
    jmp .ta_box_int32
.ta_uint16:
    mov t0, t4
    add t0, t4
    load16 t0, [t5, t0]
    jmp .ta_box_int32
.ta_int8:
    load8s t0, [t5, t4]
    jmp .ta_box_int32
.ta_int16:
    mov t0, t4
    add t0, t4
    load16s t0, [t5, t0]
    jmp .ta_box_int32
.ta_float32:
    mov t0, t4
    shl t0, 2
    add t0, t5
    loadf32 ft0, [t0, 0]
    float_to_double ft0, ft0
    fp_mov t1, ft0
    mov t3, NEGATIVE_ZERO
    branch_eq t1, t3, .ta_f64_as_double
    double_to_int32 t0, ft0, .ta_f64_as_double
    branch_nonzero t0, .ta_f64_as_int
    jmp .ta_f64_as_int
.ta_float64:
    # index * 8 for f64 elements
    mov t0, t4
    shl t0, 3
    add t0, t5
    load64 t1, [t0, 0]
    fp_mov ft0, t1
    # Exclude negative zero early (t1 gets clobbered by double_to_int32)
    mov t3, NEGATIVE_ZERO
    branch_eq t1, t3, .ta_f64_as_double
    # Try to store as int32 if the value is an integer in i32 range.
    double_to_int32 t0, ft0, .ta_f64_as_double
    branch_nonzero t0, .ta_f64_as_int
    # double_to_int32 succeeded with 0 -- this is +0.0, box as int
.ta_f64_as_int:
    box_int32 t3, t0
    store_operand m_dst, t3
    dispatch_next
.ta_f64_as_double:
    canonicalize_nan t0, ft0
    store_operand m_dst, t0
    dispatch_next
.ta_uint32:
    load32 t0, [t5, t4, 4]
    branch_bit_set t0, 31, .ta_uint32_to_double
    jmp .ta_box_int32
.ta_uint32_to_double:
    # Value > INT32_MAX, convert to double
    int_to_double ft0, t0
    fp_mov t0, ft0
    store_operand m_dst, t0
    dispatch_next
.ta_box_int32:
    box_int32_clean t3, t0
    store_operand m_dst, t3
    dispatch_next
.try_typed_array_slow:
    call_interp asm_try_get_by_value_typed_array
    branch_nonzero t0, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_by_value
end

# Fast path for Array.length (magical length property).
# Also includes IC fast path for non-array objects (same as GetById).
handler GetLength
    load_operand t1, m_base
    # Check base is an object
    extract_tag t2, t1
    branch_ne t2, OBJECT_TAG, .slow
    # Extract Object*
    unbox_object t3, t1
    # Check has_magical_length_property flag
    load8 t0, [t3, OBJECT_FLAGS]
    branch_bits_set t0, OBJECT_FLAG_HAS_MAGICAL_LENGTH, .magical_length
    # Non-magical length: IC fast path (same as GetById)
    load64 t4, [t3, OBJECT_SHAPE]
    load64 t5, [pb, pc, m_cache]
    # Check entry[0].shape and entry[0].prototype.
    load_pair64 t1, t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_ne t1, t4, .slow
    branch_nonzero t0, .slow
    # Check dictionary generation
    load_pair32 t1, t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .slow
    # IC hit
    load64 t5, [t3, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t1, 8]
    extract_tag t2, t0
    branch_eq t2, ACCESSOR_TAG, .slow
    store_operand m_dst, t0
    dispatch_next
.magical_length:
    # Object.m_indexed_array_like_size (u32)
    load32 t0, [t3, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    # Box as int32 if fits (u32 always fits since bit 31 check is for sign)
    mov t2, t0
    shr t2, 31
    branch_nonzero t2, .length_double
    # Tag as int32
    box_int32 t3, t0
    store_operand m_dst, t3
    dispatch_next
.length_double:
    int_to_double ft0, t0
    fp_mov t0, ft0
    store_operand m_dst, t0
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_length
end

# Inline cache fast path for global variable access via the global object.
handler GetGlobal
    # Load global_declarative_environment and global_object via realm
    load64 t0, [exec_ctx, EXECUTION_CONTEXT_REALM]
    load_pair64 t2, t1, [t0, REALM_GLOBAL_OBJECT], [t0, REALM_GLOBAL_DECLARATIVE_ENVIRONMENT]
    # Get GlobalVariableCache* (direct pointer from instruction stream)
    load64 t3, [pb, pc, m_cache]
    # Check environment_serial_number matches
    load64 t0, [t3, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL]
    load64 t4, [t1, DECLARATIVE_ENVIRONMENT_SERIAL]
    branch_ne t0, t4, .slow
    # Shape-based fast path: check entries[0].shape matches global_object.shape
    # (falls through to env binding path on shape mismatch)
    load64 t4, [t2, OBJECT_SHAPE]
    load64 t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne t0, t4, .try_env_binding
    # Check dictionary generation
    load32 t5, [t4, SHAPE_DICTIONARY_GENERATION]
    load_pair32 t4, t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    branch_ne t0, t5, .try_env_binding
    # IC hit! Load property value via get_direct
    load64 t5, [t2, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t4, 8]
    # Check not accessor
    extract_tag t5, t0
    branch_eq t5, ACCESSOR_TAG, .slow
    store_operand m_dst, t0
    dispatch_next
.try_env_binding:
    # Check if cache has an environment binding index (global let/const)
    load8 t0, [t3, GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING]
    branch_zero t0, .slow
    # Bail to C++ for module environments (rare)
    load8 t0, [t3, GLOBAL_VARIABLE_CACHE_IN_MODULE_ENVIRONMENT]
    branch_nonzero t0, .slow_env
    # Inline env binding: index into global_declarative_environment bindings
    # t1 = global_declarative_environment (loaded at handler entry)
    load32 t0, [t3, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX]
    load64 t4, [t1, BINDINGS_DATA_PTR]
    mul t0, t0, SIZEOF_BINDING
    add t4, t0
    # Check binding is initialized (TDZ)
    load8 t0, [t4, BINDING_INITIALIZED]
    branch_zero t0, .slow
    # Load binding value
    load64 t0, [t4, BINDING_VALUE]
    store_operand m_dst, t0
    dispatch_next
.slow_env:
    call_interp asm_try_get_global_env_binding
    branch_nonzero t0, .slow
    dispatch_next
.slow:
    call_slow_path asm_slow_path_get_global
end

# Inline cache fast path for global variable store via the global object.
handler SetGlobal
    # Load global_declarative_environment and global_object via realm
    load64 t0, [exec_ctx, EXECUTION_CONTEXT_REALM]
    load_pair64 t2, t1, [t0, REALM_GLOBAL_OBJECT], [t0, REALM_GLOBAL_DECLARATIVE_ENVIRONMENT]
    # Get GlobalVariableCache* (direct pointer from instruction stream)
    load64 t3, [pb, pc, m_cache]
    # Check environment_serial_number matches
    load64 t0, [t3, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL]
    load64 t4, [t1, DECLARATIVE_ENVIRONMENT_SERIAL]
    branch_ne t0, t4, .slow
    # Shape-based fast path: check entries[0].shape matches global_object.shape
    # (falls through to env binding path on shape mismatch)
    load64 t4, [t2, OBJECT_SHAPE]
    load64 t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne t0, t4, .try_env_binding
    # Check dictionary generation
    load32 t5, [t4, SHAPE_DICTIONARY_GENERATION]
    load_pair32 t4, t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET], [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    branch_ne t0, t5, .try_env_binding
    # IC hit! Load current value to check it's not an accessor
    load64 t5, [t2, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t4, 8]
    extract_tag t0, t0
    branch_eq t0, ACCESSOR_TAG, .slow
    # Store new value via put_direct
    # NB: load_operand clobbers t0, so property offset stays in t4.
    load_operand t0, m_src
    store64 [t5, t4, 8], t0
    dispatch_next
.try_env_binding:
    # Check if cache has an environment binding index (global let/const)
    load8 t0, [t3, GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING]
    branch_zero t0, .slow
    # Bail to C++ for module environments (rare)
    load8 t0, [t3, GLOBAL_VARIABLE_CACHE_IN_MODULE_ENVIRONMENT]
    branch_nonzero t0, .slow_env
    # Inline env binding: index into global_declarative_environment bindings
    # t1 = global_declarative_environment (loaded at handler entry)
    load32 t0, [t3, GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX]
    load64 t4, [t1, BINDINGS_DATA_PTR]
    mul t0, t0, SIZEOF_BINDING
    add t4, t0
    # Check binding is initialized (TDZ) and mutable
    load8 t0, [t4, BINDING_INITIALIZED]
    branch_zero t0, .slow
    load8 t0, [t4, BINDING_MUTABLE]
    branch_zero t0, .slow
    # Store value into binding
    load_operand t0, m_src
    store64 [t4, BINDING_VALUE], t0
    dispatch_next
.slow_env:
    call_interp asm_try_set_global_env_binding
    branch_nonzero t0, .slow
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
    # Everything else — non-functions, NativeJavaScriptBackedFunction,
    # ECMAScript functions that can't inline, Proxies, ... — falls through
    # to .call_slow, i.e. asm_slow_path_call.
    #
    # High-level flow of the ECMAScript fast path:
    #   1. Validate the callee and load its shared function metadata.
    #   2. Bind `this` inline when we can do so without allocations.
    #   3. Reserve an InterpreterStack frame and populate ExecutionContext.
    #   4. Materialize [registers | locals | constants | arguments].
    #   5. Swap VM state over to the callee frame and dispatch at pc = 0.
    #
    # Register usage within this handler:
    #   t3 = callee ECMAScriptFunctionObject*
    #   t2 = asm-call metadata / later callee ExecutionContext*
    #   t7 = callee Executable* carried across `this` binding
    #   t8 = boxed `this` value carried into the callee
    load_operand t0, m_callee
    extract_tag t1, t0
    branch_ne t1, OBJECT_TAG, .call_slow
    unbox_object t0, t0
    mov t3, t0

    # Non-functions still go through the normal Call slow path for proper error
    # reporting. Non-ECMAScript function objects get a RawNativeFunction fast
    # path attempt before we fully give up.
    load8 t1, [t3, OBJECT_FLAGS]
    branch_bits_clear t1, OBJECT_FLAG_IS_FUNCTION, .call_slow
    branch_bits_clear t1, OBJECT_FLAG_IS_ECMASCRIPT_FUNCTION_OBJECT, .call_try_native

    load64 t2, [t3, ECMASCRIPT_FUNCTION_OBJECT_SHARED_DATA]
    load_pair64 t7, t2, [t2, SHARED_FUNCTION_INSTANCE_DATA_EXECUTABLE], [t2, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA]
    branch_bits_clear t2, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_CAN_INLINE_CALL, .call_slow
    # NewFunctionEnvironment() allocates and has to stay out of the pure asm
    # path, but we still preserve inline-call semantics via .call_interp_inline.
    branch_bits_set t2, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_FUNCTION_ENVIRONMENT_NEEDED, .call_interp_inline

    # Bind this without allocations. Sloppy primitive this-values still need
    # ToObject(), so they use the C++ inline-frame helper.
    #
    # t8 starts as "empty" to match the normal interpreter behavior for
    # callees that never observe `this`.
    mov t8, EMPTY_TAG_SHIFTED
    branch_bits_clear t2, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_USES_THIS, .this_ready
    load_operand t8, m_this_value
    branch_bits_set t2, SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_STRICT, .this_ready

    # Sloppy null/undefined binds the callee realm's global object.
    # Sloppy primitive receivers need ToObject(), which may allocate wrappers,
    # so they go through the helper instead of the full Call slow path.
    extract_tag t1, t8
    mov t0, t1
    and t0, 0xFFFE
    branch_eq t0, UNDEFINED_TAG, .sloppy_global_this
    branch_eq t1, OBJECT_TAG, .this_ready
    jmp .call_interp_inline

.sloppy_global_this:
    load64 t1, [t3, OBJECT_SHAPE]
    load64 t1, [t1, SHAPE_REALM]
    load64 t1, [t1, REALM_GLOBAL_ENVIRONMENT]
    load64 t1, [t1, GLOBAL_ENVIRONMENT_GLOBAL_THIS_VALUE]
    # Match Value(Object*): keep only the low 48 pointer bits before boxing.
    shl t1, 16
    shr t1, 16
    mov t8, OBJECT_TAG_SHIFTED
    or t8, t1

.this_ready:
    # The low 32 bits of the packed metadata word hold the formal parameter count.
    and t2, 0xFFFFFFFF

    load32 t6, [pb, pc, m_argument_count]
    mov t4, t2
    branch_ge_unsigned t4, t6, .arg_count_ready
    mov t4, t6
.arg_count_ready:
    load_pair32 t5, t1, [t7, EXECUTABLE_REGISTERS_AND_LOCALS_COUNT], [t7, EXECUTABLE_REGISTERS_AND_LOCALS_AND_CONSTANTS_COUNT]

    # Inline InterpreterStack::allocate().
    # t1 = total Value slots, t2 = new stack top, t6 = current frame base.
    add t1, t4
    mov t2, t1
    shl t2, 3
    add t2, SIZEOF_EXECUTION_CONTEXT

    load_vm t0
    lea t0, [t0, VM_INTERPRETER_STACK]
    load_pair64 t6, t0, [t0, INTERPRETER_STACK_TOP], [t0, INTERPRETER_STACK_LIMIT]
    add t2, t6
    branch_ge_unsigned t0, t2, .stack_ok
    jmp .call_slow

.stack_ok:
    load_vm t0
    store64 [t0, VM_INTERPRETER_STACK_TOP], t2

    # Set up the callee ExecutionContext header exactly the way
    # VM::push_inline_frame() / run_executable() would see it.
    mov t2, t6
    lea t6, [t6, SIZEOF_EXECUTION_CONTEXT]
    store_pair32 [t2, EXECUTION_CONTEXT_REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT], [t2, EXECUTION_CONTEXT_ARGUMENT_COUNT], t1, t4
    load32 t0, [pb, pc, m_argument_count]
    store32 [t2, EXECUTION_CONTEXT_PASSED_ARGUMENT_COUNT], t0

    load64 t0, [t3, OBJECT_SHAPE]
    load64 t0, [t0, SHAPE_REALM]
    store_pair64 [t2, EXECUTION_CONTEXT_FUNCTION], [t2, EXECUTION_CONTEXT_REALM], t3, t0

    load_pair64 t0, t1, [t3, ECMASCRIPT_FUNCTION_OBJECT_ENVIRONMENT], [t3, ECMASCRIPT_FUNCTION_OBJECT_PRIVATE_ENVIRONMENT]
    store_pair64 [t2, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT], [t2, EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT], t0, t0
    store64 [t2, EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT], t1
    store_pair64 [t2, EXECUTION_CONTEXT_THIS_VALUE], [t2, EXECUTION_CONTEXT_EXECUTABLE], t8, t7

    mov t1, EMPTY_TAG_SHIFTED
    store_pair64 [t6, ACCUMULATOR_REG_OFFSET], [t6, EXCEPTION_REG_OFFSET], t1, t1
    store64 [t6, THIS_VALUE_REG_OFFSET], t8
    store_pair64 [t6, RETURN_VALUE_REG_OFFSET], [t6, SAVED_LEXICAL_ENVIRONMENT_REG_OFFSET], t1, t1

    # ScriptOrModule is a two-word Variant in ExecutionContext, so copy both
    # machine words explicitly.
    lea t0, [t2, EXECUTION_CONTEXT_SCRIPT_OR_MODULE]
    lea t7, [t3, ECMASCRIPT_FUNCTION_OBJECT_SCRIPT_OR_MODULE]
    load_pair64 t3, t8, [t7, 0], [t7, 8]
    store64 [t0, 0], t3
    store64 [t0, 8], t8

    store32 [t2, EXECUTION_CONTEXT_PROGRAM_COUNTER], 0
    store32 [t2, EXECUTION_CONTEXT_SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER], 0
    mov t0, EXECUTION_CONTEXT_NO_YIELD_CONTINUATION
    store32 [t2, EXECUTION_CONTEXT_YIELD_CONTINUATION], t0
    store8 [t2, EXECUTION_CONTEXT_YIELD_IS_AWAIT], 0
    store8 [t2, EXECUTION_CONTEXT_CALLER_IS_CONSTRUCT], 0
    store64 [t2, EXECUTION_CONTEXT_CALLER_FRAME], exec_ctx
    load_pair32 t0, t1, [pb, pc, m_length], [pb, pc, m_dst]
    lea t3, [pb, pc]
    sub t3, pb
    add t0, t3
    store_pair32 [t2, EXECUTION_CONTEXT_CALLER_RETURN_PC], [t2, EXECUTION_CONTEXT_CALLER_DST_RAW], t0, t1

    # values = [registers | locals | constants | arguments]
    # Keep t2 at the ExecutionContext base while t6 walks the Value tail.
    mov t0, t5
    shl t0, 3
    mov t3, RESERVED_REGISTERS_SIZE
.clear_registers_and_locals:
    mov t8, t3
    add t8, 8
    branch_ge_unsigned t8, t0, .clear_registers_and_locals_tail
    store_pair64 [t6, t3, 0], [t6, t3, 8], t1, t1
    add t3, 16
    jmp .clear_registers_and_locals

.clear_registers_and_locals_tail:
    branch_ge_unsigned t3, t0, .copy_constants
    store64 [t6, t3], t1

.copy_constants:
    load64 t0, [t2, EXECUTION_CONTEXT_EXECUTABLE]
    load_pair64 t3, t0, [t0, EXECUTABLE_ASM_CONSTANTS_SIZE], [t0, EXECUTABLE_ASM_CONSTANTS_DATA]
    mov t1, t5
    xor t8, t8
.copy_constants_loop:
    branch_ge_unsigned t8, t3, .copy_arguments
    load64 t7, [t0, t8, 8]
    store64 [t6, t1, 8], t7
    add t8, 1
    add t1, 1
    jmp .copy_constants_loop

.copy_arguments:
    load32 t7, [pb, pc, m_argument_count]
    mov t1, t5
    add t1, t3
    lea t0, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    lea t8, [pb, pc]
    add t8, m_expression_string
    add t8, 4
    xor t3, t3
.copy_arguments_loop:
    # The operand array in the bytecode stores caller register indices.
    branch_ge_unsigned t3, t7, .fill_missing_arguments
    load32 t5, [t8, t3, 4]
    load64 t5, [t0, t5, 8]
    store64 [t6, t1, 8], t5
    add t3, 1
    add t1, 1
    jmp .copy_arguments_loop

.fill_missing_arguments:
    mov t3, t1
    add t3, t4
    sub t3, t7
    mov t0, UNDEFINED_SHIFTED
.fill_missing_arguments_loop:
    branch_ge_unsigned t1, t3, .enter_callee
    store64 [t6, t1, 8], t0
    add t1, 1
    jmp .fill_missing_arguments_loop

.enter_callee:
    load64 pb, [t2, EXECUTION_CONTEXT_EXECUTABLE]
    load64 pb, [pb, EXECUTABLE_BYTECODE_DATA]
    load_vm t0
    store64 [t0, VM_RUNNING_EXECUTION_CONTEXT], t2
    mov exec_ctx, t2
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    xor pc, pc
    goto_handler pc
.call_interp_inline:
    # Shared escape hatch for the cases that need C++ help to build the inline
    # frame correctly but must not take the full Call slow path, since that
    # would insert a run_executable() boundary and observable microtask drain.
    call_interp asm_try_inline_call
    branch_nonzero t0, .call_slow
    load_vm t0
    load64 exec_ctx, [t0, VM_RUNNING_EXECUTION_CONTEXT]
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    load64 t0, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    load64 pb, [t0, EXECUTABLE_BYTECODE_DATA]
    xor pc, pc
    goto_handler pc
.call_try_native:
    # Fast path for RawNativeFunction: the callee is a plain C++ function
    # pointer with no JS-visible prologue, so we can build the callee frame
    # ourselves and jump straight at the entry point. NativeFunction objects
    # that still carry a callback (NativeJavaScriptBackedFunction) do not have
    # this flag set and fall through to .call_slow.
    load8 t0, [t3, OBJECT_FLAGS]
    branch_bits_clear t0, OBJECT_FLAG_IS_RAW_NATIVE_FUNCTION, .call_slow

    # Unlike the ECMAScript path we don't pad to the formal parameter count:
    # native functions read their arguments via the passed-count API, so we
    # only need space for the call-site arguments plus the ExecutionContext
    # header. t4 = argument count, t5 = total bytes needed for this frame.
    load32 t4, [pb, pc, m_argument_count]
    mov t5, t4
    shl t5, 3
    add t5, SIZEOF_EXECUTION_CONTEXT

    # Inline InterpreterStack::allocate(): bail to C++ if the interpreter
    # stack doesn't have room for the new frame. t6 = new frame base (old
    # top), t5 becomes the new top after the add below.
    load_vm t0
    lea t0, [t0, VM_INTERPRETER_STACK]
    load_pair64 t6, t7, [t0, INTERPRETER_STACK_TOP], [t0, INTERPRETER_STACK_LIMIT]
    add t5, t6
    branch_ge_unsigned t7, t5, .native_interpreter_stack_ok
    jmp .call_slow

.native_interpreter_stack_ok:
    # RawNativeFunctions run real C++ code on the host stack, so we also have
    # to check that we're not about to blow past the VM's reserved stack
    # limit. The ECMAScript path can skip this because it never leaves asm.
    load_vm t0
    lea t0, [t0, VM_STACK_INFO]
    load64 t7, [t0, STACK_INFO_BASE]
    add t7, VM_STACK_SPACE_LIMIT
    branch_ge_unsigned fp, t7, .native_stack_space_ok
    jmp .call_slow

.native_stack_space_ok:
    # Commit the new interpreter stack top. From here on we own [t6, t5).
    load_vm t0
    store64 [t0, VM_INTERPRETER_STACK_TOP], t5

    # Populate the callee ExecutionContext to match what VM::push_execution_context
    # plus NativeFunction::internal_call would produce. t2 tracks the EC
    # header, t6 advances to the argument Value array that follows it.
    mov t2, t6
    lea t6, [t6, SIZEOF_EXECUTION_CONTEXT]
    # For natives, argument_count and "registers+..." total are both just the
    # call-site argument count: there are no registers, locals, or constants.
    store_pair32 [t2, EXECUTION_CONTEXT_REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT], [t2, EXECUTION_CONTEXT_ARGUMENT_COUNT], t4, t4
    store32 [t2, EXECUTION_CONTEXT_PASSED_ARGUMENT_COUNT], t4

    # Shape stores a Realm pointer; use it as the callee EC realm.
    load64 t0, [t3, OBJECT_SHAPE]
    load64 t0, [t0, SHAPE_REALM]
    store_pair64 [t2, EXECUTION_CONTEXT_FUNCTION], [t2, EXECUTION_CONTEXT_REALM], t3, t0

    # Mirror NativeFunction::internal_call: a raw native has no environment of
    # its own, so lexical/variable/private environments are copied straight
    # from the caller frame.
    load_pair64 t0, t7, [exec_ctx, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT], [exec_ctx, EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT]
    store_pair64 [t2, EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT], [t2, EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT], t0, t7
    load64 t0, [exec_ctx, EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT]
    store64 [t2, EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT], t0
    # |this| is forwarded unchanged. Native builtins do their own type checks
    # on the receiver where they need to.
    load_operand t0, m_this_value
    store64 [t2, EXECUTION_CONTEXT_THIS_VALUE], t0

    # Zero out the ScriptOrModule variant (two words) and Executable pointer.
    # Native frames don't belong to any script/module and have no bytecode.
    xor t0, t0
    lea t7, [t2, EXECUTION_CONTEXT_SCRIPT_OR_MODULE]
    store_pair64 [t7, 0], [t7, 8], t0, t0
    store64 [t2, EXECUTION_CONTEXT_EXECUTABLE], t0
    store32 [t2, EXECUTION_CONTEXT_PROGRAM_COUNTER], 0
    store32 [t2, EXECUTION_CONTEXT_SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER], 0
    mov t0, EXECUTION_CONTEXT_NO_YIELD_CONTINUATION
    store32 [t2, EXECUTION_CONTEXT_YIELD_CONTINUATION], t0
    store8 [t2, EXECUTION_CONTEXT_YIELD_IS_AWAIT], 0
    store8 [t2, EXECUTION_CONTEXT_CALLER_IS_CONSTRUCT], 0

    # While asm runs, the authoritative program counter lives in the `pc`
    # register and the caller EC's stored program_counter is stale. Before we
    # leave asm to run native C++ that may throw, sync `pc` into the caller
    # EC as a bytecode offset (pc - pb). asm_helper_handle_raw_native_exception
    # and VM::handle_exception both read from the caller EC after unwind.
    lea t7, [pb, pc]
    sub t7, pb
    store32 [exec_ctx, EXECUTION_CONTEXT_PROGRAM_COUNTER], t7
    store64 [t2, EXECUTION_CONTEXT_CALLER_FRAME], exec_ctx
    # CALLER_RETURN_PC is the bytecode offset of the instruction after the
    # Call (Call offset + Call length). CALLER_DST_RAW records where the
    # return value should be written in the caller's value array.
    load32 t0, [pb, pc, m_length]
    add t0, t7
    store32 [t2, EXECUTION_CONTEXT_CALLER_RETURN_PC], t0
    load32 t0, [pb, pc, m_dst]
    store32 [t2, EXECUTION_CONTEXT_CALLER_DST_RAW], t0

    # Copy the call-site arguments from the caller's value array into the
    # callee frame's argument tail. t0 points at the caller's value array,
    # t8 at the Operand[] that trails the fixed Call instruction fields.
    # The Call layout ends with `m_expression_string: Optional<StringTableIndex>`
    # (4 bytes via the sentinel specialization) followed by `m_arguments`, so
    # base + offsetof(m_expression_string) + 4 is the operand array.
    lea t0, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    lea t8, [pb, pc]
    add t8, m_expression_string
    add t8, 4
    xor t7, t7
.copy_native_arguments_loop:
    branch_ge_unsigned t7, t4, .enter_raw_native
    load32 t5, [t8, t7, 4]
    load64 t5, [t0, t5, 8]
    store64 [t6, t7, 8], t5
    add t7, 1
    jmp .copy_native_arguments_loop

.enter_raw_native:
    # Swap the running ExecutionContext over to the callee and point the
    # asm `values` register at its argument array. After this, we look like
    # a normal inline frame from the VM's perspective.
    load_vm t0
    store64 [t0, VM_RUNNING_EXECUTION_CONTEXT], t2
    mov exec_ctx, t2
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]

    # Invoke the raw C++ function pointer. call_raw_native lowers to a native
    # call through the platform ABI and surfaces the returned
    # ThrowCompletionOr<Value> via (t0, t1): t0 is the Value payload and t1
    # holds the Variant discriminator in its low byte (0 = Value, 1 =
    # ErrorValue). Anything non-zero in that byte means the native threw and
    # t0 is the thrown Value, not a return value.
    load64 t3, [t3, RAW_NATIVE_FUNCTION_NATIVE_FUNCTION]
    call_raw_native t3
    and t1, 0xFF
    branch_nonzero t1, .call_raw_native_exception

    # Normal return path: tear the callee frame off the interpreter stack,
    # restore the caller as the running ExecutionContext, write the return
    # value into the caller's m_dst operand, and dispatch the next insn.
    load64 t2, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    load_vm t3
    store64 [t3, VM_RUNNING_EXECUTION_CONTEXT], t2
    store64 [t3, VM_INTERPRETER_STACK_TOP], exec_ctx
    mov exec_ctx, t2
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    store_operand m_dst, t0
    load32 t0, [pb, pc, m_length]
    dispatch_variable t0

.call_raw_native_exception:
    # The native threw. Hand the thrown Value off to a C++ helper, which
    # unwinds the callee frame off the interpreter stack and calls through
    # to VM::handle_exception. Return value (t0) follows the standard asm
    # slow-path convention (see AsmInterpreter.cpp:127):
    #   >= 0 : an enclosing handler was found; t0 is the new program counter
    #          to resume at inside the (post-unwind) running execution context.
    #    < 0 : no handler; bail out of the asm dispatch loop entirely.
    mov t1, t0
    call_helper asm_helper_handle_raw_native_exception
    branch_negative t0, .call_exit_asm
    jmp .call_exception_handled
.call_exception_handled:
    # Reload exec_ctx/values/pb/pc from the caller frame the helper left us
    # on, and resume dispatching at its program_counter (which the helper
    # already updated to the handler entry).
    load_vm t0
    load64 exec_ctx, [t0, VM_RUNNING_EXECUTION_CONTEXT]
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
    load64 t0, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    load64 pb, [t0, EXECUTABLE_BYTECODE_DATA]
    load32 t2, [exec_ctx, EXECUTION_CONTEXT_PROGRAM_COUNTER]
    mov pc, t2
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
    validate_callee_builtin BUILTIN_MATH_ABS, .slow
    load_operand t1, m_argument
    check_is_double t1, .try_abs_int32
    # abs(double) = clear sign bit (bit 63)
    clear_bit t1, 63
    store_operand m_dst, t1
    dispatch_next
.try_abs_int32:
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    # abs(int32): negate if negative
    unbox_int32 t3, t1
    branch_not_negative t3, .abs_positive
    neg32_overflow t3, .abs_overflow
.abs_positive:
    box_int32_clean t4, t3
    store_operand m_dst, t4
    dispatch_next
.abs_overflow:
    # INT32_MIN: abs(-2147483648) = 2147483648.0
    unbox_int32 t3, t1
    neg t3
    int_to_double ft0, t3
    fp_mov t4, ft0
    store_operand m_dst, t4
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_abs
end

handler CallBuiltinMathFloor
    validate_callee_builtin BUILTIN_MATH_FLOOR, .slow
    load_operand t1, m_argument
    check_is_double t1, .slow
    fp_mov ft0, t1
    fp_floor ft0, ft0
    box_double_or_int32 t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_floor
end

handler CallBuiltinMathCeil
    validate_callee_builtin BUILTIN_MATH_CEIL, .slow
    load_operand t1, m_argument
    check_is_double t1, .slow
    fp_mov ft0, t1
    fp_ceil ft0, ft0
    box_double_or_int32 t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_ceil
end

handler CallBuiltinMathSqrt
    validate_callee_builtin BUILTIN_MATH_SQRT, .slow
    load_operand t1, m_argument
    check_is_double t1, .slow
    fp_mov ft0, t1
    fp_sqrt ft0, ft0
    box_double_or_int32 t5, ft0
    store_operand m_dst, t5
    dispatch_next
.slow:
    call_slow_path asm_slow_path_call_builtin_math_sqrt
end

handler CallBuiltinMathExp
    validate_callee_builtin BUILTIN_MATH_EXP, .slow
    load_operand t1, m_argument
    check_is_double t1, .slow
    fp_mov ft0, t1
    call_helper asm_helper_math_exp
    store_operand m_dst, t0
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
    validate_callee_builtin BUILTIN_STRING_FROM_CHAR_CODE, .slow

    load_operand t1, m_argument
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    unbox_int32 t0, t1
    and t0, 0xffff
    branch_ge_unsigned t0, 0x80, .single_code_unit

    mov t1, t0
    call_helper asm_helper_single_ascii_character_string
    store_operand m_dst, t0
    dispatch_next

.single_code_unit:
    mov t1, t0
    call_helper asm_helper_single_utf16_code_unit_string
    store_operand m_dst, t0
    dispatch_next

.slow:
    call_slow_path asm_slow_path_call_builtin_string_from_char_code
end

handler CallBuiltinStringPrototypeCharCodeAt
    validate_callee_builtin BUILTIN_STRING_PROTOTYPE_CHAR_CODE_AT, .slow

    load_operand t1, m_this_value
    extract_tag t3, t1
    branch_ne t3, STRING_TAG, .slow
    unbox_object t2, t1

    load_operand t1, m_argument
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    unbox_int32 t4, t1
    branch_negative t4, .out_of_bounds

    load_primitive_string_utf16_code_unit .out_of_bounds, .slow
    box_int32_clean t1, t0
    store_operand m_dst, t1
    dispatch_next

.out_of_bounds:
    mov t0, CANON_NAN_BITS
    store_operand m_dst, t0
    dispatch_next

.slow:
    call_slow_path asm_slow_path_call_builtin_string_prototype_char_code_at
end

handler CallBuiltinStringPrototypeCharAt
    validate_callee_builtin BUILTIN_STRING_PROTOTYPE_CHAR_AT, .slow

    load_operand t1, m_this_value
    extract_tag t3, t1
    branch_ne t3, STRING_TAG, .slow
    unbox_object t2, t1

    load_operand t1, m_argument
    extract_tag t3, t1
    branch_ne t3, INT32_TAG, .slow
    unbox_int32 t4, t1
    branch_negative t4, .empty

    load_primitive_string_utf16_code_unit .empty, .slow
    branch_ge_unsigned t0, 0x80, .slow

    mov t1, t0
    call_helper asm_helper_single_ascii_character_string
    store_operand m_dst, t0
    dispatch_next

.empty:
    mov t1, 0
    call_helper asm_helper_empty_string
    store_operand m_dst, t0
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
    load_operand t1, m_iterator_object
    extract_tag t2, t1
    branch_ne t2, OBJECT_TAG, .slow
    unbox_object t3, t1

    load8 t4, [t3, PROPERTY_NAME_ITERATOR_FAST_PATH]
    mov t0, OBJECT_PROPERTY_ITERATOR_FAST_PATH_NONE
    branch_eq t4, t0, .slow

    # These guards mirror PropertyNameIterator::fast_path_still_valid(). If the
    # receiver or prototype chain no longer matches the cached snapshot, we drop
    # to C++ and continue in deoptimized mode for the rest of the enumeration.
    load_pair64 t5, t7, [t3, PROPERTY_NAME_ITERATOR_PROPERTY_CACHE], [t3, PROPERTY_NAME_ITERATOR_SHAPE]
    load64 t6, [t3, PROPERTY_NAME_ITERATOR_OBJECT]
    load64 t8, [t6, OBJECT_SHAPE]
    branch_ne t8, t7, .slow

    load8 t2, [t3, PROPERTY_NAME_ITERATOR_SHAPE_IS_DICTIONARY]
    branch_zero t2, .check_receiver
    load32 t0, [t8, SHAPE_DICTIONARY_GENERATION]
    load32 t2, [t3, PROPERTY_NAME_ITERATOR_SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .slow

.check_receiver:
    mov t0, OBJECT_PROPERTY_ITERATOR_FAST_PATH_PACKED_INDEXED
    branch_ne t4, t0, .check_proto
    load8 t0, [t6, OBJECT_INDEXED_STORAGE_KIND]
    mov t2, INDEXED_STORAGE_KIND_PACKED
    branch_ne t0, t2, .slow
    load32 t0, [t6, OBJECT_INDEXED_ARRAY_LIKE_SIZE]
    load32 t2, [t3, PROPERTY_NAME_ITERATOR_INDEXED_PROPERTY_COUNT]
    branch_ne t0, t2, .slow

.check_proto:
    load64 t0, [t3, PROPERTY_NAME_ITERATOR_PROTOTYPE_CHAIN_VALIDITY]
    branch_zero t0, .next_key
    load8 t2, [t0, PROTOTYPE_CHAIN_VALIDITY_VALID]
    branch_zero t2, .slow

.next_key:
    # property_values is laid out as:
    #   [receiver packed index keys..., flattened named keys...]
    load_pair32 t2, t0, [t3, PROPERTY_NAME_ITERATOR_INDEXED_PROPERTY_COUNT], [t3, PROPERTY_NAME_ITERATOR_NEXT_INDEXED_PROPERTY]
    branch_ge_unsigned t0, t2, .named
    load64 t8, [t5, OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA]
    load64 t8, [t8, t0, 8]
    add t0, 1
    store32 [t3, PROPERTY_NAME_ITERATOR_NEXT_INDEXED_PROPERTY], t0
    store_operand m_dst_value, t8
    mov t0, BOOLEAN_FALSE
    store_operand m_dst_done, t0
    dispatch_next

.named:
    load64 t0, [t3, PROPERTY_NAME_ITERATOR_NEXT_PROPERTY]
    load64 t8, [t5, OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_SIZE]
    sub t8, t2
    branch_ge_unsigned t0, t8, .done
    mov t8, t0
    add t8, t2
    load64 t5, [t5, OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA]
    load64 t8, [t5, t8, 8]
    add t0, 1
    store64 [t3, PROPERTY_NAME_ITERATOR_NEXT_PROPERTY], t0
    store_operand m_dst_value, t8
    mov t0, BOOLEAN_FALSE
    store_operand m_dst_done, t0
    dispatch_next

.done:
    load64 t5, [t3, PROPERTY_NAME_ITERATOR_ITERATOR_CACHE_SLOT]
    branch_zero t5, .store_done
    # Return the exhausted iterator object to the bytecode-site cache so the
    # next execution of this loop can reset and reuse it.
    mov t0, 0
    store64 [t3, PROPERTY_NAME_ITERATOR_OBJECT], t0
    store64 [t5, OBJECT_PROPERTY_ITERATOR_CACHE_REUSABLE_PROPERTY_NAME_ITERATOR], t3
    store64 [t3, PROPERTY_NAME_ITERATOR_ITERATOR_CACHE_SLOT], t0

.store_done:
    mov t0, BOOLEAN_TRUE
    store_operand m_dst_done, t0
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
    load64 t0, [values, THIS_VALUE_REG_OFFSET]
    mov t1, EMPTY_VALUE
    branch_eq t0, t1, .slow
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
