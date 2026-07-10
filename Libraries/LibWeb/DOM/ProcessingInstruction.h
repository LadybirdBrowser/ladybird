/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/DOM/CharacterData.h>

namespace Web::DOM {

class ProcessingInstruction final : public CharacterData {
    WEB_PLATFORM_OBJECT(ProcessingInstruction, CharacterData);
    GC_DECLARE_ALLOCATOR(ProcessingInstruction);

public:
    virtual ~ProcessingInstruction() override = default;

    virtual Utf16FlyString node_name() const override { return m_target; }

    Utf16FlyString const& target() const { return m_target; }

private:
    ProcessingInstruction(Document&, Utf16String data, Utf16FlyString const& target);

    virtual void initialize(JS::Realm&) override;

    Utf16FlyString m_target;
};

template<>
inline bool Node::fast_is<ProcessingInstruction>() const { return node_type() == (u16)NodeType::PROCESSING_INSTRUCTION_NODE; }

}
