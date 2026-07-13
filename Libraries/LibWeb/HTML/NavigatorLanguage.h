/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::HTML {

class NavigatorLanguageMixin {
public:
    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-language
    // FIXME: Honor WebDriver BiDi emulated language.
    Utf16String language() const
    {
        return Utf16String::from_ascii_without_validation(ResourceLoader::the().preferred_languages()[0].bytes());
    }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-languages
    // FIXME: Honor WebDriver BiDi emulated language.
    Vector<Utf16String> languages() const
    {
        auto const& preferred_languages = ResourceLoader::the().preferred_languages();
        Vector<Utf16String> languages;
        languages.ensure_capacity(preferred_languages.size());
        for (auto const& preferred_language : preferred_languages)
            languages.unchecked_append(Utf16String::from_ascii_without_validation(preferred_language.bytes()));
        return languages;
    }
};

}
