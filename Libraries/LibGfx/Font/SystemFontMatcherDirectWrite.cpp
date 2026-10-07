/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <AK/Windows.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/SystemFontMatcher.h>

#include <dwrite.h>
#include <dwrite_2.h>
// NB: WRL instead of WinRT, because WinRT reports errors with exceptions.
#include <wrl/client.h>

namespace Gfx::SystemFontMatcher {

using Microsoft::WRL::ComPtr;

namespace {

struct DirectWrite {
    ComPtr<IDWriteFactory2> factory;
    ComPtr<IDWriteFontCollection> system_fonts;
    ComPtr<IDWriteFontFallback> system_fallback;
    // The locale of the user, which picks among the fonts of a script, ending with a null character.
    Vector<wchar_t> user_locale;
};

// The text of one code point, which the font fallback of DirectWrite reads. It lives on the stack for one call, so it
// does not count references.
class CodePointText final : public IDWriteTextAnalysisSource {
public:
    CodePointText(u32 code_point, wchar_t const* locale)
        : m_locale(locale)
    {
        if (code_point < 0x10000) {
            m_code_units[0] = static_cast<wchar_t>(code_point);
            m_length = 1;
        } else {
            m_code_units[0] = static_cast<wchar_t>(0xD800 + ((code_point - 0x10000) >> 10));
            m_code_units[1] = static_cast<wchar_t>(0xDC00 + ((code_point - 0x10000) & 0x3FF));
            m_length = 2;
        }
    }

    UINT32 length() const { return m_length; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) override
    {
        if (interface_id != __uuidof(IUnknown) && interface_id != __uuidof(IDWriteTextAnalysisSource)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        *object = this;
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, WCHAR const** text, UINT32* length) override
    {
        *text = position < m_length ? m_code_units + position : nullptr;
        *length = position < m_length ? m_length - position : 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, WCHAR const** text, UINT32* length) override
    {
        *text = position > 0 && position <= m_length ? m_code_units : nullptr;
        *length = position > 0 && position <= m_length ? position : 0;
        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length, WCHAR const** locale) override
    {
        *length = position < m_length ? m_length - position : 0;
        *locale = m_locale;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 position, UINT32* length, IDWriteNumberSubstitution** number_substitution) override
    {
        // The text has no digits to substitute.
        *length = position < m_length ? m_length - position : 0;
        *number_substitution = nullptr;
        return S_OK;
    }

private:
    wchar_t m_code_units[2] {};
    UINT32 m_length { 0 };
    wchar_t const* m_locale { nullptr };
};

}

static DirectWrite const* direct_write()
{
    static DirectWrite const* const instance = []() -> DirectWrite const* {
        auto* direct_write = new DirectWrite;
        // IDWriteFactory2 and its system font fallback need Windows 8.1 or later.
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(direct_write->factory.GetAddressOf())))
            || FAILED(direct_write->factory->GetSystemFontCollection(&direct_write->system_fonts, FALSE))
            || FAILED(direct_write->factory->GetSystemFontFallback(&direct_write->system_fallback))) {
            delete direct_write;
            return nullptr;
        }

        wchar_t locale[LOCALE_NAME_MAX_LENGTH] {};
        auto locale_length = GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
        for (int index = 0; index < locale_length && locale[index] != 0; ++index)
            direct_write->user_locale.append(locale[index]);
        direct_write->user_locale.append(0);
        return direct_write;
    }();
    return instance;
}

static Vector<wchar_t> to_wide_string(StringView string)
{
    Vector<wchar_t> wide_string;
    if (!string.is_empty()) {
        auto length = MultiByteToWideChar(CP_UTF8, 0, string.characters_without_null_termination(), static_cast<int>(string.length()), nullptr, 0);
        if (length > 0) {
            wide_string.resize(length);
            MultiByteToWideChar(CP_UTF8, 0, string.characters_without_null_termination(), static_cast<int>(string.length()), wide_string.data(), length);
        }
    }
    wide_string.append(0);
    return wide_string;
}

