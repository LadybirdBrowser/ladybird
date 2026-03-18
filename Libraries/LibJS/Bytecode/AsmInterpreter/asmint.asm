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
#   t0-t9    = general-purpose scratch
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

# Dispatch using the instruction's m_length field (for variable-length instructions).
macro dispatch_callbuiltin_size()
    load32 t0, [pb, pc, m_length]
    dispatch_variable t0
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
    load32 t1, [t0, ENVIRONMENT_COORDINATE_HOPS]
    mov t4, ENVIRONMENT_COORDINATE_INVALID
    branch_eq t1, t4, fail_label
    load32 t2, [t0, ENVIRONMENT_COORDINATE_INDEX]
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

# Reload pb and values from the current running execution context.
# Used after inline call/return to switch to the new frame's bytecode.
# Clobbers t0, t1.
macro reload_state_from_exec_ctx()
    reload_exec_ctx
    load64 t1, [exec_ctx, EXECUTION_CONTEXT_EXECUTABLE]
    load64 pb, [t1, EXECUTABLE_BYTECODE_DATA]
    lea values, [exec_ctx, SIZEOF_EXECUTION_CONTEXT]
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
# canonicalize_nan ensures NaN results don't collide with the tag space.
handler Add
    load_operand t1, m_lhs
    load_operand t2, m_rhs
    coerce_to_doubles .both_int, .slow
    # One or both doubles: ft0=lhs, ft1=rhs
    fp_add ft0, ft1
    canonicalize_nan t5, ft0
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
    canonicalize_nan t5, ft0
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
    canonicalize_nan t5, ft0
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
    # Check if this is an inline frame (caller_frame != nullptr)
    load64 t1, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    branch_zero t1, .top_level
    # Inline return: pop the frame via C++ helper, then reload state
    call_interp asm_pop_inline_frame
    reload_state_from_exec_ctx
    # Load the restored caller's program_counter
    load32 pc, [exec_ctx, EXECUTION_CONTEXT_PROGRAM_COUNTER]
    dispatch_current
.top_level:
    # Top-level return: load value, empty->undefined, store to return_value
    load_operand t0, m_value
    mov t1, EMPTY_TAG_SHIFTED
    branch_ne t0, t1, .store_return
    mov t0, UNDEFINED_SHIFTED
.store_return:
    # values[3] = return_value, values[1] = empty (clear exception)
    store64 [values, 24], t0
    mov t1, EMPTY_TAG_SHIFTED
    store64 [values, 8], t1
    exit
end

# Like Return, but does not clear the exception register (values[1]).
# Used at the end of a function body (after all user code).
handler End
    # Check if this is an inline frame (caller_frame != nullptr)
    load64 t1, [exec_ctx, EXECUTION_CONTEXT_CALLER_FRAME]
    branch_zero t1, .top_level
    # Inline return: pop the frame via C++ helper, then reload state
    call_interp asm_pop_inline_frame_end
    reload_state_from_exec_ctx
    # Load the restored caller's program_counter
    load32 pc, [exec_ctx, EXECUTION_CONTEXT_PROGRAM_COUNTER]
    dispatch_current
.top_level:
    # Top-level end: load value, empty->undefined, store to return_value
    load_operand t0, m_value
    mov t1, EMPTY_TAG_SHIFTED
    branch_ne t0, t1, .store_end
    mov t0, UNDEFINED_SHIFTED
.store_end:
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
    load64 t1, [t0, BINDING_VALUE]
    # Check value is not empty (TDZ)
    mov t4, EMPTY_TAG_SHIFTED
    branch_eq t1, t4, .slow
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
    box_int32 t3, t2
    store_operand m_dst, t3
    dispatch_next
