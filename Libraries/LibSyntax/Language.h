/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>

namespace Syntax {

enum class Language {
    CMake,
    CMakeCache,
    Cpp,
    CSS,
    GitCommit,
    GML,
    HTML,
    INI,
    JavaScript,
    Markdown,
    PlainText,
    Shell,
};

StringView language_to_string(Language);
StringView common_language_extension(Language);
Optional<Language> language_from_name(StringView);
Optional<Language> language_from_filename(LexicalPath const&);

}
