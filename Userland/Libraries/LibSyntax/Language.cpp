/*
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Language.h"
#include <AK/LexicalPath.h>
#include <LibSyntax/Highlighter.h>

namespace Syntax {

StringView language_to_string(Language language)
{
    switch (language) {
    case Language::CMake:
        return "CMake"sv;
    case Language::CMakeCache:
        return "CMakeCache"sv;
    case Language::Cpp:
        return "C++"sv;
    case Language::CSS:
        return "CSS"sv;
    case Language::GitCommit:
        return "Git"sv;
    case Language::GML:
        return "GML"sv;
    case Language::HTML:
        return "HTML"sv;
    case Language::INI:
        return "INI"sv;
    case Language::JavaScript:
        return "JavaScript"sv;
    case Language::Markdown:
        return "Markdown"sv;
    case Language::PlainText:
        return "Plain Text"sv;
    case Language::Shell:
        return "Shell"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView common_language_extension(Language language)
{
    switch (language) {
    case Language::CMake:
        return "cmake"sv;
    case Language::CMakeCache:
        return {};
    case Language::Cpp:
        return "cpp"sv;
    case Language::CSS:
        return "css"sv;
    case Language::GitCommit:
        return {};
    case Language::GML:
        return "gml"sv;
    case Language::HTML:
        return "html"sv;
    case Language::INI:
        return "ini"sv;
    case Language::JavaScript:
        return "js"sv;
    case Language::Markdown:
        return "md"sv;
    case Language::PlainText:
        return "txt"sv;
    case Language::Shell:
        return "sh"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Language> language_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("CMake"sv))
        return Language::CMake;
    if (name.equals_ignoring_ascii_case("CMakeCache"sv))
        return Language::CMakeCache;
    if (name.equals_ignoring_ascii_case("Cpp"sv))
        return Language::Cpp;
    if (name.equals_ignoring_ascii_case("CSS"sv))
        return Language::CSS;
    if (name.equals_ignoring_ascii_case("GitCommit"sv))
        return Language::GitCommit;
    if (name.equals_ignoring_ascii_case("GML"sv))
        return Language::GML;
    if (name.equals_ignoring_ascii_case("HTML"sv))
        return Language::HTML;
    if (name.equals_ignoring_ascii_case("INI"sv))
        return Language::INI;
    if (name.equals_ignoring_ascii_case("JavaScript"sv))
        return Language::JavaScript;
    if (name.equals_ignoring_ascii_case("Markdown"sv))
        return Language::Markdown;
    if (name.equals_ignoring_ascii_case("PlainText"sv))
        return Language::PlainText;
    if (name.equals_ignoring_ascii_case("Shell"sv))
        return Language::Shell;

    return {};
}

Optional<Language> language_from_filename(LexicalPath const& file)
{
    if (file.title() == "COMMIT_EDITMSG"sv)
        return Language::GitCommit;

    auto extension = file.extension();
    VERIFY(!extension.starts_with('.'));
    if (extension == "cmake"sv || (extension == "txt"sv && file.title() == "CMakeLists"sv))
        return Language::CMake;
    if (extension == "txt"sv && file.title() == "CMakeCache"sv)
        return Language::CMakeCache;
    if (extension.is_one_of("c"sv, "cc"sv, "cxx"sv, "cpp"sv, "c++", "h"sv, "hh"sv, "hxx"sv, "hpp"sv, "h++"sv))
        return Language::Cpp;
    if (extension == "css"sv)
        return Language::CSS;
    if (extension == "gml"sv)
        return Language::GML;
    if (extension.is_one_of("html"sv, "htm"sv))
        return Language::HTML;
    if (extension.is_one_of("ini"sv, "af"sv))
        return Language::INI;
    if (extension.is_one_of("js"sv, "mjs"sv, "json"sv))
        return Language::JavaScript;
    if (extension == "md"sv)
        return Language::Markdown;
    if (extension.is_one_of("sh"sv, "bash"sv))
        return Language::Shell;

    // Check "txt" after the CMake related files that use "txt" as their extension.
    if (extension == "txt"sv)
        return Language::PlainText;

    return {};
}

}
