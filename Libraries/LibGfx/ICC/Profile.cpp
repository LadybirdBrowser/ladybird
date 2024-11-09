/*
 * Copyright (c) 2022-2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <LibGfx/CIELAB.h>
#include <LibGfx/CMYKBitmap.h>
#include <LibGfx/ICC/BinaryFormat.h>
#include <LibGfx/ICC/Profile.h>
#include <LibGfx/ICC/Tags.h>
#include <LibGfx/Matrix3x3.h>
#include <math.h>
#include <time.h>

// V2 spec: https://color.org/specification/ICC.1-2001-04.pdf
// V4 spec: https://color.org/specification/ICC.1-2022-05.pdf

namespace Gfx::ICC {

namespace {

ErrorOr<DateTime> parse_date_time_number(DateTimeNumber const& date_time)
{
    return DateTime {
        .year = date_time.year,
        .month = date_time.month,
        .day = date_time.day,
        .hours = date_time.hours,
        .minutes = date_time.minutes,
        .seconds = date_time.seconds,
    };
}

ErrorOr<u32> parse_size(ICCHeader const& header, ReadonlyBytes icc_bytes)
{
    // ICC v4, 7.2.2 Profile size field
    // "The value in the profile size field shall be the exact size obtained by combining the profile header,
    // the tag table, and the tagged element data, including the pad bytes for the last tag."

    // Valid files have enough data for profile header and tag table entry count.
    if (header.profile_size < sizeof(ICCHeader) + sizeof(u32))
        return Error::from_string_literal("ICC::Profile: Profile size too small");

    if (header.profile_size > icc_bytes.size())
        return Error::from_string_literal("ICC::Profile: Profile size larger than input data");

    // ICC v4, 7.1.2:
    // "NOTE 1 This implies that the length is required to be a multiple of four."
    // The ICC v2 spec doesn't have this note. It instead has:
    // ICC v2, 6.2.2 Offset:
    // "All tag data is required to start on a 4-byte boundary"
    // And indeed, there are files in the wild where the last tag has a size that isn't a multiple of four,
    // resulting in an ICC file whose size isn't a multiple of four either.
    if (header.profile_version_major >= 4 && header.profile_size % 4 != 0)
        return Error::from_string_literal("ICC::Profile: Profile size not a multiple of four");

    return header.profile_size;
}

Optional<PreferredCMMType> parse_preferred_cmm_type(ICCHeader const& header)
{
    // ICC v4, 7.2.3 Preferred CMM type field

    // "This field may be used to identify the preferred CMM to be used.
    //  If used, it shall match a CMM type signature registered in the ICC Tag Registry"
    // https://www.color.org/signatures2.xalter currently links to
    // https://www.color.org/registry/signature/TagRegistry-2021-03.pdf, which contains
    // some CMM signatures.
    // This requirement is often honored in practice, but not always. For example,
    // JPEGs exported in Adobe Lightroom contain profiles that set this to 'Lino',
    // which is not present in the "CMM Signatures" table in that PDF.

    // "If no preferred CMM is identified, this field shall be set to zero (00000000h)."
    if (header.preferred_cmm_type == PreferredCMMType { 0 })
        return {};
    return header.preferred_cmm_type;
}

ErrorOr<Version> parse_version(ICCHeader const& header)
{
    // ICC v4, 7.2.4 Profile version field
    if (header.profile_version_zero != 0)
        return Error::from_string_literal("ICC::Profile: Reserved version bytes not zero");
    return Version(header.profile_version_major, header.profile_version_minor_bugfix);
}

ErrorOr<DeviceClass> parse_device_class(ICCHeader const& header)
{
    // ICC v4, 7.2.5 Profile/device class field
    switch (header.profile_device_class) {
    case DeviceClass::InputDevice:
    case DeviceClass::DisplayDevice:
    case DeviceClass::OutputDevice:
    case DeviceClass::DeviceLink:
    case DeviceClass::ColorSpace:
    case DeviceClass::Abstract:
    case DeviceClass::NamedColor:
        return header.profile_device_class;
    }
    return Error::from_string_literal("ICC::Profile: Invalid device class");
}

ErrorOr<ColorSpace> parse_color_space(ColorSpace color_space)
{
    // ICC v4, Table 19 — Data colour space signatures
    switch (color_space) {
    case ColorSpace::nCIEXYZ:
    case ColorSpace::CIELAB:
    case ColorSpace::CIELUV:
    case ColorSpace::YCbCr:
    case ColorSpace::CIEYxy:
    case ColorSpace::RGB:
    case ColorSpace::Gray:
    case ColorSpace::HSV:
    case ColorSpace::HLS:
    case ColorSpace::CMYK:
    case ColorSpace::CMY:
    case ColorSpace::TwoColor:
    case ColorSpace::ThreeColor:
    case ColorSpace::FourColor:
    case ColorSpace::FiveColor:
    case ColorSpace::SixColor:
    case ColorSpace::SevenColor:
    case ColorSpace::EightColor:
    case ColorSpace::NineColor:
    case ColorSpace::TenColor:
    case ColorSpace::ElevenColor:
    case ColorSpace::TwelveColor:
    case ColorSpace::ThirteenColor:
    case ColorSpace::FourteenColor:
    case ColorSpace::FifteenColor:
        return color_space;
    }
    return Error::from_string_literal("ICC::Profile: Invalid color space");
}

ErrorOr<ColorSpace> parse_data_color_space(ICCHeader const& header)
{
    // ICC v4, 7.2.6 Data colour space field
    return parse_color_space(header.data_color_space);
}

ErrorOr<ColorSpace> parse_connection_space(ICCHeader const& header)
{
    // ICC v4, 7.2.7 PCS field
    //         and Annex D
    auto space = TRY(parse_color_space(header.profile_connection_space));

    if (header.profile_device_class != DeviceClass::DeviceLink && (space != ColorSpace::PCSXYZ && space != ColorSpace::PCSLAB))
        return Error::from_string_literal("ICC::Profile: Invalid profile connection space: Non-PCS space on non-DeviceLink profile");

    return space;
}

ErrorOr<DateTime> parse_creation_date_time(ICCHeader const& header)
{
    // ICC v4, 7.2.8 Date and time field
    return parse_date_time_number(header.profile_creation_time);
}

ErrorOr<void> parse_file_signature(ICCHeader const& header)
{
    // ICC v4, 7.2.9 Profile file signature field
    if (header.profile_file_signature != ProfileFileSignature)
        return Error::from_string_literal("ICC::Profile: profile file signature not 'acsp'");
    return {};
}

ErrorOr<Optional<PrimaryPlatform>> parse_primary_platform(ICCHeader const& header)
{
    // ICC v4, 7.2.10 Primary platform field
    // "If there is no primary platform identified, this field shall be set to zero (00000000h)."
    if (header.primary_platform == PrimaryPlatform { 0 })
        return OptionalNone {};

    switch (header.primary_platform) {
    case PrimaryPlatform::Apple:
    case PrimaryPlatform::Microsoft:
    case PrimaryPlatform::SiliconGraphics:
    case PrimaryPlatform::Sun:
        return header.primary_platform;
    }
    return Error::from_string_literal("ICC::Profile: Invalid primary platform");
}

Optional<DeviceManufacturer> parse_device_manufacturer(ICCHeader const& header)
{
    // ICC v4, 7.2.12 Device manufacturer field
    // "This field may be used to identify a device manufacturer.
    //  If used the signature shall match the signature contained in the appropriate section of the ICC signature registry found at www.color.org"
    // Device manufacturers can be looked up at https://www.color.org/signatureRegistry/index.xalter
    // For example: https://www.color.org/signatureRegistry/?entityEntry=APPL-4150504C
    // Some icc files use codes not in that registry. For example. D50_XYZ.icc from https://www.color.org/XYZprofiles.xalter
    // has its device manufacturer set to 'none', but https://www.color.org/signatureRegistry/?entityEntry=none-6E6F6E65 does not exist.

    // "If not used this field shall be set to zero (00000000h)."
    if (header.device_manufacturer == DeviceManufacturer { 0 })
        return {};
    return header.device_manufacturer;
}

Optional<DeviceModel> parse_device_model(ICCHeader const& header)
{
    // ICC v4, 7.2.13 Device model field
    // "This field may be used to identify a device model.
    //  If used the signature shall match the signature contained in the appropriate section of the ICC signature registry found at www.color.org"
    // Device models can be looked up at https://www.color.org/signatureRegistry/deviceRegistry/index.xalter
    // For example: https://www.color.org/signatureRegistry/deviceRegistry/?entityEntry=7FD8-37464438
    // Some icc files use codes not in that registry. For example. D50_XYZ.icc from https://www.color.org/XYZprofiles.xalter
    // has its device model set to 'none', but https://www.color.org/signatureRegistry/deviceRegistry?entityEntry=none-6E6F6E65 does not exist.

    // "If not used this field shall be set to zero (00000000h)."
    if (header.device_model == DeviceModel { 0 })
        return {};
    return header.device_model;
}

ErrorOr<DeviceAttributes> parse_device_attributes(ICCHeader const& header)
{
    // ICC v4, 7.2.14 Device attributes field

    // "4 to 31": "Reserved (set to binary zero)"
    if (header.device_attributes & 0xffff'fff0)
        return Error::from_string_literal("ICC::Profile: Device attributes reserved bits not set to 0");

    return DeviceAttributes { header.device_attributes };
}

ErrorOr<RenderingIntent> parse_rendering_intent(ICCHeader const& header)
{
    // ICC v4, 7.2.15 Rendering intent field
    switch (header.rendering_intent) {
    case RenderingIntent::Perceptual:
    case RenderingIntent::MediaRelativeColorimetric:
    case RenderingIntent::Saturation:
    case RenderingIntent::ICCAbsoluteColorimetric:
        return header.rendering_intent;
    }
    return Error::from_string_literal("ICC::Profile: Invalid rendering intent");
}

ErrorOr<XYZ> parse_pcs_illuminant(ICCHeader const& header)
{
    // ICC v4, 7.2.16 PCS illuminant field
    XYZ xyz = (XYZ)header.pcs_illuminant;

    /// "The value, when rounded to four decimals, shall be X = 0,9642, Y = 1,0 and Z = 0,8249."
    if (round(xyz.X * 10'000) != 9'642 || round(xyz.Y * 10'000) != 10'000 || round(xyz.Z * 10'000) != 8'249)
        return Error::from_string_literal("ICC::Profile: Invalid pcs illuminant");

    return xyz;
}

Optional<Creator> parse_profile_creator(ICCHeader const& header)
{
    // ICC v4, 7.2.17 Profile creator field
    // "This field may be used to identify the creator of the profile.
    //  If used the signature should match the signature contained in the device manufacturer section of the ICC signature registry found at www.color.org."
    // This is not always true in practice.
    // For example, .icc files in /System/ColorSync/Profiles on macOS 12.6 set this to 'appl', which is a CMM signature, not a device signature (that one would be 'APPL').

    // "If not used this field shall be set to zero (00000000h)."
    if (header.profile_creator == Creator { 0 })
        return {};
    return header.profile_creator;
}

template<size_t N>
bool all_bytes_are_zero(u8 const (&bytes)[N])
{
    for (u8 byte : bytes) {
        if (byte != 0)
            return false;
    }
    return true;
}

ErrorOr<Optional<Crypto::Hash::MD5::DigestType>> parse_profile_id(ICCHeader const& header, ReadonlyBytes icc_bytes)
{
    // ICC v4, 7.2.18 Profile ID field
    // "A profile ID field value of zero (00h) shall indicate that a profile ID has not been calculated."
    if (all_bytes_are_zero(header.profile_id))
        return OptionalNone {};

    Crypto::Hash::MD5::DigestType id;
    static_assert(sizeof(id.data) == sizeof(header.profile_id));
    memcpy(id.data, header.profile_id, sizeof(id.data));

    auto computed_id = Profile::compute_id(icc_bytes);
    if (id != computed_id)
        return Error::from_string_literal("ICC::Profile: Invalid profile id");

    return id;
}

ErrorOr<void> parse_reserved(ICCHeader const& header)
{
    // ICC v4, 7.2.19 Reserved field
    // "This field of the profile header is reserved for future ICC definition and shall be set to zero."
    if (!all_bytes_are_zero(header.reserved))
        return Error::from_string_literal("ICC::Profile: Reserved header bytes are not zero");
    return {};
}
}

URL::URL device_manufacturer_url(DeviceManufacturer device_manufacturer)
{
    return URL::URL(ByteString::formatted("https://www.color.org/signatureRegistry/?entityEntry={:c}{:c}{:c}{:c}-{:08X}",
        device_manufacturer.c0(), device_manufacturer.c1(), device_manufacturer.c2(), device_manufacturer.c3(), device_manufacturer.value));
}

URL::URL device_model_url(DeviceModel device_model)
{
    return URL::URL(ByteString::formatted("https://www.color.org/signatureRegistry/deviceRegistry/?entityEntry={:c}{:c}{:c}{:c}-{:08X}",
        device_model.c0(), device_model.c1(), device_model.c2(), device_model.c3(), device_model.value));
}

Flags::Flags() = default;
Flags::Flags(u32 bits)
    : m_bits(bits)
{
}

DeviceAttributes::DeviceAttributes() = default;
DeviceAttributes::DeviceAttributes(u64 bits)
    : m_bits(bits)
{
}

static ErrorOr<void> validate_date_time(DateTime const& date_time)
{
    // Returns if a DateTime is valid per ICC V4, 4.2 dateTimeNumber.
    // In practice, some profiles contain invalid dates, but we should enforce this for data we write at least.

    // "Number of the month (1 to 12)"
    if (date_time.month < 1 || date_time.month > 12)
        return Error::from_string_literal("ICC::Profile: dateTimeNumber month out of bounds");

    // "Number of the day of the month (1 to 31)"
    if (date_time.day < 1 || date_time.day > 31)
        return Error::from_string_literal("ICC::Profile: dateTimeNumber day out of bounds");

    // "Number of hours (0 to 23)"
    if (date_time.hours > 23)
        return Error::from_string_literal("ICC::Profile: dateTimeNumber hours out of bounds");

    // "Number of minutes (0 to 59)"
    if (date_time.minutes > 59)
        return Error::from_string_literal("ICC::Profile: dateTimeNumber minutes out of bounds");

    // "Number of seconds (0 to 59)"
    // ICC profiles apparently can't be created during leap seconds (seconds would be 60 there, but the spec doesn't allow that).
    if (date_time.seconds > 59)
        return Error::from_string_literal("ICC::Profile: dateTimeNumber seconds out of bounds");

    return {};
}

ErrorOr<time_t> DateTime::to_time_t() const
{
    TRY(validate_date_time(*this));

    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hours;
    tm.tm_min = minutes;
    tm.tm_sec = seconds;
    // timegm() doesn't read tm.tm_isdst, tm.tm_wday, and tm.tm_yday, no need to fill them in.

    time_t timestamp = timegm(&tm);
    if (timestamp == -1)
        return Error::from_string_literal("ICC::Profile: dateTimeNumber not representable as timestamp");

    return timestamp;
}

ErrorOr<DateTime> DateTime::from_time_t(time_t timestamp)
{
    struct tm gmt_tm;
    if (gmtime_r(&timestamp, &gmt_tm) == NULL)
        return Error::from_string_literal("ICC::Profile: timestamp not representable as DateTimeNumber");

    // FIXME: Range-check, using something like `TRY(Checked<u16>(x).try_value())`?
    DateTime result {
        .year = static_cast<u16>(gmt_tm.tm_year + 1900),
        .month = static_cast<u16>(gmt_tm.tm_mon + 1),
        .day = static_cast<u16>(gmt_tm.tm_mday),
        .hours = static_cast<u16>(gmt_tm.tm_hour),
        .minutes = static_cast<u16>(gmt_tm.tm_min),
        .seconds = static_cast<u16>(gmt_tm.tm_sec),
    };
    TRY(validate_date_time(result));
    return result;
}

static ErrorOr<ProfileHeader> read_header(ReadonlyBytes bytes)
{
    if (bytes.size() < sizeof(ICCHeader))
        return Error::from_string_literal("ICC::Profile: Not enough data for header");

    ProfileHeader header;
    auto raw_header = *bit_cast<ICCHeader const*>(bytes.data());

    TRY(parse_file_signature(raw_header));
    header.on_disk_size = TRY(parse_size(raw_header, bytes));
    header.preferred_cmm_type = parse_preferred_cmm_type(raw_header);
    header.version = TRY(parse_version(raw_header));
    header.device_class = TRY(parse_device_class(raw_header));
    header.data_color_space = TRY(parse_data_color_space(raw_header));
    header.connection_space = TRY(parse_connection_space(raw_header));
    header.creation_timestamp = TRY(parse_creation_date_time(raw_header));
    header.primary_platform = TRY(parse_primary_platform(raw_header));
    header.flags = Flags { raw_header.profile_flags };
    header.device_manufacturer = parse_device_manufacturer(raw_header);
    header.device_model = parse_device_model(raw_header);
    header.device_attributes = TRY(parse_device_attributes(raw_header));
    header.rendering_intent = TRY(parse_rendering_intent(raw_header));
    header.pcs_illuminant = TRY(parse_pcs_illuminant(raw_header));
    header.creator = parse_profile_creator(raw_header);
    header.id = TRY(parse_profile_id(raw_header, bytes));
    TRY(parse_reserved(raw_header));

    return header;
}

static ErrorOr<NonnullRefPtr<TagData>> read_tag(ReadonlyBytes bytes, u32 offset_to_beginning_of_tag_data_element, u32 size_of_tag_data_element)
{
    // "All tag data elements shall start on a 4-byte boundary (relative to the start of the profile data stream)"
    if (offset_to_beginning_of_tag_data_element % 4 != 0)
        return Error::from_string_literal("ICC::Profile: Tag data not aligned");

    if (static_cast<u64>(offset_to_beginning_of_tag_data_element) + size_of_tag_data_element > bytes.size())
        return Error::from_string_literal("ICC::Profile: Tag data out of bounds");

    auto tag_bytes = bytes.slice(offset_to_beginning_of_tag_data_element, size_of_tag_data_element);

    // ICC v4, 9 Tag definitions
    // ICC v4, 9.1 General
    // "All tags, including private tags, have as their first four bytes a tag signature to identify to profile readers
    //  what kind of data is contained within a tag."
    if (tag_bytes.size() < sizeof(u32))
        return Error::from_string_literal("ICC::Profile: Not enough data for tag type");

    auto type = tag_type(tag_bytes);
    switch (type) {
    case ChromaticityTagData::Type:
        return ChromaticityTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case CicpTagData::Type:
        return CicpTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case CurveTagData::Type:
        return CurveTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case Lut16TagData::Type:
        return Lut16TagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case Lut8TagData::Type:
        return Lut8TagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case LutAToBTagData::Type:
        return LutAToBTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case LutBToATagData::Type:
        return LutBToATagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case MeasurementTagData::Type:
        return MeasurementTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case MultiLocalizedUnicodeTagData::Type:
        return MultiLocalizedUnicodeTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case NamedColor2TagData::Type:
        return NamedColor2TagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case ParametricCurveTagData::Type:
        return ParametricCurveTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case S15Fixed16ArrayTagData::Type:
        return S15Fixed16ArrayTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case SignatureTagData::Type:
        return SignatureTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case TextDescriptionTagData::Type:
        return TextDescriptionTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case TextTagData::Type:
        return TextTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case ViewingConditionsTagData::Type:
        return ViewingConditionsTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    case XYZTagData::Type:
        return XYZTagData::from_bytes(tag_bytes, offset_to_beginning_of_tag_data_element, size_of_tag_data_element);
    default:
        // FIXME: optionally ignore tags of unknown type
        return try_make_ref_counted<UnknownTagData>(offset_to_beginning_of_tag_data_element, size_of_tag_data_element, type);
    }
}

static ErrorOr<OrderedHashMap<TagSignature, NonnullRefPtr<TagData>>> read_tag_table(ReadonlyBytes bytes)
{
    OrderedHashMap<TagSignature, NonnullRefPtr<TagData>> tag_table;

    // ICC v4, 7.3 Tag table
    // ICC v4, 7.3.1 Overview
    // "The tag table acts as a table of contents for the tags and an index into the tag data element in the profiles. It
    //  shall consist of a 4-byte entry that contains a count of the number of tags in the table followed by a series of 12-
    //  byte entries with one entry for each tag. The tag table therefore contains 4+12n bytes where n is the number of
    //  tags contained in the profile. The entries for the tags within the table are not required to be in any particular
    //  order nor are they required to match the sequence of tag data element within the profile.
    //  Each 12-byte tag entry following the tag count shall consist of a 4-byte tag signature, a 4-byte offset to define
    //  the beginning of the tag data element, and a 4-byte entry identifying the length of the tag data element in bytes.
    //  [...]
    //  The tag table shall define a contiguous sequence of unique tag elements, with no gaps between the last byte
    //  of any tag data element referenced from the tag table (inclusive of any necessary additional pad bytes required
    //  to reach a four-byte boundary) and the byte offset of the following tag element, or the end of the file.
    //  Duplicate tag signatures shall not be included in the tag table.
    //  Tag data elements shall not partially overlap, so there shall be no part of any tag data element that falls within
    //  the range defined for another tag in the tag table."

    ReadonlyBytes tag_table_bytes = bytes.slice(sizeof(ICCHeader));

    if (tag_table_bytes.size() < sizeof(u32))
        return Error::from_string_literal("ICC::Profile: Not enough data for tag count");
    auto tag_count = *bit_cast<BigEndian<u32> const*>(tag_table_bytes.data());

    tag_table_bytes = tag_table_bytes.slice(sizeof(u32));
    if (tag_table_bytes.size() < tag_count * sizeof(TagTableEntry))
        return Error::from_string_literal("ICC::Profile: Not enough data for tag table entries");
    auto tag_table_entries = bit_cast<TagTableEntry const*>(tag_table_bytes.data());

    // "The tag table may contain multiple tags signatures that all reference the same tag data element offset, allowing
    //  efficient reuse of tag data elements."
    HashMap<u32, NonnullRefPtr<TagData>> offset_to_tag_data;

    for (u32 i = 0; i < tag_count; ++i) {
        // FIXME: optionally ignore tags with unknown signature

        // Dedupe identical offset/sizes.
        NonnullRefPtr<TagData> tag_data = TRY(offset_to_tag_data.try_ensure(tag_table_entries[i].offset_to_beginning_of_tag_data_element, [&]() {
            return read_tag(bytes, tag_table_entries[i].offset_to_beginning_of_tag_data_element, tag_table_entries[i].size_of_tag_data_element);
        }));

        // "In such cases, both the offset and size of the tag data elements in the tag table shall be the same."
        if (tag_data->size() != tag_table_entries[i].size_of_tag_data_element)
            return Error::from_string_literal("ICC::Profile: two tags have same offset but different sizes");

        // "Duplicate tag signatures shall not be included in the tag table."
        if (TRY(tag_table.try_set(tag_table_entries[i].tag_signature, move(tag_data))) != AK::HashSetResult::InsertedNewEntry)
            return Error::from_string_literal("ICC::Profile: duplicate tag signature");
    }

    return tag_table;
}

static bool is_xCLR(ColorSpace color_space)
{
    switch (color_space) {
    case ColorSpace::TwoColor:
    case ColorSpace::ThreeColor:
    case ColorSpace::FourColor:
    case ColorSpace::FiveColor:
    case ColorSpace::SixColor:
    case ColorSpace::SevenColor:
    case ColorSpace::EightColor:
    case ColorSpace::NineColor:
    case ColorSpace::TenColor:
    case ColorSpace::ElevenColor:
    case ColorSpace::TwelveColor:
    case ColorSpace::ThirteenColor:
    case ColorSpace::FourteenColor:
    case ColorSpace::FifteenColor:
        return true;
    default:
        return false;
    }
}

ErrorOr<void> Profile::check_required_tags()
{
    // ICC v4, 8 Required tags

    // ICC v4, 8.2 Common requirements
    // "With the exception of DeviceLink profiles, all profiles shall contain the following tags:
    //  - profileDescriptionTag (see 9.2.41);
    //  - copyrightTag (see 9.2.21);
    //  - mediaWhitePointTag (see 9.2.34);
    //  - chromaticAdaptationTag, when the measurement data used to calculate the profile was specified for an
    //    adopted white with a chromaticity different from that of the PCS adopted white (see 9.2.15).
    //  NOTE A DeviceLink profile is not required to have either a mediaWhitePointTag or a chromaticAdaptationTag."
    // profileDescriptionTag, copyrightTag are required for DeviceLink too (see ICC v4, 8.6 DeviceLink profile).
    // profileDescriptionTag, copyrightTag, mediaWhitePointTag are required in ICC v2 as well.
    // chromaticAdaptationTag isn't required in v2 profiles as far as I can tell.

    if (!m_tag_table.contains(profileDescriptionTag))
        return Error::from_string_literal("ICC::Profile: required profileDescriptionTag is missing");

    if (!m_tag_table.contains(copyrightTag))
        return Error::from_string_literal("ICC::Profile: required copyrightTag is missing");

    if (device_class() != DeviceClass::DeviceLink) {
        if (!m_tag_table.contains(mediaWhitePointTag))
            return Error::from_string_literal("ICC::Profile: required mediaWhitePointTag is missing");

        // FIXME: Check for chromaticAdaptationTag after figuring out when exactly it needs to be present.
    }

    auto has_tag = [&](auto& tag) { return m_tag_table.contains(tag); };
    auto has_all_tags = [&]<class T>(T tags) { return all_of(tags, has_tag); };

    switch (device_class()) {
    case DeviceClass::InputDevice: {
        // ICC v4, 8.3 Input profiles
        // "8.3.1 General
        //  Input profiles are generally used with devices such as scanners and digital cameras. The types of profiles
        //  available for use as Input profiles are N-component LUT-based, Three-component matrix-based, and
        //  monochrome.
        //  8.3.2 N-component LUT-based Input profiles
        //  In addition to the tags listed in 8.2 an N-component LUT-based Input profile shall contain the following tag:
        //  - AToB0Tag (see 9.2.1).
        //  8.3.3 Three-component matrix-based Input profiles
        //  In addition to the tags listed in 8.2, a three-component matrix-based Input profile shall contain the following tags:
        //  - redMatrixColumnTag (see 9.2.44);
        //  - greenMatrixColumnTag (see 9.2.30);
        //  - blueMatrixColumnTag (see 9.2.4);
        //  - redTRCTag (see 9.2.45);
        //  - greenTRCTag (see 9.2.31);
        //  - blueTRCTag (see 9.2.5).
        //  [...] Only the PCSXYZ encoding can be used with matrix/TRC models.
        //  8.3.4 Monochrome Input profiles
        //  In addition to the tags listed in 8.2, a monochrome Input profile shall contain the following tag:
        //  - grayTRCTag (see 9.2.29)."
        bool has_n_component_lut_based_tags = has_tag(AToB0Tag);
        bool has_three_component_matrix_based_tags = has_all_tags(Array { redMatrixColumnTag, greenMatrixColumnTag, blueMatrixColumnTag, redTRCTag, greenTRCTag, blueTRCTag });
        bool has_monochrome_tags = has_tag(grayTRCTag);
        if (!has_n_component_lut_based_tags && !has_three_component_matrix_based_tags && !has_monochrome_tags)
            return Error::from_string_literal("ICC::Profile: InputDevice required tags are missing");
        if (!has_n_component_lut_based_tags && has_three_component_matrix_based_tags && connection_space() != ColorSpace::PCSXYZ)
            return Error::from_string_literal("ICC::Profile: InputDevice three-component matrix-based profile must use PCSXYZ");
        break;
    }
    case DeviceClass::DisplayDevice: {
        // ICC v4, 8.4 Display profiles
        // "8.4.1 General
        //  This class of profiles represents display devices such as monitors. The types of profiles available for use as
        //  Display profiles are N-component LUT-based, Three-component matrix-based, and monochrome.
        //  8.4.2 N-Component LUT-based Display profiles
        //  In addition to the tags listed in 8.2 an N-component LUT-based Input profile shall contain the following tags:
        //  - AToB0Tag (see 9.2.1);
        //  - BToA0Tag (see 9.2.6).
        //  8.4.3 Three-component matrix-based Display profiles
        //  In addition to the tags listed in 8.2, a three-component matrix-based Display profile shall contain the following
        //  tags:
        //  - redMatrixColumnTag (see 9.2.44);
        //  - greenMatrixColumnTag (see 9.2.30);
        //  - blueMatrixColumnTag (see 9.2.4);
        //  - redTRCTag (see 9.2.45);
        //  - greenTRCTag (see 9.2.31);
        //  - blueTRCTag (see 9.2.5).
        // [...] Only the PCSXYZ encoding can be used with matrix/TRC models.
        //  8.4.4 Monochrome Display profiles
        //  In addition to the tags listed in 8.2 a monochrome Display profile shall contain the following tag:
        //  - grayTRCTag (see 9.2.29)."
        bool has_n_component_lut_based_tags = has_all_tags(Array { AToB0Tag, BToA0Tag });
        bool has_three_component_matrix_based_tags = has_all_tags(Array { redMatrixColumnTag, greenMatrixColumnTag, blueMatrixColumnTag, redTRCTag, greenTRCTag, blueTRCTag });
        bool has_monochrome_tags = has_tag(grayTRCTag);
        if (!has_n_component_lut_based_tags && !has_three_component_matrix_based_tags && !has_monochrome_tags)
            return Error::from_string_literal("ICC::Profile: DisplayDevice required tags are missing");
        if (!has_n_component_lut_based_tags && has_three_component_matrix_based_tags && connection_space() != ColorSpace::PCSXYZ)
            return Error::from_string_literal("ICC::Profile: DisplayDevice three-component matrix-based profile must use PCSXYZ");
        break;
    }
    case DeviceClass::OutputDevice: {
        // ICC v4, 8.5 Output profiles
        // "8.5.1 General
        //  Output profiles are used to support devices such as printers and film recorders. The types of profiles available
        //  for use as Output profiles are N-component LUT-based and Monochrome.
        //  8.5.2 N-component LUT-based Output profiles
        //  In addition to the tags listed in 8.2 an N-component LUT-based Output profile shall contain the following tags:
        //  - AToB0Tag (see 9.2.1);
        //  - AToB1Tag (see 9.2.2);
        //  - AToB2Tag (see 9.2.3);
        //  - BToA0Tag (see 9.2.6);
        //  - BToA1Tag (see 9.2.7);
        //  - BToA2Tag (see 9.2.8);
        //  - gamutTag (see 9.2.28);
        //  - colorantTableTag (see 9.2.18), for the xCLR colour spaces (see 7.2.6)
        //  8.5.3 Monochrome Output profiles
        //  In addition to the tags listed in 8.2 a monochrome Output profile shall contain the following tag:
        //  - grayTRCTag (see 9.2.29)."
        // The colorantTableTag requirement is new in v4.
        Vector<TagSignature, 8> required_n_component_lut_based_tags = { AToB0Tag, AToB1Tag, AToB2Tag, BToA0Tag, BToA1Tag, BToA2Tag, gamutTag };
        if (is_v4() && is_xCLR(connection_space()))
            required_n_component_lut_based_tags.append(colorantTableTag);
        bool has_n_component_lut_based_tags = has_all_tags(required_n_component_lut_based_tags);
        bool has_monochrome_tags = has_tag(grayTRCTag);
        if (!has_n_component_lut_based_tags && !has_monochrome_tags)
            return Error::from_string_literal("ICC::Profile: OutputDevice required tags are missing");
        break;
    }
    case DeviceClass::DeviceLink: {
        // ICC v4, 8.6 DeviceLink profile
        // "A DeviceLink profile shall contain the following tags:
        //  - profileDescriptionTag (see 9.2.41);
        //  - copyrightTag (see 9.2.21);
        //  - profileSequenceDescTag (see 9.2.42);
        //  - AToB0Tag (see 9.2.1);
        //  - colorantTableTag (see 9.2.18) which is required only if the data colour space field is xCLR, where x is
        //    hexadecimal 2 to F (see 7.2.6);
        //  - colorantTableOutTag (see 9.2.19), required only if the PCS field is xCLR, where x is hexadecimal 2 to F
        //    (see 7.2.6)"
        // profileDescriptionTag and copyrightTag are already checked above, in the code for section 8.2.
        Vector<TagSignature, 4> required_tags = { profileSequenceDescTag, AToB0Tag };
        if (is_v4() && is_xCLR(connection_space())) { // This requirement is new in v4.
            required_tags.append(colorantTableTag);
            required_tags.append(colorantTableOutTag);
        }
        if (!has_all_tags(required_tags))
            return Error::from_string_literal("ICC::Profile: DeviceLink required tags are missing");
        // "The data colour space field (see 7.2.6) in the DeviceLink profile will be the same as the data colour space field
        //  of the first profile in the sequence used to construct the device link. The PCS field (see 7.2.7) will be the same
        //  as the data colour space field of the last profile in the sequence."
        // FIXME: Check that if profileSequenceDescType parsing is implemented.
        break;
    }
    case DeviceClass::ColorSpace:
        // ICC v4, 8.7 ColorSpace profile
        // "In addition to the tags listed in 8.2, a ColorSpace profile shall contain the following tags:
        //  - BToA0Tag (see 9.2.6);
        //  - AToB0Tag (see 9.2.1).
        //  [...] ColorSpace profiles may be embedded in images."
        if (!has_all_tags(Array { AToB0Tag, BToA0Tag }))
            return Error::from_string_literal("ICC::Profile: ColorSpace required tags are missing");
        break;
    case DeviceClass::Abstract:
        // ICC v4, 8.8 Abstract profile
        // "In addition to the tags listed in 8.2, an Abstract profile shall contain the following tag:
        //  - AToB0Tag (see 9.2.1).
        //  [...] Abstract profiles cannot be embedded in images."
        if (!has_tag(AToB0Tag))
            return Error::from_string_literal("ICC::Profile: Abstract required AToB0Tag is missing");
        break;
    case DeviceClass::NamedColor:
        // ICC v4, 8.9 NamedColor profile
        // "In addition to the tags listed in 8.2, a NamedColor profile shall contain the following tag:
        //  - namedColor2Tag (see 9.2.35)."
        if (!has_tag(namedColor2Tag))
            return Error::from_string_literal("ICC::Profile: NamedColor required namedColor2Tag is missing");
        break;
    }

    m_cached_has_any_a_to_b_tag = has_tag(AToB0Tag) || has_tag(AToB1Tag) || has_tag(AToB2Tag);
    m_cached_has_a_to_b0_tag = has_tag(AToB0Tag);
    m_cached_has_any_b_to_a_tag = has_tag(BToA0Tag) || has_tag(BToA1Tag) || has_tag(BToA2Tag);
    m_cached_has_b_to_a0_tag = has_tag(BToA0Tag);
    m_cached_has_all_rgb_matrix_tags = has_all_tags(Array { redMatrixColumnTag, greenMatrixColumnTag, blueMatrixColumnTag, redTRCTag, greenTRCTag, blueTRCTag });

    return {};
}

ErrorOr<void> Profile::check_tag_types()
{
    // This uses m_tag_table.get() even for tags that are guaranteed to exist after check_required_tags()
    // so that the two functions can be called in either order.

    // Profile ID of /System/Library/ColorSync/Profiles/ITU-2020.icc on macOS 13.1.
    static constexpr Crypto::Hash::MD5::DigestType apple_itu_2020_id = { 0x57, 0x0b, 0x1b, 0x76, 0xc6, 0xa0, 0x50, 0xaa, 0x9f, 0x6c, 0x53, 0x8d, 0xbe, 0x2d, 0x3e, 0xf0 };

    // Profile ID of the "Display P3" profiles embedded in the images on https://webkit.org/blog-files/color-gamut/comparison.html
    // (The macOS 13.1 /System/Library/ColorSync/Profiles/Display\ P3.icc file no longer has this quirk.)
    static constexpr Crypto::Hash::MD5::DigestType apple_p3_2015_id = { 0xe5, 0xbb, 0x0e, 0x98, 0x67, 0xbd, 0x46, 0xcd, 0x4b, 0xbe, 0x44, 0x6e, 0xbd, 0x1b, 0x75, 0x98 };

    // Profile ID of the "Display P3" profile in object 881 in https://fredrikbk.com/publications/copy-and-patch.pdf
    // (The macOS 13.1 /System/Library/ColorSync/Profiles/Display\ P3.icc file no longer has this quirk.)
    static constexpr Crypto::Hash::MD5::DigestType apple_p3_2017_id = { 0xca, 0x1a, 0x95, 0x82, 0x25, 0x7f, 0x10, 0x4d, 0x38, 0x99, 0x13, 0xd5, 0xd1, 0xea, 0x15, 0x82 };

    auto has_type = [&](auto tag, std::initializer_list<TagTypeSignature> types, std::initializer_list<TagTypeSignature> v4_types) {
        if (auto type = m_tag_table.get(tag); type.has_value()) {
            auto type_matches = [&](auto wanted_type) { return type.value()->type() == wanted_type; };
            return any_of(types, type_matches) || (is_v4() && any_of(v4_types, type_matches));
        }
        return true;
    };

    // ICC v4, 9.2.1 AToB0Tag
    // "Permitted tag types: lut8Type or lut16Type or lutAToBType"
    // ICC v2, 6.4.1 AToB0Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(AToB0Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutAToBTagData::Type }))
        return Error::from_string_literal("ICC::Profile: AToB0Tag has unexpected type");

    // ICC v4, 9.2.2 AToB1Tag
    // "Permitted tag types: lut8Type or lut16Type or lutAToBType"
    // ICC v2, 6.4.2 AToB1Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(AToB1Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutAToBTagData::Type }))
        return Error::from_string_literal("ICC::Profile: AToB1Tag has unexpected type");

    // ICC v4, 9.2.3 AToB2Tag
    // "Permitted tag types: lut8Type or lut16Type or lutAToBType"
    // ICC v2, 6.4.3 AToB2Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(AToB2Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutAToBTagData::Type }))
        return Error::from_string_literal("ICC::Profile: AToB2Tag has unexpected type");

    // ICC v4, 9.2.4 blueMatrixColumnTag
    // "Permitted tag types: XYZType
    //  This tag contains the third column in the matrix used in matrix/TRC transforms."
    // (Called blueColorantTag in the v2 spec, otherwise identical there.)
    if (auto type = m_tag_table.get(blueMatrixColumnTag); type.has_value()) {
        if (type.value()->type() != XYZTagData::Type)
            return Error::from_string_literal("ICC::Profile: blueMatrixColumnTag has unexpected type");
        if (static_cast<XYZTagData const&>(*type.value()).xyzs().size() != 1)
            return Error::from_string_literal("ICC::Profile: blueMatrixColumnTag has unexpected size");
    }

    // ICC v4, 9.2.5 blueTRCTag
    // "Permitted tag types: curveType or parametricCurveType"
    // ICC v2, 6.4.5 blueTRCTag
    // "Tag Type: curveType"
    if (!has_type(blueTRCTag, { CurveTagData::Type }, { ParametricCurveTagData::Type }))
        return Error::from_string_literal("ICC::Profile: blueTRCTag has unexpected type");

    // ICC v4, 9.2.6 BToA0Tag
    // "Permitted tag types: lut8Type or lut16Type or lutBToAType"
    // ICC v2, 6.4.6 BToA0Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(BToA0Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: BToA0Tag has unexpected type");

    // ICC v4, 9.2.7 BToA1Tag
    // "Permitted tag types: lut8Type or lut16Type or lutBToAType"
    // ICC v2, 6.4.7 BToA1Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(BToA1Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: BToA1Tag has unexpected type");

    // ICC v4, 9.2.8 BToA2Tag
    // "Permitted tag types: lut8Type or lut16Type or lutBToAType"
    // ICC v2, 6.4.8 BToA2Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(BToA2Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: BToA2Tag has unexpected type");

    // ICC v4, 9.2.9 BToD0Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.10 BToD1Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.11 BToD2Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.12 BToD3Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.13 calibrationDateTimeTag
    // "Permitted tag types: dateTimeType"
    // FIXME

    // ICC v4, 9.2.14 charTargetTag
    // "Permitted tag types: textType"
    if (!has_type(charTargetTag, { TextTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: charTargetTag has unexpected type");

    // ICC v4, 9.2.15 chromaticAdaptationTag
    // "Permitted tag types: s15Fixed16ArrayType [...]
    //  Such a 3 x 3 chromatic adaptation matrix is organized as a 9-element array"
    if (auto type = m_tag_table.get(chromaticAdaptationTag); type.has_value()) {
        if (type.value()->type() != S15Fixed16ArrayTagData::Type)
            return Error::from_string_literal("ICC::Profile: chromaticAdaptationTag has unexpected type");
        if (static_cast<S15Fixed16ArrayTagData const&>(*type.value()).values().size() != 9)
            return Error::from_string_literal("ICC::Profile: chromaticAdaptationTag has unexpected size");
    }

    // ICC v4, 9.2.16 chromaticityTag
    // "Permitted tag types: chromaticityType"
    if (!has_type(chromaticityTag, { ChromaticityTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: ChromaticityTagData has unexpected type");

    // ICC v4, 9.2.17 cicpTag
    // "Permitted tag types: cicpType"
    if (auto type = m_tag_table.get(cicpTag); type.has_value()) {
        if (type.value()->type() != CicpTagData::Type)
            return Error::from_string_literal("ICC::Profile: cicpTag has unexpected type");

        // "The colour encoding specified by the CICP tag content shall be equivalent to the data colour space encoding
        //  represented by this ICC profile.
        //  NOTE The ICC colour transform cannot match every possible rendering of a CICP colour encoding."
        // FIXME: Figure out what that means and check for it.

        // "This tag may be present when the data colour space in the profile header is RGB, YCbCr, or XYZ, and the
        //  profile class in the profile header is Input or Display. The tag shall not be present for other data colour spaces
        //  or profile classes indicated in the profile header."
        bool is_color_space_allowed = data_color_space() == ColorSpace::RGB || data_color_space() == ColorSpace::YCbCr || data_color_space() == ColorSpace::nCIEXYZ;
        bool is_profile_class_allowed = device_class() == DeviceClass::InputDevice || device_class() == DeviceClass::DisplayDevice;
        bool cicp_is_allowed = is_color_space_allowed && is_profile_class_allowed;
        if (!cicp_is_allowed)
            return Error::from_string_literal("ICC::Profile: cicpTag present but not allowed");
    }

    // ICC v4, 9.2.18 colorantOrderTag
    // "Permitted tag types: colorantOrderType"
    // FIXME

    // ICC v4, 9.2.19 colorantTableTag
    // "Permitted tag types: colorantTableType"
    // FIXME

    // ICC v4, 9.2.20 colorantTableOutTag
    // "Permitted tag types: colorantTableType"
    // FIXME

    // ICC v4, 9.2.21 colorimetricIntentImageStateTag
    // "Permitted tag types: signatureType"
    if (!has_type(colorimetricIntentImageStateTag, { SignatureTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: colorimetricIntentImageStateTag has unexpected type");

    // ICC v4, 9.2.22 copyrightTag
    // "Permitted tag types: multiLocalizedUnicodeType"
    // ICC v2, 6.4.13 copyrightTag
    // "Tag Type: textType"
    if (auto type = m_tag_table.get(copyrightTag); type.has_value()) {
        // The v4 spec requires multiLocalizedUnicodeType for this, but I'm aware of a single file
        // that still uses the v2 'text' type here: /System/Library/ColorSync/Profiles/ITU-2020.icc on macOS 13.1.
        // https://openradar.appspot.com/radar?id=5529765549178880
        bool has_v2_cprt_type_in_v4_file_quirk = id() == apple_itu_2020_id || id() == apple_p3_2015_id || id() == apple_p3_2017_id;
        if (is_v4() && type.value()->type() != MultiLocalizedUnicodeTagData::Type && (!has_v2_cprt_type_in_v4_file_quirk || type.value()->type() != TextTagData::Type))
            return Error::from_string_literal("ICC::Profile: copyrightTag has unexpected v4 type");
        if (is_v2() && type.value()->type() != TextTagData::Type)
            return Error::from_string_literal("ICC::Profile: copyrightTag has unexpected v2 type");
    }

    // ICC v4, 9.2.23 deviceMfgDescTag
    // "Permitted tag types: multiLocalizedUnicodeType"
    // ICC v2, 6.4.15 deviceMfgDescTag
    // "Tag Type: textDescriptionType"
    if (auto type = m_tag_table.get(deviceMfgDescTag); type.has_value()) {
        if (is_v4() && type.value()->type() != MultiLocalizedUnicodeTagData::Type)
            return Error::from_string_literal("ICC::Profile: deviceMfgDescTag has unexpected v4 type");
        if (is_v2() && type.value()->type() != TextDescriptionTagData::Type)
            return Error::from_string_literal("ICC::Profile: deviceMfgDescTag has unexpected v2 type");
    }

    // ICC v4, 9.2.24 deviceModelDescTag
    // "Permitted tag types: multiLocalizedUnicodeType"
    // ICC v2, 6.4.16 deviceModelDescTag
    // "Tag Type: textDescriptionType"
    if (auto type = m_tag_table.get(deviceModelDescTag); type.has_value()) {
        if (is_v4() && type.value()->type() != MultiLocalizedUnicodeTagData::Type)
            return Error::from_string_literal("ICC::Profile: deviceModelDescTag has unexpected v4 type");
        if (is_v2() && type.value()->type() != TextDescriptionTagData::Type)
            return Error::from_string_literal("ICC::Profile: deviceModelDescTag has unexpected v2 type");
    }

    // ICC v4, 9.2.25 DToB0Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.26 DToB1Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.27 DToB2Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.28 DToB3Tag
    // "Permitted tag types: multiProcessElementsType"
    // FIXME

    // ICC v4, 9.2.29 gamutTag
    // "Permitted tag types: lut8Type or lut16Type or lutBToAType"
    // ICC v2, 6.4.18 gamutTag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(gamutTag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: gamutTag has unexpected type");

    // ICC v4, 9.2.30 grayTRCTag
    // "Permitted tag types: curveType or parametricCurveType"
    // ICC v2, 6.4.19 grayTRCTag
    // "Tag Type: curveType"
    if (!has_type(grayTRCTag, { CurveTagData::Type }, { ParametricCurveTagData::Type }))
        return Error::from_string_literal("ICC::Profile: grayTRCTag has unexpected type");

    // ICC v4, 9.2.31 greenMatrixColumnTag
    // "Permitted tag types: XYZType
    //  This tag contains the second column in the matrix, which is used in matrix/TRC transforms."
    // (Called greenColorantTag in the v2 spec, otherwise identical there.)
    if (auto type = m_tag_table.get(greenMatrixColumnTag); type.has_value()) {
        if (type.value()->type() != XYZTagData::Type)
            return Error::from_string_literal("ICC::Profile: greenMatrixColumnTag has unexpected type");
        if (static_cast<XYZTagData const&>(*type.value()).xyzs().size() != 1)
            return Error::from_string_literal("ICC::Profile: greenMatrixColumnTag has unexpected size");
    }

    // ICC v4, 9.2.32 greenTRCTag
    // "Permitted tag types: curveType or parametricCurveType"
    // ICC v2, 6.4.21 greenTRCTag
    // "Tag Type: curveType"
    if (!has_type(greenTRCTag, { CurveTagData::Type }, { ParametricCurveTagData::Type }))
        return Error::from_string_literal("ICC::Profile: greenTRCTag has unexpected type");

    // ICC v4, 9.2.33 luminanceTag
    // "Permitted tag types: XYZType"
    //  This tag contains the absolute luminance of emissive devices in candelas per square metre as described by the
    //  Y channel.
    //  NOTE The X and Z values are set to zero."
    // ICC v2, 6.4.22 luminanceTag
    // "Absolute luminance of emissive devices in candelas per square meter as described by the Y channel. The
    //  X and Z channels are ignored in all cases."
    if (auto type = m_tag_table.get(luminanceTag); type.has_value()) {
        if (type.value()->type() != XYZTagData::Type)
            return Error::from_string_literal("ICC::Profile: luminanceTag has unexpected type");
        auto& xyz_type = static_cast<XYZTagData const&>(*type.value());
        if (xyz_type.xyzs().size() != 1)
            return Error::from_string_literal("ICC::Profile: luminanceTag has unexpected size");
        if (is_v4() && xyz_type.xyzs()[0].X != 0)
            return Error::from_string_literal("ICC::Profile: luminanceTag.x unexpectedly not 0");
        if (is_v4() && xyz_type.xyzs()[0].Z != 0)
            return Error::from_string_literal("ICC::Profile: luminanceTag.z unexpectedly not 0");
    }

    // ICC v4, 9.2.34 measurementTag
    // "Permitted tag types: measurementType"
    if (!has_type(measurementTag, { MeasurementTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: measurementTag has unexpected type");

    // ICC v4, 9.2.35 metadataTag
    // "Permitted tag types: dictType"
    // FIXME

    // ICC v4, 9.2.36 mediaWhitePointTag
    // "Permitted tag types: XYZType
    //  This tag, which is used for generating the ICC-absolute colorimetric intent, specifies the chromatically adapted
    //  nCIEXYZ tristimulus values of the media white point. When the measurement data used to create the profile
    //  were specified relative to an adopted white with a chromaticity different from that of the PCS adopted white, the
    //  media white point nCIEXYZ values shall be adapted to be relative to the PCS adopted white chromaticity using
    //  the chromaticAdaptationTag matrix, before recording in the tag. For capture devices, the media white point is
    //  the encoding maximum white for the capture encoding. For displays, the values specified shall be those of the
    //  PCS illuminant as defined in 7.2.16.
    //  See Clause 6 and Annex A for a more complete description of the use of the media white point."
    // ICC v2, 6.4.25 mediaWhitePointTag
    // "This tag specifies the media white point and is used for generating ICC-absolute colorimetric intent. See
    //  Annex A for a more complete description of its use."
    if (auto type = m_tag_table.get(mediaWhitePointTag); type.has_value()) {
        if (type.value()->type() != XYZTagData::Type)
            return Error::from_string_literal("ICC::Profile: mediaWhitePointTag has unexpected type");
        auto& xyz_type = static_cast<XYZTagData const&>(*type.value());
        if (xyz_type.xyzs().size() != 1)
            return Error::from_string_literal("ICC::Profile: mediaWhitePointTag has unexpected size");

        // V4 requires "For displays, the values specified shall be those of the PCS illuminant".
        // But in practice that's not always true. For example, on macOS 13.1, '/System/Library/ColorSync/Profiles/DCI(P3) RGB.icc'
        // has these values in the header: 0000F6D6 00010000 0000D32D
        // but these values in the tag:    0000F6D5 00010000 0000D32C
        // These are close, but not equal.
        // FIXME: File bug for these, and add id-based quirk instead.
        // if (is_v4() && device_class() == DeviceClass::DisplayDevice && xyz_type.xyzs()[0] != pcs_illuminant())
        // return Error::from_string_literal("ICC::Profile: mediaWhitePointTag for displays should be equal to PCS illuminant");
    }

    // ICC v4, 9.2.37 namedColor2Tag
    // "Permitted tag types: namedColor2Type"
    if (auto type = m_tag_table.get(namedColor2Tag); type.has_value()) {
        if (type.value()->type() != NamedColor2TagData::Type)
            return Error::from_string_literal("ICC::Profile: namedColor2Tag has unexpected type");
        // ICC v4, 10.17 namedColor2Type
        // "The device representation corresponds to the header’s “data colour space” field.
        //  This representation should be consistent with the “number of device coordinates” field in the namedColor2Type.
        //  If this field is 0, device coordinates are not provided."
        if (auto number_of_device_coordinates = static_cast<NamedColor2TagData const&>(*type.value()).number_of_device_coordinates();
            number_of_device_coordinates != 0 && number_of_device_coordinates != number_of_components_in_color_space(data_color_space())) {
            return Error::from_string_literal("ICC::Profile: namedColor2Tag number of device coordinates inconsistent with data color space");
        }
    }

    // ICC v4, 9.2.38 outputResponseTag
    // "Permitted tag types: responseCurveSet16Type"
    // FIXME

    // ICC v4, 9.2.39 perceptualRenderingIntentGamutTag
    // "Permitted tag types: signatureType"
    if (!has_type(perceptualRenderingIntentGamutTag, { SignatureTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: perceptualRenderingIntentGamutTag has unexpected type");

    // ICC v4, 9.2.40 preview0Tag
    // "Permitted tag types: lut8Type or lut16Type or lutAToBType or lutBToAType"
    // ICC v2, 6.4.29 preview0Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(preview0Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type, LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: preview0Tag has unexpected type");

    // ICC v4, 9.2.41 preview1Tag
    // "Permitted tag types: lut8Type or lut16Type or lutBToAType"
    // ICC v2, 6.4.30 preview1Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(preview1Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: preview1Tag has unexpected type");

    // ICC v4, 9.2.42 preview2Tag
    // "Permitted tag types: lut8Type or lut16Type or lutBToAType"
    // ICC v2, 6.4.31 preview2Tag
    // "Tag Type: lut8Type or lut16Type"
    if (!has_type(preview2Tag, { Lut8TagData::Type, Lut16TagData::Type }, { LutBToATagData::Type }))
        return Error::from_string_literal("ICC::Profile: preview2Tag has unexpected type");

    // ICC v4, 9.2.43 profileDescriptionTag
    // "Permitted tag types: multiLocalizedUnicodeType"
    // ICC v2, 6.4.32 profileDescriptionTag
    // "Tag Type: textDescriptionType"
    if (auto type = m_tag_table.get(profileDescriptionTag); type.has_value()) {
        // The v4 spec requires multiLocalizedUnicodeType for this, but I'm aware of a single file
        // that still uses the v2 'desc' type here: /System/Library/ColorSync/Profiles/ITU-2020.icc on macOS 13.1.
        // https://openradar.appspot.com/radar?id=5529765549178880
        bool has_v2_desc_type_in_v4_file_quirk = id() == apple_itu_2020_id || id() == apple_p3_2015_id || id() == apple_p3_2017_id;
        if (is_v4() && type.value()->type() != MultiLocalizedUnicodeTagData::Type && (!has_v2_desc_type_in_v4_file_quirk || type.value()->type() != TextDescriptionTagData::Type))
            return Error::from_string_literal("ICC::Profile: profileDescriptionTag has unexpected v4 type");
        if (is_v2() && type.value()->type() != TextDescriptionTagData::Type)
            return Error::from_string_literal("ICC::Profile: profileDescriptionTag has unexpected v2 type");
    }

    // ICC v4, 9.2.44 profileSequenceDescTag
    // "Permitted tag types: profileSequenceDescType"
    // FIXME

    // ICC v4, 9.2.45 profileSequenceIdentifierTag
    // "Permitted tag types: profileSequenceIdentifierType"
    // FIXME

    // ICC v4, 9.2.46 redMatrixColumnTag
    // "Permitted tag types: XYZType
    //  This tag contains the first column in the matrix, which is used in matrix/TRC transforms."
    // (Called redColorantTag in the v2 spec, otherwise identical there.)
    if (auto type = m_tag_table.get(redMatrixColumnTag); type.has_value()) {
        if (type.value()->type() != XYZTagData::Type)
            return Error::from_string_literal("ICC::Profile: redMatrixColumnTag has unexpected type");
        if (static_cast<XYZTagData const&>(*type.value()).xyzs().size() != 1)
            return Error::from_string_literal("ICC::Profile: redMatrixColumnTag has unexpected size");
    }

    // ICC v4, 9.2.47 redTRCTag
    // "Permitted tag types: curveType or parametricCurveType"
    // ICC v2, 6.4.41 redTRCTag
    // "Tag Type: curveType"
    if (!has_type(redTRCTag, { CurveTagData::Type }, { ParametricCurveTagData::Type }))
        return Error::from_string_literal("ICC::Profile: redTRCTag has unexpected type");

    // ICC v4, 9.2.48 saturationRenderingIntentGamutTag
    // "Permitted tag types: signatureType"
    if (!has_type(saturationRenderingIntentGamutTag, { SignatureTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: saturationRenderingIntentGamutTag has unexpected type");

    // ICC v4, 9.2.49 technologyTag
    // "Permitted tag types: signatureType"
    if (!has_type(technologyTag, { SignatureTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: technologyTag has unexpected type");

    // ICC v4, 9.2.50 viewingCondDescTag
    // "Permitted tag types: multiLocalizedUnicodeType"
    // ICC v2, 6.4.46 viewingCondDescTag
    // "Tag Type: textDescriptionType"
    if (auto type = m_tag_table.get(viewingCondDescTag); type.has_value()) {
        if (is_v4() && type.value()->type() != MultiLocalizedUnicodeTagData::Type)
            return Error::from_string_literal("ICC::Profile: viewingCondDescTag has unexpected v4 type");
        if (is_v2() && type.value()->type() != TextDescriptionTagData::Type)
            return Error::from_string_literal("ICC::Profile: viewingCondDescTag has unexpected v2 type");
    }

    // ICC v4, 9.2.51 viewingConditionsTag
    // "Permitted tag types: viewingConditionsType"
    if (!has_type(viewingConditionsTag, { ViewingConditionsTagData::Type }, {}))
        return Error::from_string_literal("ICC::Profile: viewingConditionsTag has unexpected type");

    // FIXME: Add validation for v2-only tags:
    // - ICC v2, 6.4.14 crdInfoTag
    //   "Tag Type: crdInfoType"
    // - ICC v2, 6.4.17 deviceSettingsTag
    //   "Tag Type: deviceSettingsType"
    // - ICC v2, 6.4.24 mediaBlackPointTag
    //   "Tag Type: XYZType"
    // - ICC v2, 6.4.26 namedColorTag
    //   "Tag Type: namedColorType"
    // - ICC v2, 6.4.34 ps2CRD0Tag
    //   "Tag Type: dataType"
    // - ICC v2, 6.4.35 ps2CRD1Tag
    //   "Tag Type: dataType"
    // - ICC v2, 6.4.36 ps2CRD2Tag
    //   "Tag Type: dataType"
    // - ICC v2, 6.4.37 ps2CRD3Tag
    //   "Tag Type: dataType"
    // - ICC v2, 6.4.38 ps2CSATag
    //   "Tag Type: dataType"
    // - ICC v2, 6.4.39 ps2RenderingIntentTag
    //   "Tag Type: dataType"
    // - ICC v2, 6.4.42 screeningDescTag
    //   "Tag Type: textDescriptionType"
    // - ICC v2, 6.4.43 screeningTag
    //   "Tag Type: screeningType"
    // - ICC v2, 6.4.45 ucrbgTag
    //   "Tag Type: ucrbgType"
    // https://www.color.org/v2profiles.xalter says about these tags:
    // "it is also recommended that optional tags in the v2 specification that have subsequently become
    //  obsolete are not included in future profiles made to the v2 specification."

    return {};
}

ErrorOr<NonnullRefPtr<Profile>> Profile::try_load_from_externally_owned_memory(ReadonlyBytes bytes)
{
    auto header = TRY(read_header(bytes));
    bytes = bytes.trim(header.on_disk_size);
    auto tag_table = TRY(read_tag_table(bytes));

    return create(header, move(tag_table));
}

ErrorOr<NonnullRefPtr<Profile>> Profile::create(ProfileHeader const& header, OrderedHashMap<TagSignature, NonnullRefPtr<TagData>> tag_table)
{
    auto profile = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) Profile(header, move(tag_table))));

    TRY(profile->check_required_tags());
    TRY(profile->check_tag_types());

    return profile;
}

Crypto::Hash::MD5::DigestType Profile::compute_id(ReadonlyBytes bytes)
{
    // ICC v4, 7.2.18 Profile ID field
    // "The Profile ID shall be calculated using the MD5 fingerprinting method as defined in Internet RFC 1321.
    //  The entire profile, whose length is given by the size field in the header, with the
    //  profile flags field (bytes 44 to 47, see 7.2.11),
    //  rendering intent field (bytes 64 to 67, see 7.2.15),
    //  and profile ID field (bytes 84 to 99)
    //  in the profile header temporarily set to zeros (00h),
    //  shall be used to calculate the ID."
    u8 const zero[16] = {};
    Crypto::Hash::MD5 md5;
    md5.update(bytes.slice(0, 44));
    md5.update(ReadonlyBytes { zero, 4 }); // profile flags field
    md5.update(bytes.slice(48, 64 - 48));
    md5.update(ReadonlyBytes { zero, 4 }); // rendering intent field
    md5.update(bytes.slice(68, 84 - 68));
    md5.update(ReadonlyBytes { zero, 16 }); // profile ID field
    md5.update(bytes.slice(100));
    return md5.digest();
}

static TagSignature forward_transform_tag_for_rendering_intent(RenderingIntent rendering_intent)
{
    // ICCv4, Table 25 — Profile type/profile tag and defined rendering intents
    // This function assumes a profile class of InputDevice, DisplayDevice, OutputDevice, or ColorSpace.
    switch (rendering_intent) {
    case RenderingIntent::Perceptual:
        return AToB0Tag;
    case RenderingIntent::MediaRelativeColorimetric:
    case RenderingIntent::ICCAbsoluteColorimetric:
        return AToB1Tag;
    case RenderingIntent::Saturation:
        return AToB2Tag;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<FloatVector3> Profile::to_pcs_a_to_b(TagData const& tag_data, ReadonlyBytes color) const
{
    // Assumes a "normal" device_class() (i.e. not DeviceLink).
    VERIFY(number_of_components_in_color_space(connection_space()) == 3);

    if (m_to_pcs_clut_cache.has_value() && m_to_pcs_clut_cache->key == color)
        return m_to_pcs_clut_cache->value;

    FloatVector3 result;

    switch (tag_data.type()) {
    case Lut16TagData::Type: {
        auto const& a_to_b = static_cast<Lut16TagData const&>(tag_data);
        result = TRY(a_to_b.evaluate(data_color_space(), connection_space(), color));
        break;
    }
    case Lut8TagData::Type: {
        auto const& a_to_b = static_cast<Lut8TagData const&>(tag_data);
        result = TRY(a_to_b.evaluate(data_color_space(), connection_space(), color));
        break;
    }
    case LutAToBTagData::Type: {
        auto const& a_to_b = static_cast<LutAToBTagData const&>(tag_data);
        if (a_to_b.number_of_input_channels() != number_of_components_in_color_space(data_color_space()))
            return Error::from_string_literal("ICC::Profile::to_pcs_a_to_b: mAB input channel count does not match color space size");

        if (a_to_b.number_of_output_channels() != number_of_components_in_color_space(connection_space()))
            return Error::from_string_literal("ICC::Profile::to_pcs_a_to_b: mAB output channel count does not match profile connection space size");

        result = TRY(a_to_b.evaluate(connection_space(), color));
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }

    if (!m_to_pcs_clut_cache.has_value())
        m_to_pcs_clut_cache = OneElementCLUTCache {};
    m_to_pcs_clut_cache->key = Vector<u8, 4>(color);
    m_to_pcs_clut_cache->value = result;

    return result;
}

ErrorOr<FloatVector3> Profile::to_pcs(ReadonlyBytes color) const
{
    if (color.size() != number_of_components_in_color_space(data_color_space()))
        return Error::from_string_literal("ICC::Profile: input color doesn't match color space size");

    auto get_tag = [&](auto tag) { return m_tag_table.get(tag); };

    switch (device_class()) {
    case DeviceClass::InputDevice:
    case DeviceClass::DisplayDevice:
    case DeviceClass::OutputDevice:
    case DeviceClass::ColorSpace: {
        // ICC v4, 8.10 Precedence order of tag usage
        // "There are several methods of colour transformation that can function within a single CMM. If data for more than
        //  one method are included in the same profile, the following selection algorithm shall be used by the software
        //  implementation."
        // ICC v4, 8.10.2 Input, display, output, or colour space profile types
        // "a) Use the BToD0Tag, BToD1Tag, BToD2Tag, BToD3Tag, DToB0Tag, DToB1Tag, DToB2Tag, or
        //     DToB3Tag designated for the rendering intent if the tag is present, except where this tag is not needed or
        //     supported by the CMM (if a particular processing element within the tag is not supported the tag is not
        //     supported)."
        // FIXME: Implement multiProcessElementsType one day.

        // "b) Use the BToA0Tag, BToA1Tag, BToA2Tag, AToB0Tag, AToB1Tag, or AToB2Tag designated for the
        //     rendering intent if present, when the tag in a) is not used."
        if (m_cached_has_any_a_to_b_tag)
            if (auto tag = get_tag(forward_transform_tag_for_rendering_intent(rendering_intent())); tag.has_value())
                return to_pcs_a_to_b(*tag.value(), color);

        // "c) Use the BToA0Tag or AToB0Tag if present, when the tags in a) and b) are not used."
        // AToB0Tag is for the conversion _to_ PCS (BToA0Tag is for conversion _from_ PCS, so not needed in this function).
        if (m_cached_has_a_to_b0_tag)
            if (auto tag = get_tag(AToB0Tag); tag.has_value())
                return to_pcs_a_to_b(*tag.value(), color);

        // "d) Use TRCs (redTRCTag, greenTRCTag, blueTRCTag, or grayTRCTag) and colorants
        //     (redMatrixColumnTag, greenMatrixColumnTag, blueMatrixColumnTag) when tags in a), b), and c) are not
        //     used."
        auto evaluate_curve = [this](TagSignature curve_tag, float f) {
            auto const& trc = *m_tag_table.get(curve_tag).value();
            VERIFY(trc.type() == CurveTagData::Type || trc.type() == ParametricCurveTagData::Type);
            if (trc.type() == CurveTagData::Type)
                return static_cast<CurveTagData const&>(trc).evaluate(f);
            return static_cast<ParametricCurveTagData const&>(trc).evaluate(f);
        };

        if (data_color_space() == ColorSpace::Gray) {
            VERIFY(color.size() == 1); // True because of color.size() check further up.

            // ICC v4, F.2 grayTRCTag
            // "connection = grayTRC[device]"
            float gray = evaluate_curve(grayTRCTag, color[0] / 255.f);
            FloatVector3 white { pcs_illuminant().X, pcs_illuminant().Y, pcs_illuminant().Z };
            return white * gray;
        }

        // FIXME: Per ICC v4, A.1 General, this should also handle HLS, HSV, YCbCr.
        if (data_color_space() == ColorSpace::RGB) {
            if (!m_cached_has_all_rgb_matrix_tags)
                return Error::from_string_literal("ICC::Profile::to_pcs: RGB color space but neither LUT-based nor matrix-based tags present");
            VERIFY(color.size() == 3); // True because of color.size() check further up.

            // ICC v4, F.3 Three-component matrix-based profiles
            // "linear_r = redTRC[device_r]
            //  linear_g = greenTRC[device_g]
            //  linear_b = blueTRC[device_b]
            //  [connection_X] = [redMatrixColumn_X greenMatrixColumn_X blueMatrixColumn_X]   [ linear_r ]
            //  [connection_Y] = [redMatrixColumn_Y greenMatrixColumn_Y blueMatrixColumn_Y] * [ linear_g ]
            //  [connection_Z] = [redMatrixColumn_Z greenMatrixColumn_Z blueMatrixColumn_Z]   [ linear_b ]"
            FloatVector3 linear_rgb {
                evaluate_curve(redTRCTag, color[0] / 255.f),
                evaluate_curve(greenTRCTag, color[1] / 255.f),
                evaluate_curve(blueTRCTag, color[2] / 255.f),
            };

            return rgb_to_xyz_matrix() * linear_rgb;
        }

        return Error::from_string_literal("ICC::Profile::to_pcs: What happened?!");
    }

    case DeviceClass::DeviceLink:
    case DeviceClass::Abstract:
        // ICC v4, 8.10.3 DeviceLink or Abstract profile types
        // FIXME
        return Error::from_string_literal("ICC::Profile::to_pcs: conversion for DeviceLink and Abstract not implemented");

    case DeviceClass::NamedColor:
        return Error::from_string_literal("ICC::Profile::to_pcs: to_pcs with NamedColor profile does not make sense");
    }
    VERIFY_NOT_REACHED();
}

static FloatVector3 lab_from_xyz(FloatVector3 xyz, XYZ white_point)
{
    // 6.3.2.2 Translation between media-relative colorimetric data and ICC-absolute colorimetric data
    // 6.3.2.3 Computation of PCSLAB
    // 6.3.4 Colour space encodings for the PCS
    // A.3 PCS encodings

    auto f = [](float x) {
        if (x > powf(6.f / 29.f, 3))
            return cbrtf(x);
        return x / (3 * powf(6.f / 29.f, 2)) + 4.f / 29.f;
    };

    // "X/Xn is replaced by Xr/Xi (or Xa/Xmw)"

    // 6.3.2.2 Translation between media-relative colorimetric data and ICC-absolute colorimetric data
    // "The translation from ICC-absolute colorimetric data to media-relative colorimetry data is given by Equations
    //      Xr = (Xi/Xmw) * Xa
    //  where
    //      Xr   media-relative colorimetric data (i.e. PCSXYZ);
    //      Xa   ICC-absolute colorimetric data (i.e. nCIEXYZ);
    //      Xmw  nCIEXYZ values of the media white point as specified in the mediaWhitePointTag;
    //      Xi   PCSXYZ values of the PCS white point defined in 6.3.4.3."
    // 6.3.4.3 PCS encodings for white and black
    // "Table 14 — Encodings of PCS white point: X 0,9642 Y 1,0000 Z 0,8249"
    // That's identical to the values in 7.2.16 PCS illuminant field (Bytes 68 to 79).
    // 9.2.36 mediaWhitePointTag
    // "For displays, the values specified shall be those of the PCS illuminant as defined in 7.2.16."
    // ...so for displays, this is all equivalent I think? It's maybe different for OutputDevice profiles?

    float Xn = white_point.X;
    float Yn = white_point.Y;
    float Zn = white_point.Z;

    float x = xyz[0] / Xn;
    float y = xyz[1] / Yn;
    float z = xyz[2] / Zn;

    float L = 116 * f(y) - 16;
    float a = 500 * (f(x) - f(y));
    float b = 200 * (f(y) - f(z));

    return { L, a, b };
}

static FloatVector3 xyz_from_lab(FloatVector3 lab, XYZ white_point)
{
    // Inverse of lab_from_xyz().
    auto L_star = lab[0];
    auto a_star = lab[1];
    auto b_star = lab[2];

    auto L = (L_star + 16) / 116 + a_star / 500; // f(x)
    auto M = (L_star + 16) / 116;                // f(y)
    auto N = (L_star + 16) / 116 - b_star / 200; // f(z)

    // Inverse of f in lab_from_xyz().
    auto g = [](float x) {
        if (x >= 6.0f / 29.0f)
            return powf(x, 3);
        return (x - 4.0f / 29.0f) * (3 * powf(6.f / 29.f, 2));
    };

    return { white_point.X * g(L), white_point.Y * g(M), white_point.Z * g(N) };
}

static TagSignature backward_transform_tag_for_rendering_intent(RenderingIntent rendering_intent)
{
    // ICCv4, Table 25 — Profile type/profile tag and defined rendering intents
    // This function assumes a profile class of InputDevice, DisplayDevice, OutputDevice, or ColorSpace.
    switch (rendering_intent) {
    case RenderingIntent::Perceptual:
        return BToA0Tag;
    case RenderingIntent::MediaRelativeColorimetric:
    case RenderingIntent::ICCAbsoluteColorimetric:
        return BToA1Tag;
    case RenderingIntent::Saturation:
        return BToA2Tag;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> Profile::from_pcs_b_to_a(TagData const& tag_data, FloatVector3 const& pcs, Bytes out_bytes) const
{
    switch (tag_data.type()) {
    case Lut16TagData::Type:
        // FIXME
        return Error::from_string_literal("ICC::Profile::to_pcs: BToA*Tag handling for mft2 tags not yet implemented");
    case Lut8TagData::Type:
        // FIXME
        return Error::from_string_literal("ICC::Profile::to_pcs: BToA*Tag handling for mft1 tags not yet implemented");
    case LutBToATagData::Type: {
        auto const& b_to_a = static_cast<LutBToATagData const&>(tag_data);
        if (b_to_a.number_of_input_channels() != number_of_components_in_color_space(connection_space()))
            return Error::from_string_literal("ICC::Profile::from_pcs_b_to_a: mBA input channel count does not match color space size");

        if (b_to_a.number_of_output_channels() != number_of_components_in_color_space(data_color_space()))
            return Error::from_string_literal("ICC::Profile::from_pcs_b_to_a: mBA output channel count does not match profile connection space size");

        return b_to_a.evaluate(connection_space(), pcs, out_bytes);
    }
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> Profile::from_pcs(Profile const& source_profile, FloatVector3 pcs, Bytes color) const
{
    if (source_profile.connection_space() != connection_space()) {
        if (source_profile.connection_space() == ColorSpace::PCSLAB) {
            VERIFY(connection_space() == ColorSpace::PCSXYZ);
            pcs = xyz_from_lab(pcs, source_profile.pcs_illuminant());
        } else {
            VERIFY(source_profile.connection_space() == ColorSpace::PCSXYZ);
            VERIFY(connection_space() == ColorSpace::PCSLAB);
            pcs = lab_from_xyz(pcs, pcs_illuminant());
        }
    }

    // See `to_pcs()` for spec links.
    // This function is very similar, but uses BToAn instead of AToBn for LUT profiles,
    // and an inverse transform for matrix profiles.
    if (color.size() != number_of_components_in_color_space(data_color_space()))
        return Error::from_string_literal("ICC::Profile: output color doesn't match color space size");

    auto get_tag = [&](auto tag) { return m_tag_table.get(tag); };

    switch (device_class()) {
    case DeviceClass::InputDevice:
    case DeviceClass::DisplayDevice:
    case DeviceClass::OutputDevice:
    case DeviceClass::ColorSpace: {
        // FIXME: Implement multiProcessElementsType one day.

        if (m_cached_has_any_b_to_a_tag)
            if (auto tag = get_tag(backward_transform_tag_for_rendering_intent(rendering_intent())); tag.has_value())
                return from_pcs_b_to_a(*tag.value(), pcs, color);

        if (m_cached_has_b_to_a0_tag)
            if (auto tag = get_tag(BToA0Tag); tag.has_value())
                return from_pcs_b_to_a(*tag.value(), pcs, color);

        if (data_color_space() == ColorSpace::Gray) {
            // FIXME
            return Error::from_string_literal("ICC::Profile::from_pcs: Gray handling not yet implemented");
        }

        // FIXME: Per ICC v4, A.1 General, this should also handle HLS, HSV, YCbCr.
        if (data_color_space() == ColorSpace::RGB) {
            if (!m_cached_has_all_rgb_matrix_tags)
                return Error::from_string_literal("ICC::Profile::from_pcs: RGB color space but neither LUT-based nor matrix-based tags present");
            VERIFY(color.size() == 3); // True because of color.size() check further up.

            // ICC v4, F.3 Three-component matrix-based profiles
            // "The inverse model is given by the following equations:
            //      [linear_r] = [redMatrixColumn_X greenMatrixColumn_X blueMatrixColumn_X]^-1   [ connection_X ]
            //      [linear_g] = [redMatrixColumn_Y greenMatrixColumn_Y blueMatrixColumn_Y]    * [ connection_Y ]
            //      [linear_b] = [redMatrixColumn_Z greenMatrixColumn_Z blueMatrixColumn_Z]      [ connection_Z ]
            //
            //      for linear_r < 0,     device_r = redTRC^-1[0]          (F.8)
            //      for 0 ≤ linear_r ≤ 1, device_r = redTRC^-1[linear_r]   (F.9)
            //      for linear_r > 1,     device_r = redTRC^-1[1]          (F.10)
            //
            //      for linear_g < 0,     device_g = greenTRC^-1[0]        (F.11)
            //      for 0 ≤ linear_g ≤ 1, device_g = greenTRC^-1[linear_g] (F.12)
            //      for linear_g > 1,     device_g = greenTRC^-1[1]        (F.13)
            //
            //      for linear_b < 0,     device_b = blueTRC^-1[0]         (F.14)
            //      for 0 ≤ linear_b ≤ 1, device_b = blueTRC^-1[linear_b]  (F.15)
            //      for linear_b > 1,     device_b = blueTRC^-1[1]         (F.16)
            //
            // where redTRC^-1, greenTRC^-1, and blueTRC^-1 indicate the inverse functions of the redTRC greenTRC and
            // blueTRC functions respectively.
            // If the redTRC, greenTRC, or blueTRC function is not invertible the behaviour of the corresponding redTRC^-1,
            // greenTRC^-1, and blueTRC^-1 function is undefined. If a one-dimensional curve is constant, the curve cannot be
            // inverted."

            // Convert from XYZ to linear rgb.
            // FIXME: Inverting curves on every call to this function is very inefficient.
            FloatVector3 linear_rgb = TRY(xyz_to_rgb_matrix()) * pcs;

            auto evaluate_curve_inverse = [this](TagSignature curve_tag, float f) {
                auto const& trc = *m_tag_table.get(curve_tag).value();
                VERIFY(trc.type() == CurveTagData::Type || trc.type() == ParametricCurveTagData::Type);
                if (trc.type() == CurveTagData::Type)
                    return static_cast<CurveTagData const&>(trc).evaluate_inverse(f);
                return static_cast<ParametricCurveTagData const&>(trc).evaluate_inverse(f);
            };

            // Convert from linear rgb to device rgb.
            // See equations (F.8) - (F.16) above.
            // FIXME: The spec says to do this, but it loses information. Color.js returns unclamped
            //        values instead (...but how do those make it through the TRC?) and has a separate
            //        clipping step. Maybe that's better?
            //        Also, maybe doing actual gamut mapping would look better?
            //        (For LUT profiles, I think the gamut mapping is baked into the BToA* data in the profile (?).
            //        But for matrix profiles, it'd have to be done in code.)
            linear_rgb.clamp(0.f, 1.f);
            float device_r = evaluate_curve_inverse(redTRCTag, linear_rgb[0]);
            float device_g = evaluate_curve_inverse(greenTRCTag, linear_rgb[1]);
            float device_b = evaluate_curve_inverse(blueTRCTag, linear_rgb[2]);

            color[0] = round(255 * device_r);
            color[1] = round(255 * device_g);
            color[2] = round(255 * device_b);
            return {};
        }

        return Error::from_string_literal("ICC::Profile::from_pcs: What happened?!");
    }

    case DeviceClass::DeviceLink:
    case DeviceClass::Abstract:
        // ICC v4, 8.10.3 DeviceLink or Abstract profile types
        // FIXME
        return Error::from_string_literal("ICC::Profile::from_pcs: conversion for DeviceLink and Abstract not implemented");

    case DeviceClass::NamedColor:
        return Error::from_string_literal("ICC::Profile::from_pcs: from_pcs with NamedColor profile does not make sense");
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<CIELAB> Profile::to_lab(ReadonlyBytes color) const
{
    auto pcs = TRY(to_pcs(color));
    if (connection_space() == ColorSpace::PCSLAB)
        return CIELAB { pcs[0], pcs[1], pcs[2] };

    if (connection_space() != ColorSpace::PCSXYZ) {
        VERIFY(device_class() == DeviceClass::DeviceLink);
        return Error::from_string_literal("ICC::Profile::to_lab: conversion for DeviceLink not implemented");
    }

    FloatVector3 lab = lab_from_xyz(pcs, pcs_illuminant());
    return CIELAB { lab[0], lab[1], lab[2] };
}

MatrixMatrixConversion::MatrixMatrixConversion(LutCurveType source_red_TRC,
    LutCurveType source_green_TRC,
    LutCurveType source_blue_TRC,
    FloatMatrix3x3 matrix,
    LutCurveType destination_red_TRC,
    LutCurveType destination_green_TRC,
    LutCurveType destination_blue_TRC)
    : m_source_red_TRC(move(source_red_TRC))
    , m_source_green_TRC(move(source_green_TRC))
    , m_source_blue_TRC(move(source_blue_TRC))
    , m_matrix(matrix)
    , m_destination_red_TRC(move(destination_red_TRC))
    , m_destination_green_TRC(move(destination_green_TRC))
    , m_destination_blue_TRC(move(destination_blue_TRC))
{
    auto check = [](auto const& trc) {
        VERIFY(trc->type() == CurveTagData::Type || trc->type() == ParametricCurveTagData::Type);
    };
    check(m_source_red_TRC);
    check(m_source_green_TRC);
    check(m_source_blue_TRC);
    check(m_destination_red_TRC);
    check(m_destination_green_TRC);
    check(m_destination_blue_TRC);
}

Optional<MatrixMatrixConversion> Profile::matrix_matrix_conversion(Profile const& source_profile) const
{
    auto has_normal_device_class = [](DeviceClass device) {
        return device == DeviceClass::InputDevice
            || device == DeviceClass::DisplayDevice
            || device == DeviceClass::OutputDevice
            || device == DeviceClass::ColorSpace;
    };

    bool is_matrix_matrix_conversion = has_normal_device_class(device_class())
        && has_normal_device_class(source_profile.device_class())
        && connection_space() == ColorSpace::PCSXYZ
        && source_profile.connection_space() == ColorSpace::PCSXYZ
        && data_color_space() == ColorSpace::RGB
        && source_profile.data_color_space() == ColorSpace::RGB
        && !m_cached_has_any_a_to_b_tag
        && !source_profile.m_cached_has_any_a_to_b_tag
        && m_cached_has_all_rgb_matrix_tags
        && source_profile.m_cached_has_all_rgb_matrix_tags
        && rgb_to_xyz_matrix().is_invertible();

    if (!is_matrix_matrix_conversion)
        return OptionalNone {};

    LutCurveType sourceRedTRC = *source_profile.m_tag_table.get(redTRCTag).value();
    LutCurveType sourceGreenTRC = *source_profile.m_tag_table.get(greenTRCTag).value();
    LutCurveType sourceBlueTRC = *source_profile.m_tag_table.get(blueTRCTag).value();

    FloatMatrix3x3 matrix = MUST(xyz_to_rgb_matrix()) * source_profile.rgb_to_xyz_matrix();

    LutCurveType destinationRedTRC = *m_tag_table.get(redTRCTag).value();
    LutCurveType destinationGreenTRC = *m_tag_table.get(greenTRCTag).value();
    LutCurveType destinationBlueTRC = *m_tag_table.get(blueTRCTag).value();

    return MatrixMatrixConversion(sourceRedTRC, sourceGreenTRC, sourceBlueTRC, matrix, destinationRedTRC, destinationGreenTRC, destinationBlueTRC);
}

ErrorOr<void> Profile::convert_image_matrix_matrix(Gfx::Bitmap& bitmap, MatrixMatrixConversion const& map) const
{
    for (auto& pixel : bitmap) {
        FloatVector3 rgb { (float)Color::from_argb(pixel).red(), (float)Color::from_argb(pixel).green(), (float)Color::from_argb(pixel).blue() };
        auto out = map.map(rgb / 255.0f);
        out.set_alpha(Color::from_argb(pixel).alpha());
        pixel = out.value();
    }
    return {};
}

ErrorOr<void> Profile::convert_image(Gfx::Bitmap& bitmap, Profile const& source_profile) const
{
    if (auto map = matrix_matrix_conversion(source_profile); map.has_value())
        return convert_image_matrix_matrix(bitmap, map.value());

    for (auto& pixel : bitmap) {
        u8 rgb[] = { Color::from_argb(pixel).red(), Color::from_argb(pixel).green(), Color::from_argb(pixel).blue() };
        auto pcs = TRY(source_profile.to_pcs(rgb));
        TRY(from_pcs(source_profile, pcs, rgb));
        pixel = Color(rgb[0], rgb[1], rgb[2], Color::from_argb(pixel).alpha()).value();
    }

    return {};
}

ErrorOr<void> Profile::convert_cmyk_image(Bitmap& out, CMYKBitmap const& in, Profile const& source_profile) const
{
    if (out.size() != in.size())
        return Error::from_string_literal("ICC::Profile::convert_cmyk_image: out and in must have the same dimensions");

    // Might fail if `out` has a scale_factor() != 1.
    if (out.data_size() != in.data_size())
        return Error::from_string_literal("ICC::Profile::convert_cmyk_image: out and in must have the same buffer size");

    static_assert(sizeof(ARGB32) == sizeof(CMYK));
    ARGB32* out_data = out.begin();
    CMYK const* in_data = const_cast<CMYKBitmap&>(in).begin();

    for (size_t i = 0; i < in.data_size() / sizeof(CMYK); ++i) {
        u8 cmyk[] = { in_data[i].c, in_data[i].m, in_data[i].y, in_data[i].k };
        auto pcs = TRY(source_profile.to_pcs(cmyk));

        u8 rgb[3];
        TRY(from_pcs(source_profile, pcs, rgb));
        out_data[i] = Color(rgb[0], rgb[1], rgb[2]).value();
    }

    return {};
}

XYZ const& Profile::red_matrix_column() const { return xyz_data(redMatrixColumnTag); }
XYZ const& Profile::green_matrix_column() const { return xyz_data(greenMatrixColumnTag); }
XYZ const& Profile::blue_matrix_column() const { return xyz_data(blueMatrixColumnTag); }

Optional<String> Profile::tag_string_data(TagSignature signature) const
{
    auto maybe_tag_data = tag_data(signature);
    if (!maybe_tag_data.has_value())
        return {};
    auto& tag_data = maybe_tag_data.release_value();
    if (tag_data.type() == Gfx::ICC::MultiLocalizedUnicodeTagData::Type) {
        auto& multi_localized_unicode = static_cast<Gfx::ICC::MultiLocalizedUnicodeTagData const&>(tag_data);
        // Try to find 'en-US', otherwise any 'en' language, otherwise the first record.
        Optional<String> en_string;
        constexpr u16 language_en = ('e' << 8) + 'n';
        constexpr u16 country_us = ('U' << 8) + 'S';
        for (auto const& record : multi_localized_unicode.records()) {
            if (record.iso_639_1_language_code == language_en) {
                if (record.iso_3166_1_country_code == country_us)
                    return record.text;
                en_string = record.text;
            }
        }
        if (en_string.has_value())
            return en_string.value();
        if (!multi_localized_unicode.records().is_empty())
            return multi_localized_unicode.records().first().text;
        return {};
    }
    if (tag_data.type() == Gfx::ICC::TextDescriptionTagData::Type) {
        auto& text_description = static_cast<Gfx::ICC::TextDescriptionTagData const&>(tag_data);
        return text_description.ascii_description();
    }
    if (tag_data.type() == Gfx::ICC::TextTagData::Type) {
        auto& text = static_cast<Gfx::ICC::TextTagData const&>(tag_data);
        return text.text();
    }
    return {};
}

ErrorOr<FloatMatrix3x3> Profile::xyz_to_rgb_matrix() const
{
    if (!m_cached_xyz_to_rgb_matrix.has_value()) {
        FloatMatrix3x3 forward_matrix = rgb_to_xyz_matrix();
        if (!forward_matrix.is_invertible())
            return Error::from_string_literal("ICC::Profile::from_pcs: matrix not invertible");
        m_cached_xyz_to_rgb_matrix = forward_matrix.inverse();
    }
    return m_cached_xyz_to_rgb_matrix.value();
}

FloatMatrix3x3 Profile::rgb_to_xyz_matrix() const
{
    auto const& red_matrix_column = this->red_matrix_column();
    auto const& green_matrix_column = this->green_matrix_column();
    auto const& blue_matrix_column = this->blue_matrix_column();

    return FloatMatrix3x3 {
        red_matrix_column.X, green_matrix_column.X, blue_matrix_column.X,
        red_matrix_column.Y, green_matrix_column.Y, blue_matrix_column.Y,
        red_matrix_column.Z, green_matrix_column.Z, blue_matrix_column.Z
    };
}

}
