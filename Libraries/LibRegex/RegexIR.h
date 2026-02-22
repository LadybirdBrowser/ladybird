/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibRegex/RegexByteCode.h>

namespace regex {

using LabelId = u32;

#define ENUMERATE_REGEX_IR_OPS(X) \
    X(Nop)                        \
    X(Label)                      \
    X(Compare)                    \
    X(CheckBegin)                 \
    X(CheckEnd)                   \
    X(CheckBoundary)              \
    X(Jump)                       \
    X(ForkJump)                   \
    X(ForkStay)                   \
    X(ForkReplaceJump)            \
    X(ForkReplaceStay)            \
    X(ForkIf)                     \
    X(JumpNonEmpty)               \
    X(Save)                       \
    X(Restore)                    \
    X(Checkpoint)                 \
    X(GoBack)                     \
    X(SetStepBack)                \
    X(IncStepBack)                \
    X(CheckStepBack)              \
    X(CheckSavedPosition)         \
    X(SaveLeftCapture)            \
    X(SaveRightCapture)           \
    X(SaveRightNamedCapture)      \
    X(ClearCaptureGroup)          \
    X(Repeat)                     \
    X(ResetRepeat)                \
    X(FailIfEmpty)                \
    X(SaveModifiers)              \
    X(RestoreModifiers)           \
    X(Exit)                       \
    X(FailForks)                  \
    X(PopSaved)                   \
    X(RSeekTo)

enum class IROp : u8 {
#define M(x) x,
    ENUMERATE_REGEX_IR_OPS(M)
#undef M
};

struct Inst {
    IROp op {};
    u8 flags {};
    u16 compare_size {};
    LabelId target {};
    u32 arg0 {};
    u32 arg1 {};
    u32 compare_start {};
};
static_assert(sizeof(Inst) == 20);

struct RegexIR {
    Vector<Inst> insts;
    Vector<ByteCodeValueType> compare_data;

    StringTable<FlyString> string_table;
    StringTable<Utf16FlyString> u16_string_table;
    StringSetTable string_set_table;
    HashMap<size_t, size_t> group_name_mappings;

    LabelId next_label {};
    LabelId alloc_label() { return next_label++; }
};

REGEX_API void compact_ir(RegexIR& ir);
REGEX_API Vector<CompareTypeAndValuePair> ir_flat_compares(Span<ByteCodeValueType const> data, u32 arg_count);
REGEX_API StringView irop_name(IROp op);

}
