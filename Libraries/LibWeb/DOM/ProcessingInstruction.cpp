/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ProcessingInstruction.h>

namespace Web::DOM {

static_assert(is_shell_of_rust_object<ProcessingInstruction, RustFFI::ProcessingInstructionObject>);

ProcessingInstruction::ProcessingInstruction(VtableProbe probe)
    : CharacterData(probe)
{
    static_assert(offsetof(ProcessingInstruction, m_processing_instruction_core) == offsetof(RustFFI::ProcessingInstructionObject, core));
}

GC::Ref<ProcessingInstruction> ProcessingInstruction::create(Document& document, Utf16String data, Utf16FlyString const& target)
{
    return static_cast<ProcessingInstruction&>(*RustFFI::rust_dom_create_processing_instruction(&document, move(data).into_raw(), target.to_raw_leaked()));
}

}
