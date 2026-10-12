/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

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

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TextEditingCommand const&);

template<>
WEBCOMMON_API ErrorOr<Web::TextEditingCommand> decode(Decoder&);

}
