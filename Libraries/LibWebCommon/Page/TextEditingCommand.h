/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web {

struct TextEditingCommand {
    enum class Type : u8 {
        MoveBackward,
        MoveForward,
        MoveWordBackward,
        MoveWordForward,
        MoveUp,
        MoveDown,
        MoveToBeginningOfParagraph,
        MoveToEndOfParagraph,
        MoveToBeginningOfLine,
        MoveToEndOfLine,
        MoveToLeftEndOfLine,
        MoveToRightEndOfLine,
        PageUp,
        PageDown,
        DeleteBackward,
        DeleteForward,
        DeleteWordBackward,
        DeleteWordForward,
        DeleteToEndOfParagraph,
    };

    Type type { Type::MoveBackward };
    bool extend_selection { false };

    bool operator==(TextEditingCommand const&) const = default;
};

}

#include <LibWebCommon/Page/TextEditingCommandCodecs.h>
