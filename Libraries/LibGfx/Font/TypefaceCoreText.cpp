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
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontTable.h>
#include <LibGfx/Font/TypefaceCoreText.h>
#include <LibIPC/Encoder.h>

#include <harfbuzz/hb-coretext.h>
#include <harfbuzz/hb-ot.h>
#include <math.h>

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

ErrorOr<NonnullRefPtr<TypefaceCoreText>> TypefaceCoreText::try_load_postscript_name(String const& postscript_name)
{
    auto name_bytes = postscript_name.bytes();
    auto name = CFStringCreateWithBytes(kCFAllocatorDefault, name_bytes.data(), static_cast<CFIndex>(name_bytes.size()), kCFStringEncodingUTF8, false);
    if (!name)
        return Error::from_string_literal("Invalid PostScript name");
    ScopeGuard release_name = [&] { CFRelease(name); };

    auto core_text_font = CTFontCreateWithName(name, 0, nullptr);
    if (!core_text_font)
        return Error::from_string_literal("CoreText has no font with this PostScript name");
    ScopeGuard release_core_text_font = [&] { CFRelease(core_text_font); };

    // CoreText gives another font for a name that it does not know.
    auto found_name = CTFontCopyPostScriptName(core_text_font);
    ScopeGuard release_found_name = [&] {
        if (found_name)
            CFRelease(found_name);
    };
    if (!found_name || !CFEqual(found_name, name))
        return Error::from_string_literal("CoreText has no font with this PostScript name");

    auto graphics_font = CTFontCopyGraphicsFont(core_text_font, nullptr);
    if (!graphics_font)
        return Error::from_string_literal("Failed to get graphics font from CoreText font");
    ScopeGuard release_graphics_font = [&] { CFRelease(graphics_font); };

    return adopt_ref(*new TypefaceCoreText(core_text_font, graphics_font, postscript_name));
}

CTFontRef create_core_text_font_with_variations(CTFontRef font, CGFloat size, ReadonlySpan<FontVariationAxis> coordinates)
{
    // A variation attribute can reset the axes that it leaves out. So the copy starts from the variation of the font,
    // and a copy without coordinates gets no variation attribute.
    if (coordinates.is_empty())
        return CTFontCreateCopyWithAttributes(font, size, nullptr, nullptr);

    CFMutableDictionaryRef variation = nullptr;
    if (auto font_variation = CTFontCopyVariation(font)) {
        variation = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, font_variation);
        CFRelease(font_variation);
    } else {
        variation = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    if (!variation)
        return nullptr;
    ScopeGuard release_variation = [&] { CFRelease(variation); };
    for (auto const& coordinate : coordinates) {
        i64 tag = coordinate.tag.to_u32();
        double value = coordinate.value;
        auto tag_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &tag);
        auto value_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
        if (tag_number && value_number)
            CFDictionarySetValue(variation, tag_number, value_number);
        if (tag_number)
            CFRelease(tag_number);
        if (value_number)
            CFRelease(value_number);
    }

    CFTypeRef attribute_keys[] = { kCTFontVariationAttribute };
    CFTypeRef attribute_values[] = { variation };
    auto attributes = CFDictionaryCreate(kCFAllocatorDefault, attribute_keys, attribute_values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attributes)
        return nullptr;
    ScopeGuard release_attributes = [&] { CFRelease(attributes); };
    auto descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    if (!descriptor)
        return nullptr;
    ScopeGuard release_descriptor = [&] { CFRelease(descriptor); };
    return CTFontCreateCopyWithAttributes(font, size, nullptr, descriptor);
}

Optional<unsigned> named_instance_with_variation(hb_face_t* face, CFDictionaryRef variation)
{
    auto axis_count = hb_ot_var_get_axis_count(face);
    auto instance_count = hb_ot_var_get_named_instance_count(face);
    if (axis_count == 0 || instance_count == 0)
        return {};
    Vector<hb_ot_var_axis_info_t> axes;
    axes.resize(axis_count);
    hb_ot_var_get_axis_infos(face, 0, &axis_count, axes.data());

    Vector<double> values;
    values.ensure_capacity(axis_count);
    for (auto const& axis : axes) {
        double value = axis.default_value;
        i64 tag = axis.tag;
        if (auto tag_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &tag)) {
            if (auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(variation, tag_number)))
                CFNumberGetValue(number, kCFNumberDoubleType, &value);
            CFRelease(tag_number);
        }
        values.unchecked_append(value);
    }

    Vector<float> coordinates;
    coordinates.resize(axis_count);
    for (unsigned instance = 0; instance < instance_count; ++instance) {
        unsigned coordinate_count = axis_count;
        if (hb_ot_var_named_instance_get_design_coords(face, instance, &coordinate_count, coordinates.data()) != axis_count)
            continue;
        // CoreText rounds some coordinates that the font gives in 16.16 fixed point.
        bool matches = true;
        for (unsigned index = 0; matches && index < axis_count; ++index)
            matches = fabs(coordinates[index] - values[index]) <= 0.001;
        if (matches)
            return instance;
    }
    return {};
}

