/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web {

// A stretch of a translation unit's text that comes from one place in the document. The translator carries each
// run's token over to the words that translate it, so the translated text can be put back where it came from, even
// when the translation reorders those words.
struct TranslationRun {
    Utf16String text;
    u32 token { 0 };
    // Text the translator must leave exactly as it is, e.g. the text of an element whose translation mode is
    // no-translate, inside a paragraph that's otherwise translated.
    bool keep_as_is { false };

    bool operator==(TranslationRun const&) const = default;
};

// A piece of a document's text that's translated as a whole, e.g. a paragraph.
struct TranslationUnit {
    u64 id { 0 };
    Vector<TranslationRun> runs;
};

// A unit's translation; nothing if the translator couldn't translate it.
struct TranslatedUnit {
    u64 id { 0 };
    Optional<Vector<TranslationRun>> runs;
};

// What a page offers for working out which language it's in.
struct TranslationLanguageSample {
    // The language of the document element, from its lang attribute or the Content-Language header.
    Optional<String> declared_language;
    Utf16String text;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TranslationRun const&);

template<>
WEBCOMMON_API ErrorOr<Web::TranslationRun> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TranslationUnit const&);

template<>
WEBCOMMON_API ErrorOr<Web::TranslationUnit> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TranslatedUnit const&);

template<>
WEBCOMMON_API ErrorOr<Web::TranslatedUnit> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TranslationLanguageSample const&);

template<>
WEBCOMMON_API ErrorOr<Web::TranslationLanguageSample> decode(Decoder&);

}
