/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/DOM/CharacterData.h>

namespace Web::DOM {

// NB: libweb_rust defines, allocates and destroys every ProcessingInstruction, see Rust/src/dom/node_objects.rs.
class ProcessingInstruction final : public CharacterData {
    WEB_WRAPPABLE(ProcessingInstruction, CharacterData);

public:
    [[nodiscard]] static GC::Ref<ProcessingInstruction> create(Document&, Utf16String data, Utf16FlyString const& target);

    virtual Utf16FlyString node_name() const override { return target(); }

    Utf16FlyString const& target() const { return m_processing_instruction_core.target; }

private:
    friend struct RustNodeObjectHooks;

    explicit ProcessingInstruction(VtableProbe);
    virtual ~ProcessingInstruction() override = default;

    RustFFI::ProcessingInstructionCore m_processing_instruction_core;
};

}
