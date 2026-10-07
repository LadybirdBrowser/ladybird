/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Noncopyable.h>
#include <AK/QuickSort.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontTable.h>
#include <LibGfx/Font/SystemFontMatcher.h>
#include <LibGfx/Font/TypefaceCoreText.h>

#include <CoreText/CoreText.h>
#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>
#include <sys/param.h>

namespace Gfx::SystemFontMatcher {

namespace {

// Releases a CoreFoundation object at the end of its scope.
template<typename T>
class CFOwned {
    AK_MAKE_NONCOPYABLE(CFOwned);

public:
    CFOwned() = default;

    explicit CFOwned(T object)
        : m_object(object)
    {
    }

    CFOwned(CFOwned&& other)
        : m_object(exchange(other.m_object, nullptr))
    {
    }

    CFOwned& operator=(CFOwned&& other)
    {
        if (this != &other) {
            release();
            m_object = exchange(other.m_object, nullptr);
        }
        return *this;
    }

    ~CFOwned() { release(); }

    T get() const { return m_object; }

private:
    void release()
    {
        if (m_object)
            CFRelease(m_object);
        m_object = nullptr;
    }

    T m_object { nullptr };
};

}

// https://developer.apple.com/documentation/appkit/nsfont/weight
// CoreText gives weights from -1 to 1. These are the weights of the AppKit constants from ultraLight to black, which
// stand for the CSS weights from 100 to 900.
static CGFloat core_text_weight(u16 css_weight)
{
    static constexpr Array<CGFloat, 9> weights { -0.8, -0.6, -0.4, 0, 0.23, 0.3, 0.4, 0.56, 0.62 };
    auto position = (clamp<CGFloat>(css_weight, 100, 900) - 100) / 100;
    auto index = min(static_cast<size_t>(position), weights.size() - 2);
    return weights[index] + (weights[index + 1] - weights[index]) * (position - index);
}

// https://learn.microsoft.com/en-us/typography/opentype/spec/os2#uswidthclass
// CoreText gives widths from -1 to 1, with 0 for the normal width. Use the change from the normal width, which the
// width classes give in percent.
static CGFloat core_text_width(u16 width_class)
{
    static constexpr Array<CGFloat, 9> percentages { 50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200 };
    return (percentages[clamp<u16>(width_class, 1, 9) - 1] - 100) / 100;
}

// https://drafts.csswg.org/css-fonts-4/#valdef-font-style-oblique-angle
// CoreText gives slants from -1 to 1, where 1 is 30 degrees. An italic or oblique style asks for the default angle of
// an oblique style, 14 degrees.
static constexpr CGFloat core_text_slant_of_slanted_styles = 14.0 / 30.0;

static CFOwned<CFStringRef> make_string(StringView string)
{
    return CFOwned<CFStringRef> { CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<UInt8 const*>(string.characters_without_null_termination()), static_cast<CFIndex>(string.length()), kCFStringEncodingUTF8, false) };
}

static Optional<String> string_from(CFStringRef string)
{
    if (!string)
        return {};
    auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    Vector<char> buffer;
    buffer.resize(capacity);
    if (!CFStringGetCString(string, buffer.data(), capacity, kCFStringEncodingUTF8))
        return {};
    auto result = String::from_utf8(StringView { buffer.data(), strlen(buffer.data()) });
    if (result.is_error())
        return {};
    return result.release_value();
}

static void set_number(CFMutableDictionaryRef dictionary, CFStringRef key, CGFloat value)
{
    CFOwned<CFNumberRef> number { CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &value) };
    if (number.get())
        CFDictionarySetValue(dictionary, key, number.get());
}

static CFOwned<CTFontDescriptorRef> create_descriptor(CFStringRef family, u16 weight, u16 width, u8 slope)
{
    CFOwned<CFMutableDictionaryRef> traits { CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    CFOwned<CFMutableDictionaryRef> attributes { CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) };
    if (!traits.get() || !attributes.get())
        return {};

    set_number(traits.get(), kCTFontWeightTrait, core_text_weight(weight));
    set_number(traits.get(), kCTFontWidthTrait, core_text_width(width));
    set_number(traits.get(), kCTFontSlantTrait, slope == 0 ? 0 : core_text_slant_of_slanted_styles);
    CFDictionarySetValue(attributes.get(), kCTFontTraitsAttribute, traits.get());
    CFDictionarySetValue(attributes.get(), kCTFontFamilyNameAttribute, family);
    return CFOwned<CTFontDescriptorRef> { CTFontDescriptorCreateWithAttributes(attributes.get()) };
}

