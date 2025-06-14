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
        return "CMake"_sv;
    case Language::CMakeCache:
        return "CMakeCache"_sv;
    case Language::Cpp:
        return "C++"_sv;
    case Language::CSS:
        return "CSS"_sv;
    case Language::GitCommit:
        return "Git"_sv;
    case Language::GML:
        return "GML"_sv;
    case Language::HTML:
        return "HTML"_sv;
    case Language::INI:
        return "INI"_sv;
    case Language::JavaScript:
        return "JavaScript"_sv;
    case Language::Markdown:
        return "Markdown"_sv;
    case Language::PlainText:
        return "Plain Text"_sv;
    case Language::Shell:
        return "Shell"_sv;
    }
    VERIFY_NOT_REACHED();
}

StringView common_language_extension(Language language)
{
    switch (language) {
    case Language::CMake:
        return "cmake"_sv;
    case Language::CMakeCache:
        return {};
    case Language::Cpp:
        return "cpp"_sv;
    case Language::CSS:
        return "css"_sv;
    case Language::GitCommit:
        return {};
    case Language::GML:
        return "gml"_sv;
    case Language::HTML:
        return "html"_sv;
    case Language::INI:
        return "ini"_sv;
    case Language::JavaScript:
        return "js"_sv;
    case Language::Markdown:
        return "md"_sv;
    case Language::PlainText:
        return "txt"_sv;
    case Language::Shell:
        return "sh"_sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Language> language_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("CMake"_sv))
        return Language::CMake;
    if (name.equals_ignoring_ascii_case("CMakeCache"_sv))
        return Language::CMakeCache;
    if (name.equals_ignoring_ascii_case("Cpp"_sv))
        return Language::Cpp;
    if (name.equals_ignoring_ascii_case("CSS"_sv))
        return Language::CSS;
    if (name.equals_ignoring_ascii_case("GitCommit"_sv))
        return Language::GitCommit;
    if (name.equals_ignoring_ascii_case("GML"_sv))
        return Language::GML;
    if (name.equals_ignoring_ascii_case("HTML"_sv))
        return Language::HTML;
    if (name.equals_ignoring_ascii_case("INI"_sv))
        return Language::INI;
    if (name.equals_ignoring_ascii_case("JavaScript"_sv))
        return Language::JavaScript;
    if (name.equals_ignoring_ascii_case("Markdown"_sv))
        return Language::Markdown;
    if (name.equals_ignoring_ascii_case("PlainText"_sv))
        return Language::PlainText;
    if (name.equals_ignoring_ascii_case("Shell"_sv))
        return Language::Shell;

    return {};
}

Optional<Language> language_from_filename(LexicalPath const& file)
{
    if (file.title() == "COMMIT_EDITMSG"_sv)
        return Language::GitCommit;

    auto extension = file.extension();
    VERIFY(!extension.starts_with('.'));
    if (extension == "cmake"_sv || (extension == "txt"_sv && file.title() == "CMakeLists"_sv))
        return Language::CMake;
    if (extension == "txt"_sv && file.title() == "CMakeCache"_sv)
        return Language::CMakeCache;
    if (extension.is_one_of("c"_sv, "cc"_sv, "cxx"_sv, "cpp"_sv, "c++", "h"_sv, "hh"_sv, "hxx"_sv, "hpp"_sv, "h++"_sv))
        return Language::Cpp;
    if (extension == "css"_sv)
        return Language::CSS;
    if (extension == "gml"_sv)
        return Language::GML;
    if (extension.is_one_of("html"_sv, "htm"_sv))
        return Language::HTML;
    if (extension.is_one_of("ini"_sv, "af"_sv))
        return Language::INI;
    if (extension.is_one_of("js"_sv, "mjs"_sv, "json"_sv))
        return Language::JavaScript;
    if (extension == "md"_sv)
        return Language::Markdown;
    if (extension.is_one_of("sh"_sv, "bash"_sv))
        return Language::Shell;

    // Check "txt" after the CMake related files that use "txt" as their extension.
    if (extension == "txt"_sv)
        return Language::PlainText;

    return {};
}

}
