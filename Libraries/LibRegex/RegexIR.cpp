/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <AK/HashMap.h>
#include <LibRegex/RegexIR.h>

namespace regex {

Vector<CompareTypeAndValuePair> ir_flat_compares(Span<ByteCodeValueType const> data, u32 arg_count)
{
    Vector<CompareTypeAndValuePair> result;
    size_t offset = 0;

    for (u32 i = 0; i < arg_count; ++i) {
        if (offset >= data.size())
            break;
        auto compare_type = static_cast<CharacterCompareType>(data[offset++]);

        if (compare_type == CharacterCompareType::Char) {
            auto ch = data[offset++];
            result.append({ compare_type, ch });
        } else if (compare_type == CharacterCompareType::Reference) {
            auto ref = data[offset++];
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::NamedReference) {
            auto ref = data[offset++];
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::String) {
            auto string_index = data[offset++];
            result.append({ compare_type, string_index });
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = data[offset++];
            result.append({ compare_type, character_class });
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = data[offset++];
            result.append({ compare_type, value });
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count_sensitive = data[offset++];
            auto count_insensitive = data[offset++];
            for (size_t j = 0; j < count_sensitive; ++j)
                result.append({ CharacterCompareType::CharRange, data[offset++] });
            offset += count_insensitive;
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension
            || compare_type == CharacterCompareType::StringSet) {
            auto value = data[offset++];
            result.append({ compare_type, value });
        } else {
            result.append({ compare_type, 0 });
        }
    }
    return result;
}

static size_t bytecode_instruction_size(OpCodeId id, Span<ByteCodeValueType const> flat, size_t ip)
{
    switch (id) {
    case OpCodeId::Compare:
        return 3 + flat[ip + 2]; // opcode + argc + args_size + args
    case OpCodeId::CompareSimple:
        return 2 + flat[ip + 1]; // opcode + args_size + args
    case OpCodeId::Jump:
    case OpCodeId::ForkJump:
    case OpCodeId::ForkStay:
    case OpCodeId::ForkReplaceJump:
    case OpCodeId::ForkReplaceStay:
        return 2;
    case OpCodeId::JumpNonEmpty:
    case OpCodeId::ForkIf:
    case OpCodeId::Repeat:
        return 4;
    case OpCodeId::SaveRightNamedCaptureGroup:
        return 3;
    case OpCodeId::GoBack:
    case OpCodeId::SetStepBack:
    case OpCodeId::SaveLeftCaptureGroup:
    case OpCodeId::SaveRightCaptureGroup:
    case OpCodeId::ClearCaptureGroup:
    case OpCodeId::FailIfEmpty:
    case OpCodeId::ResetRepeat:
    case OpCodeId::Checkpoint:
    case OpCodeId::CheckBoundary:
    case OpCodeId::RSeekTo:
    case OpCodeId::SaveModifiers:
        return 2;
    case OpCodeId::FailForks:
    case OpCodeId::PopSaved:
    case OpCodeId::Save:
    case OpCodeId::Restore:
    case OpCodeId::CheckBegin:
    case OpCodeId::CheckEnd:
    case OpCodeId::IncStepBack:
    case OpCodeId::CheckStepBack:
    case OpCodeId::CheckSavedPosition:
    case OpCodeId::RestoreModifiers:
    case OpCodeId::Exit:
        return 1;
    }
    VERIFY_NOT_REACHED();
}

RegexIR lift_bytecode(ByteCode&& bytecode)
{
    RegexIR ir;

    bytecode.flatten();
    auto flat = bytecode.flat_data();
    auto bytecode_size = flat.size();

    if (bytecode_size == 0) {
        ir.string_table = move(bytecode.m_string_table);
        ir.u16_string_table = move(bytecode.m_u16_string_table);
        ir.string_set_table = move(bytecode.m_string_set_table);
        ir.group_name_mappings = move(bytecode.m_group_name_mappings);
        return ir;
    }

    HashMap<size_t, LabelId> target_labels;

    for (size_t ip = 0; ip < bytecode_size;) {
        auto id = static_cast<OpCodeId>(flat[ip]);
        auto size = bytecode_instruction_size(id, flat, ip);

        auto register_target = [&](size_t target_ip) {
            if (!target_labels.contains(target_ip))
                target_labels.set(target_ip, ir.alloc_label());
        };

        switch (id) {
        case OpCodeId::Jump:
        case OpCodeId::ForkJump:
        case OpCodeId::ForkStay:
        case OpCodeId::ForkReplaceJump:
        case OpCodeId::ForkReplaceStay: {
            auto offset = static_cast<ssize_t>(flat[ip + 1]);
            register_target(ip + size + offset);
            break;
        }
        case OpCodeId::JumpNonEmpty:
        case OpCodeId::ForkIf: {
            auto offset = static_cast<ssize_t>(flat[ip + 1]);
            register_target(ip + size + offset);
            break;
        }
        case OpCodeId::Repeat: {
            auto offset = flat[ip + 1];
            register_target(ip - offset);
            break;
        }
        default:
            break;
        }
        ip += size;
    }

    if (!target_labels.contains(bytecode_size))
        target_labels.set(bytecode_size, ir.alloc_label());

    for (size_t ip = 0; ip < bytecode_size;) {
        if (auto label = target_labels.get(ip); label.has_value())
            ir.insts.append({ .op = IROp::Label, .target = *label });

        auto id = static_cast<OpCodeId>(flat[ip]);
        auto size = bytecode_instruction_size(id, flat, ip);
        Inst inst {};

        auto resolve_forward = [&](size_t offset_slot) -> LabelId {
            auto offset = static_cast<ssize_t>(flat[offset_slot]);
            auto target_ip = ip + size + offset;
            return *target_labels.get(target_ip);
        };

        switch (id) {
        case OpCodeId::Compare: {
            inst.op = IROp::Compare;
            inst.arg0 = static_cast<u32>(flat[ip + 1]); // arg count
            auto args_size = static_cast<u32>(flat[ip + 2]);
            inst.compare_start = ir.compare_data.size();
            inst.compare_size = static_cast<u16>(args_size);
            for (u32 j = 0; j < args_size; ++j)
                ir.compare_data.append(flat[ip + 3 + j]);
            break;
        }
        case OpCodeId::CompareSimple: {
            inst.op = IROp::Compare;
            inst.arg0 = 1; // always 1 (=simple) for CompareSimple
            auto args_size = static_cast<u32>(flat[ip + 1]);
            inst.compare_start = ir.compare_data.size();
            inst.compare_size = static_cast<u16>(args_size);
            for (u32 j = 0; j < args_size; ++j)
                ir.compare_data.append(flat[ip + 2 + j]);
            break;
        }
        case OpCodeId::Jump:
            inst.op = IROp::Jump;
            inst.target = resolve_forward(ip + 1);
            break;
        case OpCodeId::ForkJump:
            inst.op = IROp::ForkJump;
            inst.target = resolve_forward(ip + 1);
            break;
        case OpCodeId::ForkStay:
            inst.op = IROp::ForkStay;
            inst.target = resolve_forward(ip + 1);
            break;
        case OpCodeId::ForkReplaceJump:
            inst.op = IROp::ForkReplaceJump;
            inst.target = resolve_forward(ip + 1);
            break;
        case OpCodeId::ForkReplaceStay:
            inst.op = IROp::ForkReplaceStay;
            inst.target = resolve_forward(ip + 1);
            break;
        case OpCodeId::ForkIf:
            inst.op = IROp::ForkIf;
            inst.target = resolve_forward(ip + 1);
            inst.arg0 = static_cast<u32>(flat[ip + 2]); // form (OpCodeId)
            inst.arg1 = static_cast<u32>(flat[ip + 3]); // ForkIfCondition
            break;
        case OpCodeId::JumpNonEmpty: {
            inst.op = IROp::JumpNonEmpty;
            inst.target = resolve_forward(ip + 1);
            inst.arg0 = static_cast<u32>(flat[ip + 2]); // checkpoint_id
            inst.arg1 = static_cast<u32>(flat[ip + 3]); // form (OpCodeId)
            break;
        }
        case OpCodeId::Repeat: {
            inst.op = IROp::Repeat;
            auto offset = flat[ip + 1];
            inst.target = *target_labels.get(ip - offset);
            inst.arg0 = static_cast<u32>(flat[ip + 2]); // count
            inst.arg1 = static_cast<u32>(flat[ip + 3]); // id
            break;
        }
        case OpCodeId::CheckBegin:
            inst.op = IROp::CheckBegin;
            break;
        case OpCodeId::CheckEnd:
            inst.op = IROp::CheckEnd;
            break;
        case OpCodeId::CheckBoundary:
            inst.op = IROp::CheckBoundary;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::Save:
            inst.op = IROp::Save;
            break;
        case OpCodeId::Restore:
            inst.op = IROp::Restore;
            break;
        case OpCodeId::Checkpoint:
            inst.op = IROp::Checkpoint;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::GoBack:
            inst.op = IROp::GoBack;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::SetStepBack:
            inst.op = IROp::SetStepBack;
            // Step value can be max u64 for unbounded lookbehinds.
            // Store full u64 in compare_data to avoid truncation.
            inst.compare_start = ir.compare_data.size();
            inst.compare_size = 1;
            ir.compare_data.append(flat[ip + 1]);
            break;
        case OpCodeId::IncStepBack:
            inst.op = IROp::IncStepBack;
            break;
        case OpCodeId::CheckStepBack:
            inst.op = IROp::CheckStepBack;
            break;
        case OpCodeId::CheckSavedPosition:
            inst.op = IROp::CheckSavedPosition;
            break;
        case OpCodeId::SaveLeftCaptureGroup:
            inst.op = IROp::SaveLeftCapture;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::SaveRightCaptureGroup:
            inst.op = IROp::SaveRightCapture;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::SaveRightNamedCaptureGroup:
            inst.op = IROp::SaveRightNamedCapture;
            // name_index is a full 64-bit ByteCodeValueType (serial << 32 | local_index).
            // Store it in compare_data to avoid truncation to u32.
            inst.compare_start = ir.compare_data.size();
            inst.compare_size = 1;
            ir.compare_data.append(flat[ip + 1]);       // name_index (full u64)
            inst.arg0 = static_cast<u32>(flat[ip + 2]); // group_id
            break;
        case OpCodeId::ClearCaptureGroup:
            inst.op = IROp::ClearCaptureGroup;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::ResetRepeat:
            inst.op = IROp::ResetRepeat;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::FailIfEmpty:
            inst.op = IROp::FailIfEmpty;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::SaveModifiers:
            inst.op = IROp::SaveModifiers;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        case OpCodeId::RestoreModifiers:
            inst.op = IROp::RestoreModifiers;
            break;
        case OpCodeId::Exit:
            inst.op = IROp::Exit;
            break;
        case OpCodeId::FailForks:
            inst.op = IROp::FailForks;
            break;
        case OpCodeId::PopSaved:
            inst.op = IROp::PopSaved;
            break;
        case OpCodeId::RSeekTo:
            inst.op = IROp::RSeekTo;
            inst.arg0 = static_cast<u32>(flat[ip + 1]);
            break;
        }

        ir.insts.append(inst);
        ip += size;
    }

    if (auto label = target_labels.get(bytecode_size); label.has_value())
        ir.insts.append({ .op = IROp::Label, .target = *label });

    ir.string_table = move(bytecode.m_string_table);
    ir.u16_string_table = move(bytecode.m_u16_string_table);
    ir.string_set_table = move(bytecode.m_string_set_table);
    ir.group_name_mappings = move(bytecode.m_group_name_mappings);

    return ir;
}

static bool should_emit_as_simple(Inst const& inst, Span<ByteCodeValueType const> compare_data)
{
    if (inst.op != IROp::Compare || inst.arg0 != 1)
        return false;

    if (inst.compare_size == 0)
        return false;

    auto first_type = static_cast<CharacterCompareType>(compare_data[inst.compare_start]);
    switch (first_type) {
    case CharacterCompareType::And:
    case CharacterCompareType::Or:
    case CharacterCompareType::Inverse:
    case CharacterCompareType::TemporaryInverse:
    case CharacterCompareType::Subtract:
    case CharacterCompareType::Undefined:
        return false;
    default:
        return true;
    }
}

static size_t ir_inst_size(Inst const& inst)
{
    switch (inst.op) {
    case IROp::Nop:
    case IROp::Label:
        return 0;
    case IROp::Compare:
        return sizeof(Op_Compare) + inst.compare_size * sizeof(ByteCodeValueType);
    case IROp::Jump:
    case IROp::ForkJump:
    case IROp::ForkStay:
    case IROp::ForkReplaceJump:
    case IROp::ForkReplaceStay:
        return sizeof(Op_Jump);
    case IROp::JumpNonEmpty:
        return sizeof(Op_JumpNonEmpty);
    case IROp::ForkIf:
        return sizeof(Op_ForkIf);
    case IROp::Repeat:
        return sizeof(Op_Repeat);
    case IROp::SetStepBack:
        return sizeof(Op_SetStepBack);
    case IROp::SaveRightNamedCapture:
        return sizeof(Op_SaveRightNamedCapture);
    case IROp::GoBack:
    case IROp::SaveLeftCapture:
    case IROp::SaveRightCapture:
    case IROp::ClearCaptureGroup:
    case IROp::FailIfEmpty:
    case IROp::ResetRepeat:
    case IROp::Checkpoint:
    case IROp::CheckBoundary:
    case IROp::RSeekTo:
    case IROp::SaveModifiers:
        return sizeof(Op_WithArg);
    case IROp::FailForks:
    case IROp::PopSaved:
    case IROp::Save:
    case IROp::Restore:
    case IROp::CheckBegin:
    case IROp::CheckEnd:
    case IROp::IncStepBack:
    case IROp::CheckStepBack:
    case IROp::CheckSavedPosition:
    case IROp::RestoreModifiers:
    case IROp::Exit:
        return sizeof(RegexInstruction);
    }
    VERIFY_NOT_REACHED();
}

static OpCodeId irop_to_opcode_id(IROp op, bool emit_simple)
{
    switch (op) {
    case IROp::Compare:
        return emit_simple ? OpCodeId::CompareSimple : OpCodeId::Compare;
    case IROp::Jump:
        return OpCodeId::Jump;
    case IROp::ForkJump:
        return OpCodeId::ForkJump;
    case IROp::ForkStay:
        return OpCodeId::ForkStay;
    case IROp::ForkReplaceJump:
        return OpCodeId::ForkReplaceJump;
    case IROp::ForkReplaceStay:
        return OpCodeId::ForkReplaceStay;
    case IROp::ForkIf:
        return OpCodeId::ForkIf;
    case IROp::JumpNonEmpty:
        return OpCodeId::JumpNonEmpty;
    case IROp::Repeat:
        return OpCodeId::Repeat;
    case IROp::CheckBegin:
        return OpCodeId::CheckBegin;
    case IROp::CheckEnd:
        return OpCodeId::CheckEnd;
    case IROp::CheckBoundary:
        return OpCodeId::CheckBoundary;
    case IROp::Save:
        return OpCodeId::Save;
    case IROp::Restore:
        return OpCodeId::Restore;
    case IROp::Checkpoint:
        return OpCodeId::Checkpoint;
    case IROp::GoBack:
        return OpCodeId::GoBack;
    case IROp::SetStepBack:
        return OpCodeId::SetStepBack;
    case IROp::IncStepBack:
        return OpCodeId::IncStepBack;
    case IROp::CheckStepBack:
        return OpCodeId::CheckStepBack;
    case IROp::CheckSavedPosition:
        return OpCodeId::CheckSavedPosition;
    case IROp::SaveLeftCapture:
        return OpCodeId::SaveLeftCaptureGroup;
    case IROp::SaveRightCapture:
        return OpCodeId::SaveRightCaptureGroup;
    case IROp::SaveRightNamedCapture:
        return OpCodeId::SaveRightNamedCaptureGroup;
    case IROp::ClearCaptureGroup:
        return OpCodeId::ClearCaptureGroup;
    case IROp::ResetRepeat:
        return OpCodeId::ResetRepeat;
    case IROp::FailIfEmpty:
        return OpCodeId::FailIfEmpty;
    case IROp::SaveModifiers:
        return OpCodeId::SaveModifiers;
    case IROp::RestoreModifiers:
        return OpCodeId::RestoreModifiers;
    case IROp::Exit:
        return OpCodeId::Exit;
    case IROp::FailForks:
        return OpCodeId::FailForks;
    case IROp::PopSaved:
        return OpCodeId::PopSaved;
    case IROp::RSeekTo:
        return OpCodeId::RSeekTo;
    case IROp::Nop:
    case IROp::Label:
        break;
    }
    VERIFY_NOT_REACHED();
}

template<typename T>
static void emit_insn(Vector<u8>& data, T const& insn)
{
    auto const* bytes = reinterpret_cast<u8 const*>(&insn);
    data.append(bytes, sizeof(T));
}

FlatByteCode lower_ir(RegexIR&& ir)
{
    HashMap<LabelId, size_t> label_pos;
    size_t offset = 0;

    bool has_checkpoints = false;
    bool has_repetitions = false;
    u32 max_checkpoint_id = 0;
    u32 max_repetition_id = 0;

    for (auto const& inst : ir.insts) {
        if (inst.op == IROp::Label) {
            label_pos.set(inst.target, offset);
            continue;
        }
        if (inst.op == IROp::Nop)
            continue;

        if (inst.op == IROp::Checkpoint || inst.op == IROp::FailIfEmpty) {
            has_checkpoints = true;
            max_checkpoint_id = max(max_checkpoint_id, inst.arg0);
        } else if (inst.op == IROp::Repeat || inst.op == IROp::ResetRepeat) {
            has_repetitions = true;
            u32 rid = (inst.op == IROp::Repeat) ? inst.arg1 : inst.arg0;
            max_repetition_id = max(max_repetition_id, rid);
        }

        offset += ir_inst_size(inst);
    }

    FlatByteCode result;
    result.m_data.ensure_capacity(offset + sizeof(RegexInstruction)); // +Exit at end
    result.m_checkpoint_count = has_checkpoints ? max_checkpoint_id + 1 : 0;
    result.m_repetition_count = has_repetitions ? max_repetition_id + 1 : 0;

    auto resolve_target = [&](LabelId label) -> u32 {
        return static_cast<u32>(*label_pos.get(label));
    };

    for (auto const& inst : ir.insts) {
        if (inst.op == IROp::Label || inst.op == IROp::Nop)
            continue;

        bool emit_simple = should_emit_as_simple(inst, ir.compare_data.span());
        auto opcode_id = irop_to_opcode_id(inst.op, emit_simple);

        switch (inst.op) {
        case IROp::Compare: {
            Op_Compare op {};
            op.m_type = opcode_id;
            op.m_arg_count = emit_simple ? 1 : inst.arg0;
            op.m_compare_size = inst.compare_size;
            emit_insn(result.m_data, op);
            // Also add inline compare subprogram.
            auto const* compare_bytes = reinterpret_cast<u8 const*>(ir.compare_data.data() + inst.compare_start);
            result.m_data.append(compare_bytes, inst.compare_size * sizeof(ByteCodeValueType));
            break;
        }
        case IROp::Jump:
        case IROp::ForkJump:
        case IROp::ForkStay:
        case IROp::ForkReplaceJump:
        case IROp::ForkReplaceStay: {
            Op_Jump op {};
            op.m_type = opcode_id;
            op.m_target = resolve_target(inst.target);
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::JumpNonEmpty: {
            Op_JumpNonEmpty op {};
            op.m_type = OpCodeId::JumpNonEmpty;
            op.m_target = resolve_target(inst.target);
            op.m_checkpoint_id = inst.arg0;
            op.m_form = inst.arg1;
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::ForkIf: {
            Op_ForkIf op {};
            op.m_type = OpCodeId::ForkIf;
            op.m_target = resolve_target(inst.target);
            op.m_form = inst.arg0;
            op.m_condition = inst.arg1;
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::Repeat: {
            Op_Repeat op {};
            op.m_type = OpCodeId::Repeat;
            op.m_target = resolve_target(inst.target);
            op.m_count = inst.arg0;
            op.m_id = inst.arg1;
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::SetStepBack: {
            Op_SetStepBack op {};
            op.m_type = OpCodeId::SetStepBack;
            op.m_step = static_cast<i64>(ir.compare_data[inst.compare_start]);
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::SaveRightNamedCapture: {
            Op_SaveRightNamedCapture op {};
            op.m_type = OpCodeId::SaveRightNamedCaptureGroup;
            op.m_name_index = ir.compare_data[inst.compare_start];
            op.m_group_id = inst.arg0;
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::GoBack:
        case IROp::SaveLeftCapture:
        case IROp::SaveRightCapture:
        case IROp::ClearCaptureGroup:
        case IROp::FailIfEmpty:
        case IROp::ResetRepeat:
        case IROp::Checkpoint:
        case IROp::CheckBoundary:
        case IROp::RSeekTo:
        case IROp::SaveModifiers: {
            Op_WithArg op {};
            op.m_type = opcode_id;
            op.m_arg0 = inst.arg0;
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::FailForks:
        case IROp::PopSaved:
        case IROp::Save:
        case IROp::Restore:
        case IROp::CheckBegin:
        case IROp::CheckEnd:
        case IROp::IncStepBack:
        case IROp::CheckStepBack:
        case IROp::CheckSavedPosition:
        case IROp::RestoreModifiers:
        case IROp::Exit: {
            RegexInstruction op {};
            op.m_type = opcode_id;
            emit_insn(result.m_data, op);
            break;
        }
        case IROp::Nop:
        case IROp::Label:
            VERIFY_NOT_REACHED();
        }
    }

    RegexInstruction exit_op {};
    exit_op.m_type = OpCodeId::Exit;
    emit_insn(result.m_data, exit_op);

    result.m_string_table = move(ir.string_table);
    result.m_u16_string_table = move(ir.u16_string_table);
    result.m_string_set_table = move(ir.string_set_table);
    result.m_group_name_mappings = move(ir.group_name_mappings);

    return result;
}

StringView irop_name(IROp op)
{
    switch (op) {
#define M(x)      \
    case IROp::x: \
        return #x##sv;
        ENUMERATE_REGEX_IR_OPS(M)
#undef M
    }
    return "???"sv;
}

void compact_ir(RegexIR& ir)
{
    HashTable<LabelId> referenced_labels;
    for (auto const& inst : ir.insts) {
        switch (inst.op) {
        case IROp::Jump:
        case IROp::ForkJump:
        case IROp::ForkStay:
        case IROp::ForkReplaceJump:
        case IROp::ForkReplaceStay:
        case IROp::ForkIf:
        case IROp::JumpNonEmpty:
        case IROp::Repeat:
            referenced_labels.set(inst.target);
            break;
        default:
            break;
        }
    }

    auto old_size = ir.insts.size();
    ir.insts.remove_all_matching([&](Inst const& inst) {
        if (inst.op == IROp::Nop)
            return true;
        if (inst.op == IROp::Label && !referenced_labels.contains(inst.target))
            return true;
        return false;
    });
    dbgln_if(REGEX_DEBUG, "IR compact: {} -> {} instructions ({} removed)", old_size, ir.insts.size(), old_size - ir.insts.size());
}

}