// The face of a family that has the style closest to the requested one, if the family is installed.
static CFOwned<CTFontRef> match_font_for_family(CFStringRef family, u16 weight, u16 width, u8 slope)
{
    auto descriptor = create_descriptor(family, weight, width, slope);
    if (!descriptor.get())
        return {};
    CFTypeRef required_attribute_values[] = { kCTFontFamilyNameAttribute };
    CFOwned<CFSetRef> required_attributes { CFSetCreate(kCFAllocatorDefault, required_attribute_values, 1, &kCFTypeSetCallBacks) };
    if (!required_attributes.get())
        return {};
    CFOwned<CTFontDescriptorRef> matched_descriptor { CTFontDescriptorCreateMatchingFontDescriptor(descriptor.get(), required_attributes.get()) };
    if (!matched_descriptor.get())
        return {};
    return CFOwned<CTFontRef> { CTFontCreateWithFontDescriptor(matched_descriptor.get(), 0, nullptr) };
}

static bool has_code_point(CTFontRef font, u32 code_point)
{
    UniChar code_units[2] = {};
    CGGlyph glyphs[2] = {};
    CFIndex length = 1;
    if (code_point < 0x10000) {
        code_units[0] = static_cast<UniChar>(code_point);
    } else {
        code_units[0] = static_cast<UniChar>(0xD800 + ((code_point - 0x10000) >> 10));
        code_units[1] = static_cast<UniChar>(0xDC00 + ((code_point - 0x10000) & 0x3FF));
        length = 2;
    }
    return CTFontGetGlyphsForCharacters(font, code_units, glyphs, length);
}

// The face index in a font file for a font that CoreText gives. This is the face with the same PostScript name, or
// else a face of the same family with a named instance at the variation of the font, or that face by itself if no
// named instance is at the variation. The upper 16 bits of the index select the named instance.
static Optional<u32> face_index_of(ReadonlyBytes bytes, CTFontRef font, String const& postscript_name, CFDictionaryRef variation)
{
    CFOwned<CFStringRef> core_text_family { CTFontCopyFamilyName(font) };
    auto family = string_from(core_text_family.get());

    auto* blob = hb_blob_create(reinterpret_cast<char const*>(bytes.data()), bytes.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    auto face_count = hb_face_count(blob);
    Optional<u32> match;
    Optional<u32> instance_match;
    Optional<u32> family_match;
    for (u32 index = 0; index < face_count && !match.has_value(); ++index) {
        auto* face = hb_face_create(blob, index);
        if (face_name(face, HB_OT_NAME_ID_POSTSCRIPT_NAME) == postscript_name) {
            match = index;
        } else if (variation && CFDictionaryGetCount(variation) > 0 && family.has_value() && !instance_match.has_value()) {
            auto face_family = face_name(face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
            if (!face_family.has_value())
                face_family = face_name(face, HB_OT_NAME_ID_FONT_FAMILY);
            if (face_family == family) {
                if (!family_match.has_value())
                    family_match = index;
                if (auto instance = named_instance_with_variation(face, variation); instance.has_value())
                    instance_match = index | ((*instance + 1) << 16);
            }
        }
        hb_face_destroy(face);
    }
    hb_blob_destroy(blob);
    if (match.has_value())
        return match;
    if (instance_match.has_value())
        return instance_match;
    return family_match;
}

// The face index of a font depends on its file, its PostScript name and its variation.
static ByteString file_face_key(StringView path, String const& postscript_name, CFDictionaryRef variation)
{
    struct Coordinate {
        i64 tag { 0 };
        double value { 0 };
    };
    Vector<Coordinate> coordinates;
    if (variation) {
        CFDictionaryApplyFunction(
            variation, [](void const* key, void const* value, void* context) {
                Coordinate coordinate;
                CFNumberGetValue(static_cast<CFNumberRef>(key), kCFNumberSInt64Type, &coordinate.tag);
                CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &coordinate.value);
                static_cast<Vector<Coordinate>*>(context)->append(coordinate);
            },
            &coordinates);
        quick_sort(coordinates, [](auto const& a, auto const& b) { return a.tag < b.tag; });
    }

    StringBuilder builder;
    builder.append(path);
    builder.append('\0');
    builder.append(postscript_name);
    for (auto const& coordinate : coordinates) {
        builder.append('\0');
        builder.appendff("{}={:x}", coordinate.tag, bit_cast<u64>(coordinate.value));
    }
    return builder.to_byte_string();
}

// The face of a font file that a CoreText font has, if Typeface loads it. A fallback font covers many code points, and
// each code point that a page asks for gives the same font again, so the answer is kept.
static Optional<SystemFontFile> file_face_of(CTFontRef font, StringView path, String const& postscript_name)
{
    static NeverDestroyed<Mutex> mutex;
    static NeverDestroyed<HashMap<ByteString, Optional<SystemFontFile>>> file_faces;

    CFOwned<CFDictionaryRef> variation { CTFontCopyVariation(font) };
    auto key = file_face_key(path, postscript_name, variation.get());
    {
        MutexLocker locker { *mutex };
        if (auto it = file_faces->find(key); it != file_faces->end())
            return it->value;
    }

    Optional<SystemFontFile> file;
    if (auto mapped_file = Core::MappedFile::map(path); !mapped_file.is_error()) {
        if (auto ttc_index = face_index_of(mapped_file.value()->bytes(), font, postscript_name, variation.get()); ttc_index.has_value())
            file = loadable_file(path, *ttc_index);
    }
    MutexLocker locker { *mutex };
    file_faces->set(move(key), file);
    return file;
}

// The file of a font that CoreText gives, if Typeface loads its face, or else the PostScript name to open it by.
static Optional<SystemFontMatch> match_of(CTFontRef font)
{
    CFOwned<CFStringRef> core_text_postscript_name { CTFontCopyPostScriptName(font) };
    auto postscript_name = string_from(core_text_postscript_name.get());
    if (!postscript_name.has_value())
        return {};

    CFOwned<CFURLRef> url { static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute)) };
    char path[MAXPATHLEN];
    if (url.get() && CFURLGetFileSystemRepresentation(url.get(), true, reinterpret_cast<UInt8*>(path), sizeof(path))) {
        if (auto file = file_face_of(font, { path, strlen(path) }, *postscript_name); file.has_value())
            return SystemFontMatch { file.release_value() };
    }

    return SystemFontMatch { PlatformFontName { postscript_name.release_value() } };
}

