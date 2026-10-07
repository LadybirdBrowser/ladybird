/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Font/TypefaceCoreText.h>
#include <LibIPC/Encoder.h>

#include <harfbuzz/hb-coretext.h>

namespace Gfx {

// NB: These are the CoreText string values behind the public AppKit NSFontDescriptorSystemDesign constants.
// Keeping them here avoids pulling Objective-C headers into this C++ file.
static CFStringRef core_text_ui_font_design(SystemUIFontKind kind)
{
    switch (kind) {
    case SystemUIFontKind::System:
        return CFSTR("NSCTFontUIFontDesignDefault");
    case SystemUIFontKind::Serif:
        return CFSTR("NSCTFontUIFontDesignSerif");
    case SystemUIFontKind::Monospace:
        return CFSTR("NSCTFontUIFontDesignMonospaced");
    case SystemUIFontKind::Rounded:
        return CFSTR("NSCTFontUIFontDesignRounded");
    }
    VERIFY_NOT_REACHED();
}

static CTFontDescriptorRef create_system_ui_font_descriptor(SystemUIFontKind kind, u8 slope)
{
    CGFloat core_text_slant = slope == 0 ? 0.0f : 1.0f;
    auto slant_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &core_text_slant);
    if (!slant_number)
        return nullptr;

    CFTypeRef trait_keys[] = { kCTFontSlantTrait, CFSTR("NSCTFontUIFontDesignTrait") };
    CFTypeRef trait_values[] = { slant_number, core_text_ui_font_design(kind) };
    auto traits = CFDictionaryCreate(kCFAllocatorDefault, trait_keys, trait_values, array_size(trait_keys), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(slant_number);
    if (!traits)
        return nullptr;

    CFTypeRef attribute_keys[] = { kCTFontTraitsAttribute };
    CFTypeRef attribute_values[] = { traits };
    auto attributes = CFDictionaryCreate(kCFAllocatorDefault, attribute_keys, attribute_values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(traits);
    if (!attributes)
        return nullptr;

    auto descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    CFRelease(attributes);
    return descriptor;
}

// The font is made at size 0, which CoreText takes as its default size. Every process makes the same font, and a font
// of the typeface applies its own size and variations.
static CTFontRef create_system_ui_font(SystemUIFontKind kind, u8 slope)
{
    auto base_font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0, nullptr);
    if (!base_font)
        return nullptr;

    auto descriptor = create_system_ui_font_descriptor(kind, slope);
    if (!descriptor) {
        CFRelease(base_font);
        return nullptr;
    }

    auto font = CTFontCreateCopyWithAttributes(base_font, 0, nullptr, descriptor);
    if (!font)
        font = CTFontCreateWithFontDescriptor(descriptor, 0, nullptr);
    CFRelease(base_font);
    CFRelease(descriptor);
    return font;
}

RefPtr<TypefaceCoreText> TypefaceCoreText::system_ui(SystemUIFontStyle style)
{
    static NeverDestroyed<Mutex> mutex;
    static NeverDestroyed<HashMap<u64, NonnullRefPtr<TypefaceCoreText>>> typefaces;

    u64 key = (static_cast<u64>(style.kind) << 40) | (static_cast<u64>(style.weight) << 24) | (static_cast<u64>(style.width) << 8) | style.slope;
    MutexLocker locker(*mutex);
    if (auto typeface = typefaces->get(key); typeface.has_value())
        return *typeface;

    auto core_text_font = create_system_ui_font(style.kind, style.slope);
    if (!core_text_font)
        return nullptr;
    ScopeGuard release_core_text_font = [&] { CFRelease(core_text_font); };
    auto graphics_font = CTFontCopyGraphicsFont(core_text_font, nullptr);
    if (!graphics_font)
        return nullptr;
    ScopeGuard release_graphics_font = [&] { CFRelease(graphics_font); };

    auto typeface = adopt_ref(*new TypefaceCoreText(core_text_font, graphics_font, style));
    typefaces->set(key, typeface);
    return typeface;
}

TypefaceCoreText::TypefaceCoreText(CTFontRef core_text_font, CGFontRef graphics_font, SystemUIFontStyle style)
    : m_core_text_font(core_text_font)
    , m_graphics_font(graphics_font)
    , m_style(style)
{
    CFRetain(m_core_text_font);
    CFRetain(m_graphics_font);
}

TypefaceCoreText::~TypefaceCoreText()
{
    CFRelease(m_graphics_font);
    CFRelease(m_core_text_font);
}

hb_face_t* TypefaceCoreText::create_harfbuzz_face() const
{
    return hb_coretext_face_create(m_graphics_font);
}

void TypefaceCoreText::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    if (system_font_identifier().has_value()) {
        Typeface::encode_font_data_for_ipc(encoder);
        return;
    }

    MUST(encoder.encode(FontDataFormat::SystemUIFont));
    MUST(encoder.encode(m_style));
}

Optional<FaceStyle> TypefaceCoreText::fixed_style() const
{
    // One variable face of a system UI font serves every style, so the typeface answers with the style that it was
    // asked for.
    return FaceStyle { .weight = m_style.weight, .width = m_style.width, .slope = m_style.slope };
}

}
