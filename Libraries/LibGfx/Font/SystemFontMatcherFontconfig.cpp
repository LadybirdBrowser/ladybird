/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/GlobalFontConfig.h>
#include <LibGfx/Font/SystemFontMatcher.h>

#include <fontconfig/fontconfig.h>

namespace Gfx::SystemFontMatcher {

namespace {

// Destroys a fontconfig object at the end of its scope.
template<typename T, void (*destroy)(T*)>
class Owned {
    AK_MAKE_NONCOPYABLE(Owned);

public:
    explicit Owned(T* object)
        : m_object(object)
    {
    }

    Owned(Owned&& other)
        : m_object(exchange(other.m_object, nullptr))
    {
    }

    ~Owned()
    {
        if (m_object)
            destroy(m_object);
    }

    T* get() const { return m_object; }

private:
    T* m_object { nullptr };
};

using OwnedPattern = Owned<FcPattern, FcPatternDestroy>;
using OwnedCharSet = Owned<FcCharSet, FcCharSetDestroy>;
using OwnedLangSet = Owned<FcLangSet, FcLangSetDestroy>;

// The file and the face index of a matched font. The face is loaded after the lock is released.
struct MatchedFile {
    ByteString path;
    u32 ttc_index { 0 };
};

}

// The configuration is shared by all threads, so one match runs at a time.
static Mutex& fontconfig_mutex()
{
    static NeverDestroyed<Mutex> mutex;
    return *mutex;
}

static FcChar8 const* fontconfig_string(char const* string)
{
    return reinterpret_cast<FcChar8 const*>(string);
}

// Calls the callback with each string value of an object of a pattern.
template<typename Callback>
static void for_each_string(FcPattern* pattern, char const* object, Callback callback)
{
    FcChar8* value = nullptr;
    for (int id = 0; FcPatternGetString(pattern, object, id, &value) == FcResultMatch; ++id)
        callback(value, id);
}

static void add_style(FcPattern* pattern, u16 weight, u16 width, u8 slope)
{
    // https://learn.microsoft.com/en-us/typography/opentype/spec/os2#uswidthclass
    // Fontconfig gives a width in percent of the normal width, as the width classes of the OS/2 table do.
    static constexpr Array<int, 9> widths {
        FC_WIDTH_ULTRACONDENSED,
        FC_WIDTH_EXTRACONDENSED,
        FC_WIDTH_CONDENSED,
        FC_WIDTH_SEMICONDENSED,
        FC_WIDTH_NORMAL,
        FC_WIDTH_SEMIEXPANDED,
        FC_WIDTH_EXPANDED,
        FC_WIDTH_EXTRAEXPANDED,
        FC_WIDTH_ULTRAEXPANDED,
    };
    FcPatternAddInteger(pattern, FC_WEIGHT, FcWeightFromOpenType(clamp<int>(weight, 1, 1000)));
    FcPatternAddInteger(pattern, FC_WIDTH, widths[clamp<u16>(width, FontWidth::UltraCondensed, FontWidth::UltraExpanded) - 1]);
    FcPatternAddInteger(pattern, FC_SLANT, fontconfig_slant(slope));
}

// A family name that no font has. Put after the requested family, it splits the families of a pattern after
// substitution: the configuration puts the families that it substitutes for the requested family, such as the fonts
// that an alias stands for, before it, and the families that it adds to every pattern as defaults after it.
static constexpr char const* end_of_substitutes = "ladybird-end-of-family-substitutes";

static Optional<MatchedFile> matched_file(FcPattern* font)
{
    FcChar8* path = nullptr;
    if (FcPatternGetString(font, FC_FILE, 0, &path) != FcResultMatch)
        return {};
    // The upper 16 bits of the index select a named instance of a variable face.
    int index = 0;
    FcPatternGetInteger(font, FC_INDEX, 0, &index);
    return MatchedFile { reinterpret_cast<char const*>(path), static_cast<u32>(index) };
}

// The font for a family and a style, if it has the family or one that the configuration substitutes for it.
static OwnedPattern match_font_for_family(StringView family, u16 weight, u16 width, u8 slope)
{
    auto* config = GlobalFontConfig::the().get();

    OwnedPattern pattern { FcPatternCreate() };
    auto family_name = family.to_byte_string();
    FcPatternAddString(pattern.get(), FC_FAMILY, fontconfig_string(family_name.characters()));
    FcPatternAddString(pattern.get(), FC_FAMILY, fontconfig_string(end_of_substitutes));
    add_style(pattern.get(), weight, width, slope);
    FcConfigSubstitute(config, pattern.get(), FcMatchPattern);

    // A rule can replace the requested family itself, so it is accepted whatever the configuration does with it.
    Vector<ByteString> accepted_families { family_name };
    Optional<int> end_id;
    for_each_string(pattern.get(), FC_FAMILY, [&](FcChar8 const* name, int id) {
        if (end_id.has_value())
            return;
        if (FcStrCmp(name, fontconfig_string(end_of_substitutes)) == 0)
            end_id = id;
        else
            accepted_families.append(reinterpret_cast<char const*>(name));
    });
    if (end_id.has_value())
        FcPatternRemove(pattern.get(), FC_FAMILY, *end_id);

    FcDefaultSubstitute(pattern.get());
    FcResult result;
    OwnedPattern font { FcFontMatch(config, pattern.get(), &result) };
    if (!font.get())
        return font;

    bool has_accepted_family = false;
    for_each_string(font.get(), FC_FAMILY, [&](FcChar8 const* name, int) {
        for (auto const& accepted_family : accepted_families) {
            if (FcStrCmpIgnoreCase(name, fontconfig_string(accepted_family.characters())) == 0)
                has_accepted_family = true;
        }
    });
    if (!has_accepted_family)
        return OwnedPattern { nullptr };
    return font;
}

// The font for a code point and a style, if it has the code point.
static Optional<MatchedFile> match_file_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    auto* config = GlobalFontConfig::the().get();