// A copy of a CoreText font of a face at the coordinates of a named instance of the face. A face does not have to list
// its default instance, and then the index after the last instance selects it, which needs no copy.
static CTFontRef create_core_text_named_instance(CTFontRef font, hb_face_t* face, unsigned instance_index)
{
    unsigned axis_count = hb_ot_var_get_axis_count(face);
    Vector<hb_ot_var_axis_info_t> axes;
    axes.resize(axis_count);
    hb_ot_var_get_axis_infos(face, 0, &axis_count, axes.data());
    Vector<float> coordinates;
    coordinates.resize(axis_count);
    unsigned coordinate_count = axis_count;
    if (axis_count == 0 || hb_ot_var_named_instance_get_design_coords(face, instance_index, &coordinate_count, coordinates.data()) != axis_count)
        return nullptr;

    Vector<FontVariationAxis> variation;
    variation.ensure_capacity(axis_count);
    for (unsigned index = 0; index < axis_count; ++index)
        variation.unchecked_append({ FourCC::from_u32(axes[index].tag), coordinates[index] });
    return create_core_text_font_with_variations(font, 0, variation);
}

// Font data for CoreText that keeps its backing for as long as CoreText uses the data.
static CFDataRef create_core_text_data(NonnullRefPtr<Typeface::FontDataBacking> backing, ReadonlyBytes bytes)
{
    CFAllocatorContext allocator_context {};
    allocator_context.info = backing.ptr();
    allocator_context.retain = [](void const* info) -> void const* {
        static_cast<Typeface::FontDataBacking const*>(info)->ref();
        return info;
    };
    allocator_context.release = [](void const* info) { static_cast<Typeface::FontDataBacking const*>(info)->unref(); };
    allocator_context.deallocate = [](void*, void*) { };
    auto allocator = CFAllocatorCreate(kCFAllocatorDefault, &allocator_context);
    if (!allocator)
        return nullptr;
    ScopeGuard release_allocator = [&] { CFRelease(allocator); };
    return CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()), allocator);
}

CTFontRef create_core_text_font_from_data(NonnullRefPtr<Typeface::FontDataBacking> backing, ReadonlyBytes bytes, u32 ttc_index)
{
    if (bytes.size() > static_cast<size_t>(NumericLimits<CFIndex>::max()) || bytes.size() > NumericLimits<unsigned>::max())
        return nullptr;
    auto* blob = hb_blob_create(reinterpret_cast<char const*>(bytes.data()), bytes.size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    ScopeGuard destroy_blob = [&] { hb_blob_destroy(blob); };
    auto* face = hb_face_create(blob, ttc_index);
    ScopeGuard destroy_face = [&] { hb_face_destroy(face); };

    auto name = face_name(face, HB_OT_NAME_ID_POSTSCRIPT_NAME);
    if (!name.has_value())
        return nullptr;
    auto name_bytes = name->bytes();
    auto postscript_name = CFStringCreateWithBytes(kCFAllocatorDefault, name_bytes.data(), static_cast<CFIndex>(name_bytes.size()), kCFStringEncodingUTF8, false);
    if (!postscript_name)
        return nullptr;
    ScopeGuard release_name = [&] { CFRelease(postscript_name); };

    auto data = create_core_text_data(move(backing), bytes);
    if (!data)
        return nullptr;
    ScopeGuard release_data = [&] { CFRelease(data); };
    auto descriptors = CTFontManagerCreateFontDescriptorsFromData(data);
    if (!descriptors)
        return nullptr;
    ScopeGuard release_descriptors = [&] { CFRelease(descriptors); };

    // CoreText also lists the named instances of the faces, so the PostScript name of the face finds its descriptor,
    // and the position of a descriptor in the list is not a face index.
    CTFontDescriptorRef descriptor = nullptr;
    for (CFIndex index = 0; index < CFArrayGetCount(descriptors); ++index) {
        auto candidate = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors, index));
        auto candidate_name = CTFontDescriptorCopyAttribute(candidate, kCTFontNameAttribute);
        if (!candidate_name)
            continue;
        ScopeGuard release_candidate_name = [&] { CFRelease(candidate_name); };
        if (CFEqual(candidate_name, postscript_name)) {
            descriptor = candidate;
            break;
        }
    }
    if (!descriptor)
        return nullptr;
    auto font = CTFontCreateWithFontDescriptor(descriptor, 0, nullptr);
    if (!font)
        return nullptr;

    // The upper 16 bits of the index select a named instance, from 1. CoreText gets its coordinates as a variation.
    if (auto instance_index = ttc_index >> 16; instance_index > 0) {
        if (auto instance_font = create_core_text_named_instance(font, face, instance_index - 1)) {
            CFRelease(font);
            font = instance_font;
        }
    }
    return font;
}

