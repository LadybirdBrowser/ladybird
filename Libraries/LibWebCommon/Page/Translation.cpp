/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/Translation.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TranslationRun const& run)
{
    TRY(encoder.encode(run.text));
    TRY(encoder.encode(run.token));
    TRY(encoder.encode(run.keep_as_is));
    return {};
}

template<>
ErrorOr<Web::TranslationRun> IPC::decode(Decoder& decoder)
{
    auto text = TRY(decoder.decode<Utf16String>());
    auto token = TRY(decoder.decode<u32>());
    auto keep_as_is = TRY(decoder.decode<bool>());
    return Web::TranslationRun { move(text), token, keep_as_is };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TranslationUnit const& unit)
{
    TRY(encoder.encode(unit.id));
    TRY(encoder.encode(unit.runs));
    return {};
}

template<>
ErrorOr<Web::TranslationUnit> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u64>());
    auto runs = TRY(decoder.decode<Vector<Web::TranslationRun>>());
    return Web::TranslationUnit { id, move(runs) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TranslatedUnit const& unit)
{
    TRY(encoder.encode(unit.id));
    TRY(encoder.encode(unit.runs));
    return {};
}

template<>
ErrorOr<Web::TranslatedUnit> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u64>());
    auto runs = TRY(decoder.decode<Optional<Vector<Web::TranslationRun>>>());
    return Web::TranslatedUnit { id, move(runs) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TranslationLanguageSample const& sample)
{
    TRY(encoder.encode(sample.declared_language));
    TRY(encoder.encode(sample.text));
    return {};
}

template<>
ErrorOr<Web::TranslationLanguageSample> IPC::decode(Decoder& decoder)
{
    auto declared_language = TRY(decoder.decode<Optional<String>>());
    auto text = TRY(decoder.decode<Utf16String>());
    return Web::TranslationLanguageSample { move(declared_language), move(text) };
}