Optional<SystemFontMatch> match_family_style(StringView family, u16 weight, u16 width, u8 slope)
{
    auto family_name = make_string(family);
    if (!family_name.get())
        return {};
    auto font = match_font_for_family(family_name.get(), weight, width, slope);
    if (!font.get())
        return {};
    return match_of(font.get());
}

Optional<SystemFontMatch> match_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    // A code point that is a surrogate or that is past the last code point gives no string, and no font has it.
    CFOwned<CFStringRef> string { CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<UInt8 const*>(&code_point), sizeof(code_point), kCFStringEncodingUTF32LE, false) };
    if (!string.get())
        return {};

    // The cascade list of the font that the user reads documents in gives the font for the code point.
    CFOwned<CTFontRef> document_font { CTFontCreateUIFontForLanguage(kCTFontUIFontUser, 0, nullptr) };
    if (!document_font.get())
        return {};

    // https://www.unicode.org/reports/tr51/#Emoji_Script
    // The script subtag Zsye asks for emoji presentation, which CoreText gives with a color emoji font.
    CFOwned<CFStringRef> language;
    if (prefer_color_emoji)
        language = make_string("und-Zsye"sv);
    CFOwned<CTFontRef> fallback_font { CTFontCreateForStringWithLanguage(document_font.get(), string.get(), CFRangeMake(0, CFStringGetLength(string.get())), language.get()) };
    if (!fallback_font.get() || !has_code_point(fallback_font.get(), code_point))
        return {};

    // The fallback font has the style of the document font, so look for a face of its family with the requested style
    // that also has the code point. A family whose name starts with '.' is hidden, and no name matches it.
    CFOwned<CFStringRef> fallback_family { CTFontCopyFamilyName(fallback_font.get()) };
    if (fallback_family.get() && CFStringGetLength(fallback_family.get()) > 0 && CFStringGetCharacterAtIndex(fallback_family.get(), 0) != '.') {
        if (auto styled_font = match_font_for_family(fallback_family.get(), weight, width, slope); styled_font.get() && has_code_point(styled_font.get(), code_point))
            fallback_font = move(styled_font);
    }

    return match_of(fallback_font.get());
}

Optional<FlyString> resolve_generic_family(StringView family, u16 weight, u8 slope)
{
    auto family_name = make_string(family);
    if (!family_name.get())
        return {};
    auto font = match_font_for_family(family_name.get(), weight, FontWidth::Normal, slope);
    if (!font.get())
        return {};
    CFOwned<CFStringRef> resolved_family { CTFontCopyFamilyName(font.get()) };
    auto name = string_from(resolved_family.get());
    if (!name.has_value())
        return {};
    return FlyString { name.release_value() };
}

}