bool core_text_accepts_font_data(NonnullRefPtr<Typeface::FontDataBacking> backing, u32 ttc_index)
{
    auto bytes = backing->bytes();
    if (ttc_index != 0) {
        auto font = create_core_text_font_from_data(move(backing), bytes, ttc_index);
        if (!font)
            return false;
        CFRelease(font);
        return true;
    }

    // The compositor makes the CoreText font of the first face from a font descriptor of all of the data.
    if (bytes.size() > static_cast<size_t>(NumericLimits<CFIndex>::max()))
        return false;
    auto data = create_core_text_data(move(backing), bytes);
    if (!data)
        return false;
    ScopeGuard release_data = [&] { CFRelease(data); };
    auto descriptor = CTFontManagerCreateFontDescriptorFromData(data);
    if (!descriptor)
        return false;
    ScopeGuard release_descriptor = [&] { CFRelease(descriptor); };
    auto font = CTFontCreateWithFontDescriptor(descriptor, 0, nullptr);
    if (!font)
        return false;
    CFRelease(font);
    return true;
}

TypefaceCoreText::TypefaceCoreText(CTFontRef core_text_font, CGFontRef graphics_font, Identity identity)
    : m_core_text_font(core_text_font)
    , m_graphics_font(graphics_font)
    , m_identity(move(identity))
{
    CFRetain(m_core_text_font);
    CFRetain(m_graphics_font);
}

TypefaceCoreText::~TypefaceCoreText()
{
    CFRelease(m_graphics_font);
    CFRelease(m_core_text_font);
}

bool TypefaceCoreText::has_outlines_that_only_core_text_draws() const
{
    call_once(m_outline_format_once, [&] {
        m_has_outlines_that_only_core_text_draws = face_has_table(harfbuzz_typeface(), FourCC { "hvgl" });
    });
    return m_has_outlines_that_only_core_text_draws;
}

hb_face_t* TypefaceCoreText::create_harfbuzz_face() const
{
    auto* face = hb_coretext_face_create(m_graphics_font);
    // A font that CoreText opens by the PostScript name of a named instance has the coordinates of that instance. The
    // upper 16 bits of the face index select the instance, so the fonts and the description of the face start from
    // the same coordinates as CoreText.
    if (m_identity.has<String>()) {
        if (auto variation = CTFontCopyVariation(m_core_text_font)) {
            if (auto instance = named_instance_with_variation(face, variation); instance.has_value())
                hb_face_set_index(face, (*instance + 1) << 16);
            CFRelease(variation);
        }
    }
    return face;
}

void TypefaceCoreText::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    if (system_font_identifier().has_value()) {
        Typeface::encode_font_data_for_ipc(encoder);
        return;
    }

    m_identity.visit(
        [&](SystemUIFontStyle const& style) {
            MUST(encoder.encode(FontDataFormat::SystemUIFont));
            MUST(encoder.encode(style));
        },
        [&](String const& postscript_name) {
            MUST(encoder.encode(FontDataFormat::PlatformFontName));
            MUST(encoder.encode(postscript_name));
        });
}

Optional<FaceStyle> TypefaceCoreText::fixed_style() const
{
    // One variable face of a system UI font serves every style, so the typeface answers with the style that it was
    // asked for.
    if (auto const* style = m_identity.get_pointer<SystemUIFontStyle>())
        return FaceStyle { .weight = style->weight, .width = style->width, .slope = style->slope };
    return {};
}

}