    OwnedPattern pattern { FcPatternCreate() };
    add_style(pattern.get(), weight, width, slope);
    OwnedCharSet char_set { FcCharSetCreate() };
    FcCharSetAddChar(char_set.get(), code_point);
    FcPatternAddCharSet(pattern.get(), FC_CHARSET, char_set.get());

    // https://www.unicode.org/reports/tr51/#Emoji_Script
    // The script subtag Zsye asks for emoji presentation, which fontconfig gives to the color emoji fonts.
    if (prefer_color_emoji) {
        OwnedLangSet lang_set { FcLangSetCreate() };
        FcLangSetAdd(lang_set.get(), fontconfig_string("und-zsye"));
        FcPatternAddLangSet(pattern.get(), FC_LANG, lang_set.get());
    }

    FcConfigSubstitute(config, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());
    FcResult result;
    OwnedPattern font { FcFontMatch(config, pattern.get(), &result) };
    if (!font.get())
        return {};

    FcCharSet* font_char_set = nullptr;
    if (FcPatternGetCharSet(font.get(), FC_CHARSET, 0, &font_char_set) != FcResultMatch || !FcCharSetHasChar(font_char_set, code_point))
        return {};
    return matched_file(font.get());
}

static Optional<SystemFontMatch> loadable_match(Optional<MatchedFile> const& file)
{
    if (!file.has_value())
        return {};
    auto loadable = loadable_file(file->path, file->ttc_index);
    if (!loadable.has_value())
        return {};
    return SystemFontMatch { loadable.release_value() };
}

Optional<SystemFontMatch> match_family_style(StringView family, u16 weight, u16 width, u8 slope)
{
    Optional<MatchedFile> file;
    {
        MutexLocker locker(fontconfig_mutex());
        if (auto font = match_font_for_family(family, weight, width, slope); font.get())
            file = matched_file(font.get());
    }
    return loadable_match(file);
}

Optional<SystemFontMatch> match_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    Optional<MatchedFile> file;
    {
        MutexLocker locker(fontconfig_mutex());
        file = match_file_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
    }
    return loadable_match(file);
}

Optional<FlyString> resolve_generic_family(StringView family, u16 weight, u8 slope)
{
    ByteString family_name;
    {
        MutexLocker locker(fontconfig_mutex());
        auto font = match_font_for_family(family, weight, FontWidth::Normal, slope);
        if (!font.get())
            return {};
        FcChar8* name = nullptr;
        if (FcPatternGetString(font.get(), FC_FAMILY, 0, &name) != FcResultMatch)
            return {};
        family_name = reinterpret_cast<char const*>(name);
    }
    auto resolved_family = FlyString::from_utf8(family_name.view());
    if (resolved_family.is_error())
        return {};
    return resolved_family.release_value();
}

}