.try_boolean:
    branch_ne t2, BOOLEAN_TAG, .slow
    # Convert boolean to int32: false -> 0, true -> 1
    and t1, 1
    box_int32 t1, t1
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
    load64 t1, [t5, t4, 8]
    mov t0, EMPTY_TAG_SHIFTED
    branch_eq t1, t0, .slow
    load_operand t1, m_src
    store64 [t5, t4, 8], t1
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
    # Non-int32 value: only handle Float64Array with double source
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
.ta_store_int32:
    # t1 = NaN-boxed int32, extract low 32 bits into t0
    mov t0, t1
    and t0, 0xFFFFFFFF
    # Dispatch on kind (in t2)
    branch_any_eq t2, TYPED_ARRAY_KIND_INT32, TYPED_ARRAY_KIND_UINT32, .ta_put_int32
    branch_any_eq t2, TYPED_ARRAY_KIND_UINT8, TYPED_ARRAY_KIND_INT8, .ta_put_uint8
    branch_any_eq t2, TYPED_ARRAY_KIND_UINT16, TYPED_ARRAY_KIND_INT16, .ta_put_uint16
    jmp .try_typed_array_slow
.ta_put_int32:
    store32 [t5, t4, 4], t0
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
    # Check entry[0].shape matches Object's shape (direct pointer compare)
    load64 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne t0, t4, .try_cache
    # Check entry[0].prototype (null = own property, non-null = prototype chain)
    load64 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_nonzero t0, .proto
    # Check dictionary generation matches
    load32 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .try_cache
    # IC hit! Load property value via get_direct (own property)
    load32 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET]
    load64 t5, [t3, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t0, 8]
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
    load32 t1, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t1, t2, .try_cache
    # IC hit! Load property value via get_direct (from prototype)
    load32 t1, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET]
    load64 t2, [t0, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t2, t1, 8]
    # Check value is not an accessor
    extract_tag t2, t0
    branch_eq t2, ACCESSOR_TAG, .try_cache
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
    # Check entry[0].shape matches Object's shape (direct pointer compare)
    load64 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne t0, t4, .try_cache
    # Check entry[0].prototype is null (own-property store only)
    load64 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_nonzero t0, .try_cache
    # Check dictionary generation matches
    load32 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .try_cache
    # Check current value at property_offset is not an accessor
    load32 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET]
    load64 t5, [t3, OBJECT_NAMED_PROPERTIES]
    load64 t2, [t5, t0, 8]
    extract_tag t4, t2
    branch_eq t4, ACCESSOR_TAG, .try_cache
    # IC hit! Store new value via put_direct
    # Save property offset in t4 before load_operand clobbers t0 (rax)
    mov t4, t0
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
    branch_eq t0, TYPED_ARRAY_KIND_UINT8, .ta_uint8
    branch_eq t0, TYPED_ARRAY_KIND_UINT16, .ta_uint16
    branch_eq t0, TYPED_ARRAY_KIND_INT8, .ta_int8
    branch_eq t0, TYPED_ARRAY_KIND_INT16, .ta_int16
    branch_eq t0, TYPED_ARRAY_KIND_UINT32, .ta_uint32
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
    # Check entry[0].shape matches (direct pointer compare)
    load64 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_SHAPE]
    branch_ne t0, t4, .slow
    # Check entry[0].prototype is null (own-property only)
    load64 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROTOTYPE]
    branch_nonzero t0, .slow
    # Check dictionary generation
    load32 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t2, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t2, .slow
    # IC hit
    load32 t0, [t5, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET]
    load64 t5, [t3, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t0, 8]
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
    load64 t2, [t0, REALM_GLOBAL_OBJECT]
    load64 t1, [t0, REALM_GLOBAL_DECLARATIVE_ENVIRONMENT]
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
    load32 t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t5, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t5, .try_env_binding
    # IC hit! Load property value via get_direct
    load32 t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET]
    load64 t5, [t2, OBJECT_NAMED_PROPERTIES]
    load64 t0, [t5, t0, 8]
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
    load64 t2, [t0, REALM_GLOBAL_OBJECT]
    load64 t1, [t0, REALM_GLOBAL_DECLARATIVE_ENVIRONMENT]
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
    load32 t0, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_DICTIONARY_GENERATION]
    load32 t5, [t4, SHAPE_DICTIONARY_GENERATION]
    branch_ne t0, t5, .try_env_binding
    # IC hit! Load current value to check it's not an accessor
    load32 t1, [t3, PROPERTY_LOOKUP_CACHE_ENTRY0_PROPERTY_OFFSET]
    load64 t5, [t2, OBJECT_NAMED_PROPERTIES]
    load64 t4, [t5, t1, 8]
    extract_tag t4, t4
    branch_eq t4, ACCESSOR_TAG, .slow
    # Store new value via put_direct
    # NB: load_operand clobbers t0, so property offset must be in t1.
    load_operand t4, m_src
    store64 [t5, t1, 8], t4
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
    # Try to inline the call
    call_interp asm_try_inline_call
    branch_nonzero t0, .call_slow
    # Success: reload pb/values from new execution context, pc=0
    reload_state_from_exec_ctx
    xor pc, pc
    dispatch_current
