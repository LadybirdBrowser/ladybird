/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::Editing::CommandNames {

#define ENUMERATE_COMMAND_NAMES                         \
    __ENUMERATE_COMMAND_NAME(backColor)                 \
    __ENUMERATE_COMMAND_NAME(bold)                      \
    __ENUMERATE_COMMAND_NAME(copy)                      \
    __ENUMERATE_COMMAND_NAME(createLink)                \
    __ENUMERATE_COMMAND_NAME(cut)                       \
    __ENUMERATE_COMMAND_NAME(defaultParagraphSeparator) \
    __ENUMERATE_COMMAND_NAME(fontName)                  \
    __ENUMERATE_COMMAND_NAME(fontSize)                  \
    __ENUMERATE_COMMAND_NAME(foreColor)                 \
    __ENUMERATE_COMMAND_NAME(formatBlock)               \
    __ENUMERATE_COMMAND_NAME(forwardDelete)             \
    __ENUMERATE_COMMAND_NAME(hiliteColor)               \
    __ENUMERATE_COMMAND_NAME(indent)                    \
    __ENUMERATE_COMMAND_NAME(insertHTML)                \
    __ENUMERATE_COMMAND_NAME(insertHorizontalRule)      \
    __ENUMERATE_COMMAND_NAME(insertImage)               \
    __ENUMERATE_COMMAND_NAME(insertLineBreak)           \
    __ENUMERATE_COMMAND_NAME(insertOrderedList)         \
    __ENUMERATE_COMMAND_NAME(insertParagraph)           \
    __ENUMERATE_COMMAND_NAME(insertText)                \
    __ENUMERATE_COMMAND_NAME(insertUnorderedList)       \
    __ENUMERATE_COMMAND_NAME(italic)                    \
    __ENUMERATE_COMMAND_NAME(justifyCenter)             \
    __ENUMERATE_COMMAND_NAME(justifyFull)               \
    __ENUMERATE_COMMAND_NAME(justifyLeft)               \
    __ENUMERATE_COMMAND_NAME(justifyRight)              \
    __ENUMERATE_COMMAND_NAME(outdent)                   \
    __ENUMERATE_COMMAND_NAME(paste)                     \
    __ENUMERATE_COMMAND_NAME(redo)                      \
    __ENUMERATE_COMMAND_NAME(removeFormat)              \
    __ENUMERATE_COMMAND_NAME(selectAll)                 \
    __ENUMERATE_COMMAND_NAME(strikethrough)             \
    __ENUMERATE_COMMAND_NAME(styleWithCSS)              \
    __ENUMERATE_COMMAND_NAME(subscript)                 \
    __ENUMERATE_COMMAND_NAME(superscript)               \
    __ENUMERATE_COMMAND_NAME(underline)                 \
    __ENUMERATE_COMMAND_NAME(undo)                      \
    __ENUMERATE_COMMAND_NAME(unlink)                    \
    __ENUMERATE_COMMAND_NAME(useCSS)

#define __ENUMERATE_COMMAND_NAME(name) extern FlyString name;
ENUMERATE_COMMAND_NAMES
#undef __ENUMERATE_COMMAND_NAME

extern FlyString delete_;

void initialize_strings();

}
