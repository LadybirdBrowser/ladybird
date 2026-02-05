/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ProcessingInstructionPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(ProcessingInstruction);

ProcessingInstruction::ProcessingInstruction(Document& document, Utf16String data, String const& target)
    : CharacterData(document, NodeType::PROCESSING_INSTRUCTION_NODE, move(data))
    , m_target(target)
{
}

void ProcessingInstruction::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ProcessingInstruction);
    Base::initialize(realm);
}

}
