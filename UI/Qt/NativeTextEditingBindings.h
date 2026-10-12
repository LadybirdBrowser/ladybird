/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Vector.h>
#include <LibWebCommon/Page/TextEditingCommand.h>

class QKeyEvent;

namespace Ladybird {

struct NativeTextEditingBindingResult {
    bool binding_resolved { false };
    Vector<Web::TextEditingCommand> commands;
};

class NativeTextEditingBindings {
    AK_MAKE_NONCOPYABLE(NativeTextEditingBindings);
    AK_MAKE_NONMOVABLE(NativeTextEditingBindings);

public:
    NativeTextEditingBindings();
    ~NativeTextEditingBindings();

    NativeTextEditingBindingResult commands_for_key_event(QKeyEvent const&);

private:
    void* m_recorder { nullptr };
};

}
