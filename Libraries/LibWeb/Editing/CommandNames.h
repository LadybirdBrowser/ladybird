/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::Editing::CommandNames {

#define ENUMERATE_COMMAND_NAMES                                                      \
    __ENUMERATE_COMMAND_NAME(backColor, "backColor")                                 \
    __ENUMERATE_COMMAND_NAME(bold, "bold")                                           \
    __ENUMERATE_COMMAND_NAME(copy, "copy")                                           \
    __ENUMERATE_COMMAND_NAME(createLink, "createLink")                               \
    __ENUMERATE_COMMAND_NAME(cut, "cut")                                             \
    __ENUMERATE_COMMAND_NAME(defaultParagraphSeparator, "defaultParagraphSeparator") \
    __ENUMERATE_COMMAND_NAME(delete_, "delete")                                      \
    __ENUMERATE_COMMAND_NAME(fontName, "fontName")                                   \
    __ENUMERATE_COMMAND_NAME(fontSize, "fontSize")                                   \
    __ENUMERATE_COMMAND_NAME(foreColor, "foreColor")                                 \
    __ENUMERATE_COMMAND_NAME(formatBlock, "formatBlock")                             \
    __ENUMERATE_COMMAND_NAME(forwardDelete, "forwardDelete")                         \
    __ENUMERATE_COMMAND_NAME(hiliteColor, "hiliteColor")                             \
    __ENUMERATE_COMMAND_NAME(indent, "indent")                                       \
    __ENUMERATE_COMMAND_NAME(insertHorizontalRule, "insertHorizontalRule")           \
    __ENUMERATE_COMMAND_NAME(insertHTML, "insertHTML")                               \
    __ENUMERATE_COMMAND_NAME(insertImage, "insertImage")                             \
    __ENUMERATE_COMMAND_NAME(insertLineBreak, "insertLineBreak")                     \
    __ENUMERATE_COMMAND_NAME(insertOrderedList, "insertOrderedList")                 \
    __ENUMERATE_COMMAND_NAME(insertParagraph, "insertParagraph")                     \
    __ENUMERATE_COMMAND_NAME(insertText, "insertText")                               \
    __ENUMERATE_COMMAND_NAME(insertUnorderedList, "insertUnorderedList")             \
    __ENUMERATE_COMMAND_NAME(italic, "italic")                                       \
    __ENUMERATE_COMMAND_NAME(justifyCenter, "justifyCenter")                         \
    __ENUMERATE_COMMAND_NAME(justifyFull, "justifyFull")                             \
    __ENUMERATE_COMMAND_NAME(justifyLeft, "justifyLeft")                             \
    __ENUMERATE_COMMAND_NAME(justifyRight, "justifyRight")                           \
    __ENUMERATE_COMMAND_NAME(outdent, "outdent")                                     \
    __ENUMERATE_COMMAND_NAME(paste, "paste")                                         \
    __ENUMERATE_COMMAND_NAME(preserveWhitespace, "preserveWhitespace")               \
    __ENUMERATE_COMMAND_NAME(redo, "redo")                                           \
    __ENUMERATE_COMMAND_NAME(removeFormat, "removeFormat")                           \
    __ENUMERATE_COMMAND_NAME(selectAll, "selectAll")                                 \
    __ENUMERATE_COMMAND_NAME(strikethrough, "strikethrough")                         \
    __ENUMERATE_COMMAND_NAME(styleWithCSS, "styleWithCSS")                           \
    __ENUMERATE_COMMAND_NAME(subscript, "subscript")                                 \
    __ENUMERATE_COMMAND_NAME(superscript, "superscript")                             \
    __ENUMERATE_COMMAND_NAME(underline, "underline")                                 \
    __ENUMERATE_COMMAND_NAME(undo, "undo")                                           \
    __ENUMERATE_COMMAND_NAME(unlink, "unlink")                                       \
    __ENUMERATE_COMMAND_NAME(useCSS, "useCSS")

#define __ENUMERATE_COMMAND_NAME(name, command) extern FlyString name;
ENUMERATE_COMMAND_NAMES
#undef __ENUMERATE_COMMAND_NAME

}