.call_slow:
    call_slow_path asm_slow_path_call
end

# Fast paths for common Math builtins with a single double argument.
# Before using the fast path, we validate that the callee is still the
# original builtin function (user code may have reassigned e.g. Math.abs).
handler CallBuiltin
    # Validate the callee: must be an object with IsFunction flag,
    # must have m_builtin set, and the builtin must match the instruction's.
    load_operand t2, m_callee
    extract_tag t3, t2
    branch_ne t3, OBJECT_TAG, .slow
    # Extract raw Object*
    unbox_object t2, t2
    # Check IsFunction flag
    load8 t3, [t2, OBJECT_FLAGS]
    and t3, OBJECT_FLAG_IS_FUNCTION
    branch_zero t3, .slow
    # Check FunctionObject::m_builtin.has_value()
    load8 t3, [t2, FUNCTION_OBJECT_BUILTIN_HAS_VALUE]
    branch_zero t3, .slow
    # Compare FunctionObject::m_builtin value with instruction's m_builtin
    load8 t3, [t2, FUNCTION_OBJECT_BUILTIN_VALUE]
    load8 t4, [pb, pc, m_builtin]
    branch_ne t3, t4, .slow
    # Callee validated. Now dispatch on the builtin enum (already in t4).
    branch_eq t4, BUILTIN_MATH_ABS, .math_abs
    branch_eq t4, BUILTIN_MATH_FLOOR, .math_floor
    branch_eq t4, BUILTIN_MATH_CEIL, .math_ceil
    branch_eq t4, BUILTIN_MATH_SQRT, .math_sqrt
    branch_eq t4, BUILTIN_MATH_EXP, .math_exp
    jmp .slow
.math_abs:
    load_operand t1, m_arguments
    check_is_double t1, .try_abs_int32
    # abs(double) = clear sign bit (bit 63)
    clear_bit t1, 63
    store_operand m_dst, t1
    dispatch_callbuiltin_size
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
    dispatch_callbuiltin_size
.abs_overflow:
    # INT32_MIN: abs(-2147483648) = 2147483648.0
    unbox_int32 t3, t1
    neg t3
    int_to_double ft0, t3
    fp_mov t4, ft0
    store_operand m_dst, t4
    dispatch_callbuiltin_size
.math_floor:
    load_operand t1, m_arguments
    check_is_double t1, .slow
    fp_mov ft0, t1
    fp_floor ft0, ft0
    canonicalize_nan t5, ft0
    store_operand m_dst, t5
    dispatch_callbuiltin_size
.math_ceil:
    load_operand t1, m_arguments
    check_is_double t1, .slow
    fp_mov ft0, t1
    fp_ceil ft0, ft0
    canonicalize_nan t5, ft0
    store_operand m_dst, t5
    dispatch_callbuiltin_size
.math_sqrt:
    load_operand t1, m_arguments
    check_is_double t1, .slow
    fp_mov ft0, t1
    fp_sqrt ft0, ft0
    canonicalize_nan t5, ft0
    store_operand m_dst, t5
    dispatch_callbuiltin_size
.math_exp:
    load_operand t1, m_arguments
    check_is_double t1, .slow
    fp_mov ft0, t1
    call_helper asm_helper_math_exp
    store_operand m_dst, t0
    dispatch_callbuiltin_size
.slow:
    call_slow_path asm_slow_path_call_builtin
end

# ============================================================================
# Slow-path-only handlers
# ============================================================================

# Handlers below are pure slow-path delegations: no fast path is worthwhile
# because the operation is inherently complex (object allocation, prototype
# chain walks, etc). Having them here avoids the generic fallback handler's
# overhead of saving/restoring all temporaries.

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
