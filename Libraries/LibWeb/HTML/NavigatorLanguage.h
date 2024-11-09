/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::HTML {

class NavigatorLanguageMixin {
public:
    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-language
    String language() const { return ResourceLoader::the().preferred_languages()[0]; }

    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-languages
    Vector<String> languages() const { return ResourceLoader::the().preferred_languages(); }
};

}