static Optional<String> from_wide_string(wchar_t const* wide_string, UINT32 length)
{
    if (length == 0)
        return String {};
    auto size = WideCharToMultiByte(CP_UTF8, 0, wide_string, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    Vector<char> buffer;
    buffer.resize(size);
    WideCharToMultiByte(CP_UTF8, 0, wide_string, static_cast<int>(length), buffer.data(), size, nullptr, nullptr);
    auto string = String::from_utf8(StringView { buffer.data(), buffer.size() });
    if (string.is_error())
        return {};
    return string.release_value();
}

// DirectWrite uses the CSS weights and the width classes of the OS/2 table.
static DWRITE_FONT_WEIGHT direct_write_weight(u16 weight)
{
    return static_cast<DWRITE_FONT_WEIGHT>(clamp<u16>(weight, 1, 999));
}

static DWRITE_FONT_STRETCH direct_write_stretch(u16 width)
{
    return static_cast<DWRITE_FONT_STRETCH>(clamp<u16>(width, FontWidth::UltraCondensed, FontWidth::UltraExpanded));
}

static DWRITE_FONT_STYLE direct_write_style(u8 slope)
{
    if (slope == 1)
        return DWRITE_FONT_STYLE_ITALIC;
    if (slope == 2)
        return DWRITE_FONT_STYLE_OBLIQUE;
    return DWRITE_FONT_STYLE_NORMAL;
}

// The face of a font that DirectWrite gives, if it is in a local file that Typeface loads. DirectWrite gives a named
// instance of a variable font as the face at its default coordinates.
static Optional<SystemFontMatch> match_of(IDWriteFont* font)
{
    ComPtr<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(&face)))
        return {};

    // A face whose data is in several files cannot be loaded from one of them.
    UINT32 file_count = 0;
    if (FAILED(face->GetFiles(&file_count, nullptr)) || file_count != 1)
        return {};
    ComPtr<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&file_count, file.GetAddressOf())))
        return {};

    void const* reference_key = nullptr;
    UINT32 reference_key_size = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    ComPtr<IDWriteLocalFontFileLoader> local_loader;
    if (FAILED(file->GetReferenceKey(&reference_key, &reference_key_size)) || FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&local_loader)))
        return {};

    UINT32 path_length = 0;
    if (FAILED(local_loader->GetFilePathLengthFromKey(reference_key, reference_key_size, &path_length)))
        return {};
    Vector<wchar_t> path;
    path.resize(path_length + 1);
    if (FAILED(local_loader->GetFilePathFromKey(reference_key, reference_key_size, path.data(), path_length + 1)))
        return {};
    auto utf8_path = from_wide_string(path.data(), path_length);
    if (!utf8_path.has_value())
        return {};

    auto loadable = loadable_file(*utf8_path, face->GetIndex());
    if (!loadable.has_value())
        return {};
    return SystemFontMatch { loadable.release_value() };
}

static ComPtr<IDWriteFont> match_font_for_family(StringView family, u16 weight, u16 width, u8 slope)
{
    auto const* direct_write = SystemFontMatcher::direct_write();
    if (!direct_write)
        return {};

    auto family_name = to_wide_string(family);
    UINT32 family_index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> font_family;
    if (FAILED(direct_write->system_fonts->FindFamilyName(family_name.data(), &family_index, &exists)) || !exists
        || FAILED(direct_write->system_fonts->GetFontFamily(family_index, &font_family)))
        return {};

    ComPtr<IDWriteFont> font;
    if (FAILED(font_family->GetFirstMatchingFont(direct_write_weight(weight), direct_write_stretch(width), direct_write_style(slope), &font)))
        return {};
    return font;
}

Optional<SystemFontMatch> match_family_style(StringView family, u16 weight, u16 width, u8 slope)
{
    auto font = match_font_for_family(family, weight, width, slope);
    if (!font)
        return {};
    return match_of(font.Get());
}

Optional<SystemFontMatch> match_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    auto const* direct_write = SystemFontMatcher::direct_write();
    if (!direct_write || code_point > 0x10FFFF)
        return {};

    // https://www.unicode.org/reports/tr51/#Emoji_Script
    // The script subtag Zsye asks for emoji presentation, which DirectWrite gives with a color emoji font.
    CodePointText text { code_point, prefer_color_emoji ? L"und-Zsye" : direct_write->user_locale.data() };
    UINT32 mapped_length = 0;
    ComPtr<IDWriteFont> font;
    FLOAT scale = 1;
    if (FAILED(direct_write->system_fallback->MapCharacters(&text, 0, text.length(), direct_write->system_fonts.Get(), nullptr, direct_write_weight(weight), direct_write_style(slope), direct_write_stretch(width), &mapped_length, &font, &scale)))
        return {};
    if (!font)
        return {};
    return match_of(font.Get());
}

Optional<FlyString> resolve_generic_family(StringView family, u16 weight, u8 slope)
{
    auto font = match_font_for_family(family, weight, FontWidth::Normal, slope);
    if (!font)
        return {};

    ComPtr<IDWriteFontFamily> font_family;
    ComPtr<IDWriteLocalizedStrings> family_names;
    if (FAILED(font->GetFontFamily(&font_family)) || FAILED(font_family->GetFamilyNames(&family_names)))
        return {};

    // The US English name, or else the first name.
    UINT32 name_index = 0;
    BOOL exists = FALSE;
    if (FAILED(family_names->FindLocaleName(L"en-us", &name_index, &exists)) || !exists)
        name_index = 0;

    UINT32 name_length = 0;
    if (FAILED(family_names->GetStringLength(name_index, &name_length)))
        return {};
    Vector<wchar_t> name;
    name.resize(name_length + 1);
    if (FAILED(family_names->GetString(name_index, name.data(), name_length + 1)))
        return {};
    auto resolved_family = from_wide_string(name.data(), name_length);
    if (!resolved_family.has_value())
        return {};
    return FlyString { resolved_family.release_value() };
}

}
