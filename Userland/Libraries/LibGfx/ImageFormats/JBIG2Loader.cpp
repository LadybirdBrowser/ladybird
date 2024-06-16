/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Utf16View.h>
#include <LibGfx/ImageFormats/CCITTDecoder.h>
#include <LibGfx/ImageFormats/JBIG2Loader.h>
#include <LibGfx/ImageFormats/QMArithmeticDecoder.h>
#include <LibTextCodec/Decoder.h>

// Spec: ITU-T_T_88__08_2018.pdf in the zip file here:
// https://www.itu.int/rec/T-REC-T.88-201808-I
// Annex H has a datastream example.

// That spec was published in 2018 and contains all previous amendments. Its history is:
// * 2002: Original spec published, describes decoding only. Has generic regions,
//         symbol regions, text regions, halftone regions, and pattern regions.
// * 2003: Amendment 1 approved. Describes encoding. Not interesting for us.
//   * 2004: (Amendment 1 erratum 1 approved. Not interesting for us.)
// * 2003: Amendment 2 approved. Added support for EXTTEMPLATE.
// * 2011: Amendment 3 approved. Added support for color coding
//         (COLEXTFLAG, CPCOMPLEN, CPDEFCOLS, CPEXCOLS, CPNCOMP, CPNVALS, GBCOLS,
//         GBCOMBOP, GBFGCOLID, SBCOLS, SBCOLSECTSIZE and SBFGCOLID).
// This history might explain why EXTTEMPLATE and colors are very rare in practice.

namespace Gfx {

namespace JBIG2 {

// Annex A, Arithmetic integer decoding procedure
class ArithmeticIntegerDecoder {
public:
    ArithmeticIntegerDecoder(QMArithmeticDecoder&);

    // A.2 Procedure for decoding values (except IAID)
    // Returns OptionalNone for OOB.
    Optional<i32> decode();

    // Returns Error for OOB.
    ErrorOr<i32> decode_non_oob();

private:
    QMArithmeticDecoder& m_decoder;
    u16 PREV { 0 };
    Vector<QMArithmeticDecoder::Context> contexts;
};

ArithmeticIntegerDecoder::ArithmeticIntegerDecoder(QMArithmeticDecoder& decoder)
    : m_decoder(decoder)
{
    contexts.resize(1 << 9);
}

Optional<int> ArithmeticIntegerDecoder::decode()
{
    // A.2 Procedure for decoding values (except IAID)
    // "1) Set:
    //    PREV = 1"
    u16 PREV = 1;

    // "2) Follow the flowchart in Figure A.1. Decode each bit with CX equal to "IAx + PREV" where "IAx" represents the identifier
    //     of the current arithmetic integer decoding procedure, "+" represents concatenation, and the rightmost 9 bits of PREV are used."
    auto decode_bit = [&]() {
        bool D = m_decoder.get_next_bit(contexts[PREV & 0x1FF]);
        // "3) After each bit is decoded:
        //     If PREV < 256 set:
        //         PREV = (PREV << 1) OR D
        //     Otherwise set:
        //         PREV = (((PREV << 1) OR D) AND 511) OR 256
        //     where D represents the value of the just-decoded bit.
        if (PREV < 256)
            PREV = (PREV << 1) | (u16)D;
        else
            PREV = (((PREV << 1) | (u16)D) & 511) | 256;
        return D;
    };

    auto decode_bits = [&](int n) {
        u32 result = 0;
        for (int i = 0; i < n; ++i)
            result = (result << 1) | decode_bit();
        return result;
    };

    // Figure A.1 – Flowchart for the integer arithmetic decoding procedures (except IAID)
    u8 S = decode_bit();
    u32 V;
    if (!decode_bit())
        V = decode_bits(2);
    else if (!decode_bit())
        V = decode_bits(4) + 4;
    else if (!decode_bit())
        V = decode_bits(6) + 20;
    else if (!decode_bit())
        V = decode_bits(8) + 84;
    else if (!decode_bit())
        V = decode_bits(12) + 340;
    else
        V = decode_bits(32) + 4436;

    // "4) The sequence of bits decoded, interpreted according to Table A.1, gives the value that is the result of this invocation
    //     of the integer arithmetic decoding procedure."
    if (S == 1 && V == 0)
        return {};
    return S ? -V : V;
}

ErrorOr<i32> ArithmeticIntegerDecoder::decode_non_oob()
{
    auto result = decode();
    if (!result.has_value())
        return Error::from_string_literal("ArithmeticIntegerDecoder: Unexpected OOB");
    return result.value();
}

class ArithmeticIntegerIDDecoder {
public:
    ArithmeticIntegerIDDecoder(QMArithmeticDecoder&, u32 code_length);

    // A.3 The IAID decoding procedure
    u32 decode();

private:
    QMArithmeticDecoder& m_decoder;
    u32 m_code_length { 0 };
    Vector<QMArithmeticDecoder::Context> contexts;
};

ArithmeticIntegerIDDecoder::ArithmeticIntegerIDDecoder(QMArithmeticDecoder& decoder, u32 code_length)
    : m_decoder(decoder)
    , m_code_length(code_length)
{
    contexts.resize(1 << (code_length + 1));
}

u32 ArithmeticIntegerIDDecoder::decode()
{
    // A.3 The IAID decoding procedure
    u32 prev = 1;
    for (u8 i = 0; i < m_code_length; ++i) {
        bool bit = m_decoder.get_next_bit(contexts[prev]);
        prev = (prev << 1) | bit;
    }
    prev = prev - (1 << m_code_length);
    return prev;
}

}

static u8 number_of_context_bits_for_template(u8 template_)
{
    if (template_ == 0)
        return 16;
    if (template_ == 1)
        return 13;
    VERIFY(template_ == 2 || template_ == 3);
    return 10;
}

// JBIG2 spec, Annex D, D.4.1 ID string
static constexpr u8 id_string[] = { 0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A };

// 7.3 Segment types
enum SegmentType {
    SymbolDictionary = 0,
    IntermediateTextRegion = 4,
    ImmediateTextRegion = 6,
    ImmediateLosslessTextRegion = 7,
    PatternDictionary = 16,
    IntermediateHalftoneRegion = 20,
    ImmediateHalftoneRegion = 22,
    ImmediateLosslessHalftoneRegion = 23,
    IntermediateGenericRegion = 36,
    ImmediateGenericRegion = 38,
    ImmediateLosslessGenericRegion = 39,
    IntermediateGenericRefinementRegion = 40,
    ImmediateGenericRefinementRegion = 42,
    ImmediateLosslessGenericRefinementRegion = 43,
    PageInformation = 48,
    EndOfPage = 49,
    EndOfStripe = 50,
    EndOfFile = 51,
    Profiles = 52,
    Tables = 53,
    ColorPalette = 54,
    Extension = 62,
};

// Annex D
enum class Organization {
    // D.1 Sequential organization
    Sequential,

    // D.2 Random-access organization
    RandomAccess,

    // D.3 Embedded organization
    Embedded,
};

struct SegmentHeader {
    u32 segment_number { 0 };
    SegmentType type { SegmentType::Extension };
    Vector<u32> referred_to_segment_numbers;

    // 7.2.6 Segment page association
    // "The first page must be numbered "1". This field may contain a value of zero; this value indicates that this segment is not associated with any page."
    u32 page_association { 0 };

    Optional<u32> data_length;
};

class BitBuffer {
public:
    static ErrorOr<NonnullOwnPtr<BitBuffer>> create(size_t width, size_t height);
    bool get_bit(size_t x, size_t y) const;
    void set_bit(size_t x, size_t y, bool b);
    void fill(bool b);

    ErrorOr<NonnullOwnPtr<BitBuffer>> subbitmap(Gfx::IntRect const& rect) const;

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> to_gfx_bitmap() const;
    ErrorOr<ByteBuffer> to_byte_buffer() const;

    size_t width() const { return m_width; }
    size_t height() const { return m_height; }

private:
    BitBuffer(ByteBuffer, size_t width, size_t height, size_t pitch);

    ByteBuffer m_bits;
    size_t m_width { 0 };
    size_t m_height { 0 };
    size_t m_pitch { 0 };
};

ErrorOr<NonnullOwnPtr<BitBuffer>> BitBuffer::create(size_t width, size_t height)
{
    size_t pitch = ceil_div(width, static_cast<size_t>(8));
    auto bits = TRY(ByteBuffer::create_uninitialized(pitch * height));
    return adopt_nonnull_own_or_enomem(new (nothrow) BitBuffer(move(bits), width, height, pitch));
}

bool BitBuffer::get_bit(size_t x, size_t y) const
{
    VERIFY(x < m_width);
    VERIFY(y < m_height);
    size_t byte_offset = x / 8;
    size_t bit_offset = x % 8;
    u8 byte = m_bits[y * m_pitch + byte_offset];
    byte = (byte >> (8 - 1 - bit_offset)) & 1;
    return byte != 0;
}

void BitBuffer::set_bit(size_t x, size_t y, bool b)
{
    VERIFY(x < m_width);
    VERIFY(y < m_height);
    size_t byte_offset = x / 8;
    size_t bit_offset = x % 8;
    u8 byte = m_bits[y * m_pitch + byte_offset];
    u8 mask = 1u << (8 - 1 - bit_offset);
    if (b)
        byte |= mask;
    else
        byte &= ~mask;
    m_bits[y * m_pitch + byte_offset] = byte;
}

void BitBuffer::fill(bool b)
{
    u8 fill_byte = b ? 0xff : 0;
    for (auto& byte : m_bits.bytes())
        byte = fill_byte;
}

ErrorOr<NonnullOwnPtr<BitBuffer>> BitBuffer::subbitmap(Gfx::IntRect const& rect) const
{
    VERIFY(rect.x() >= 0);
    VERIFY(rect.width() >= 0);
    VERIFY(static_cast<size_t>(rect.right()) <= width());

    VERIFY(rect.y() >= 0);
    VERIFY(rect.height() >= 0);
    VERIFY(static_cast<size_t>(rect.bottom()) <= height());

    auto subbitmap = TRY(create(rect.width(), rect.height()));
    for (int y = 0; y < rect.height(); ++y)
        for (int x = 0; x < rect.width(); ++x)
            subbitmap->set_bit(x, y, get_bit(rect.x() + x, rect.y() + y));
    return subbitmap;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> BitBuffer::to_gfx_bitmap() const
{
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { m_width, m_height }));
    for (size_t y = 0; y < m_height; ++y) {
        for (size_t x = 0; x < m_width; ++x) {
            auto color = get_bit(x, y) ? Color::Black : Color::White;
            bitmap->set_pixel(x, y, color);
        }
    }
    return bitmap;
}

ErrorOr<ByteBuffer> BitBuffer::to_byte_buffer() const
{
    return ByteBuffer::copy(m_bits);
}

BitBuffer::BitBuffer(ByteBuffer bits, size_t width, size_t height, size_t pitch)
    : m_bits(move(bits))
    , m_width(width)
    , m_height(height)
    , m_pitch(pitch)
{
}

class Symbol : public RefCounted<Symbol> {
public:
    static NonnullRefPtr<Symbol> create(NonnullOwnPtr<BitBuffer> bitmap)
    {
        return adopt_ref(*new Symbol(move(bitmap)));
    }

    BitBuffer const& bitmap() const { return *m_bitmap; }

private:
    Symbol(NonnullOwnPtr<BitBuffer> bitmap)
        : m_bitmap(move(bitmap))
    {
    }

    NonnullOwnPtr<BitBuffer> m_bitmap;
};

struct SegmentData {
    SegmentHeader header;
    ReadonlyBytes data;

    // Set on dictionary segments after they've been decoded.
    Optional<Vector<NonnullRefPtr<Symbol>>> symbols;

    // Set on pattern segments after they've been decoded.
    Optional<Vector<NonnullRefPtr<Symbol>>> patterns;
};

// 7.4.8.5 Page segment flags
enum class CombinationOperator {
    Or = 0,
    And = 1,
    Xor = 2,
    XNor = 3,
    Replace = 4,
};

static void composite_bitbuffer(BitBuffer& out, BitBuffer const& bitmap, Gfx::IntPoint position, CombinationOperator operator_)
{
    if (!IntRect { position, { bitmap.width(), bitmap.height() } }.intersects(IntRect { { 0, 0 }, { out.width(), out.height() } }))
        return;

    size_t start_x = 0, end_x = bitmap.width();
    size_t start_y = 0, end_y = bitmap.height();
    if (position.x() < 0) {
        start_x = -position.x();
        position.set_x(0);
    }
    if (position.y() < 0) {
        start_y = -position.y();
        position.set_y(0);
    }
    if (position.x() + bitmap.width() > out.width())
        end_x = out.width() - position.x();
    if (position.y() + bitmap.height() > out.height())
        end_y = out.height() - position.y();

    for (size_t y = start_y; y < end_y; ++y) {
        for (size_t x = start_x; x < end_x; ++x) {
            bool bit = bitmap.get_bit(x, y);
            switch (operator_) {
            case CombinationOperator::Or:
                bit = bit || out.get_bit(position.x() + x, position.y() + y);
                break;
            case CombinationOperator::And:
                bit = bit && out.get_bit(position.x() + x, position.y() + y);
                break;
            case CombinationOperator::Xor:
                bit = bit ^ out.get_bit(position.x() + x, position.y() + y);
                break;
            case CombinationOperator::XNor:
                bit = !(bit ^ out.get_bit(position.x() + x, position.y() + y));
                break;
            case CombinationOperator::Replace:
                // Nothing to do.
                break;
            }
            out.set_bit(position.x() + x, position.y() + y, bit);
        }
    }
}

struct Page {
    IntSize size;

    // This is never CombinationOperator::Replace for Pages.
    CombinationOperator default_combination_operator { CombinationOperator::Or };

    OwnPtr<BitBuffer> bits;
};

struct JBIG2LoadingContext {
    enum class State {
        NotDecoded = 0,
        Error,
        Decoded,
    };
    State state { State::NotDecoded };

    Organization organization { Organization::Sequential };
    Page page;

    Optional<u32> number_of_pages;

    Vector<SegmentData> segments;
    HashMap<u32, u32> segments_by_number;
};

static ErrorOr<void> decode_jbig2_header(JBIG2LoadingContext& context, ReadonlyBytes data)
{
    if (!JBIG2ImageDecoderPlugin::sniff(data))
        return Error::from_string_literal("JBIG2LoadingContext: Invalid JBIG2 header");

    FixedMemoryStream stream(data.slice(sizeof(id_string)));

    // D.4.2 File header flags
    u8 header_flags = TRY(stream.read_value<u8>());
    if (header_flags & 0b11110000)
        return Error::from_string_literal("JBIG2LoadingContext: Invalid header flags");
    context.organization = (header_flags & 1) ? Organization::Sequential : Organization::RandomAccess;
    dbgln_if(JBIG2_DEBUG, "JBIG2LoadingContext: Organization: {} ({})", (int)context.organization, context.organization == Organization::Sequential ? "Sequential" : "Random-access");
    bool has_known_number_of_pages = (header_flags & 2) ? false : true;
    bool uses_templates_with_12_AT_pixels = (header_flags & 4) ? true : false;
    bool contains_colored_region_segments = (header_flags & 8) ? true : false;

    // FIXME: Do something with these?
    (void)uses_templates_with_12_AT_pixels;
    (void)contains_colored_region_segments;

    // D.4.3 Number of pages
    if (has_known_number_of_pages) {
        context.number_of_pages = TRY(stream.read_value<BigEndian<u32>>());
        dbgln_if(JBIG2_DEBUG, "JBIG2LoadingContext: Number of pages: {}", context.number_of_pages.value());
    }

    return {};
}

static ErrorOr<SegmentHeader> decode_segment_header(SeekableStream& stream)
{
    // 7.2.2 Segment number
    u32 segment_number = TRY(stream.read_value<BigEndian<u32>>());
    dbgln_if(JBIG2_DEBUG, "Segment number: {}", segment_number);

    // 7.2.3 Segment header flags
    u8 flags = TRY(stream.read_value<u8>());
    SegmentType type = static_cast<SegmentType>(flags & 0b11'1111);
    dbgln_if(JBIG2_DEBUG, "Segment type: {}", (int)type);
    bool segment_page_association_size_is_32_bits = (flags & 0b100'0000) != 0;
    bool segment_retained_only_by_itself_and_extension_segments = (flags & 0b1000'00000) != 0;

    // FIXME: Do something with this?
    (void)segment_retained_only_by_itself_and_extension_segments;

    // 7.2.4 Referred-to segment count and retention flags
    u8 referred_to_segment_count_and_retention_flags = TRY(stream.read_value<u8>());
    u32 count_of_referred_to_segments = referred_to_segment_count_and_retention_flags >> 5;
    if (count_of_referred_to_segments == 5 || count_of_referred_to_segments == 6)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid count_of_referred_to_segments");
    u32 extra_count = 0;
    if (count_of_referred_to_segments == 7) {
        TRY(stream.seek(-1, SeekMode::FromCurrentPosition));
        count_of_referred_to_segments = TRY(stream.read_value<BigEndian<u32>>()) & 0x1FFF'FFFF;
        extra_count = ceil_div(count_of_referred_to_segments + 1, 8);
        TRY(stream.seek(extra_count, SeekMode::FromCurrentPosition));
    }
    dbgln_if(JBIG2_DEBUG, "Referred-to segment count: {}", count_of_referred_to_segments);

    // 7.2.5 Referred-to segment numbers
    Vector<u32> referred_to_segment_numbers;
    for (u32 i = 0; i < count_of_referred_to_segments; ++i) {
        u32 referred_to_segment_number;
        if (segment_number <= 256)
            referred_to_segment_number = TRY(stream.read_value<u8>());
        else if (segment_number <= 65536)
            referred_to_segment_number = TRY(stream.read_value<BigEndian<u16>>());
        else
            referred_to_segment_number = TRY(stream.read_value<BigEndian<u32>>());
        referred_to_segment_numbers.append(referred_to_segment_number);
        dbgln_if(JBIG2_DEBUG, "Referred-to segment number: {}", referred_to_segment_number);
    }

    // 7.2.6 Segment page association
    u32 segment_page_association;
    if (segment_page_association_size_is_32_bits) {
        segment_page_association = TRY(stream.read_value<BigEndian<u32>>());
    } else {
        segment_page_association = TRY(stream.read_value<u8>());
    }
    dbgln_if(JBIG2_DEBUG, "Segment page association: {}", segment_page_association);

    // 7.2.7 Segment data length
    u32 data_length = TRY(stream.read_value<BigEndian<u32>>());
    dbgln_if(JBIG2_DEBUG, "Segment data length: {}", data_length);

    // FIXME: Add some validity checks:
    // - check type is valid
    // - check referred_to_segment_numbers are smaller than segment_number
    // - 7.3.1 Rules for segment references
    // - 7.3.2 Rules for page associations

    Optional<u32> opt_data_length;
    if (data_length != 0xffff'ffff)
        opt_data_length = data_length;
    else if (type != ImmediateGenericRegion)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Unknown data length only allowed for ImmediateGenericRegion");

    return SegmentHeader { segment_number, type, move(referred_to_segment_numbers), segment_page_association, opt_data_length };
}

static ErrorOr<size_t> scan_for_immediate_generic_region_size(ReadonlyBytes data)
{
    // 7.2.7 Segment data length
    // "If the segment's type is "Immediate generic region", then the length field may contain the value 0xFFFFFFFF.
    //  This value is intended to mean that the length of the segment's data part is unknown at the time that the segment header is written (...).
    //  In this case, the true length of the segment's data part shall be determined through examination of the data:
    //  if the segment uses template-based arithmetic coding, then the segment's data part ends with the two-byte sequence 0xFF 0xAC followed by a four-byte row count.
    //  If the segment uses MMR coding, then the segment's data part ends with the two-byte sequence 0x00 0x00 followed by a four-byte row count.
    //  The form of encoding used by the segment may be determined by examining the eighteenth byte of its segment data part,
    //  and the end sequences can occur anywhere after that eighteenth byte."
    // 7.4.6.4 Decoding a generic region segment
    // "NOTE – The sequence 0x00 0x00 cannot occur within MMR-encoded data; the sequence 0xFF 0xAC can occur only at the end of arithmetically-coded data.
    //  Thus, those sequences cannot occur by chance in the data that is decoded to generate the contents of the generic region."
    dbgln_if(JBIG2_DEBUG, "(Unknown data length, computing it)");

    if (data.size() < 19 + sizeof(u32))
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Data too short to contain segment data header and end sequence");

    // Per 7.4.6.1 Generic region segment data header, this starts with the 17 bytes described in
    // 7.4.1 Region segment information field, followed the byte described in 7.4.6.2 Generic region segment flags.
    // That byte's lowest bit stores if the segment uses MMR.
    u8 flags = data[17];
    bool uses_mmr = (flags & 1) != 0;
    auto end_sequence = uses_mmr ? to_array<u8>({ 0x00, 0x00 }) : to_array<u8>({ 0xFF, 0xAC });
    u8 const* end = static_cast<u8 const*>(memmem(data.data() + 19, data.size() - 19 - sizeof(u32), end_sequence.data(), end_sequence.size()));
    if (!end)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Could not find end sequence in segment data");

    size_t size = end - data.data() + end_sequence.size() + sizeof(u32);
    dbgln_if(JBIG2_DEBUG, "(Computed size is {})", size);
    return size;
}

static ErrorOr<void> decode_segment_headers(JBIG2LoadingContext& context, ReadonlyBytes data)
{
    FixedMemoryStream stream(data);

    Vector<ReadonlyBytes> segment_datas;
    auto store_and_skip_segment_data = [&](SegmentHeader const& segment_header) -> ErrorOr<void> {
        size_t start_offset = TRY(stream.tell());
        u32 data_length = TRY(segment_header.data_length.try_value_or_lazy_evaluated([&]() {
            return scan_for_immediate_generic_region_size(data.slice(start_offset));
        }));

        if (start_offset + data_length > data.size()) {
            dbgln_if(JBIG2_DEBUG, "JBIG2ImageDecoderPlugin: start_offset={}, data_length={}, data.size()={}", start_offset, data_length, data.size());
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Segment data length exceeds file size");
        }
        ReadonlyBytes segment_data = data.slice(start_offset, data_length);
        segment_datas.append(segment_data);

        TRY(stream.seek(data_length, SeekMode::FromCurrentPosition));
        return {};
    };

    Vector<SegmentHeader> segment_headers;
    while (!stream.is_eof()) {
        auto segment_header = TRY(decode_segment_header(stream));
        segment_headers.append(segment_header);

        if (context.organization != Organization::RandomAccess)
            TRY(store_and_skip_segment_data(segment_header));

        // Required per spec for files with RandomAccess organization.
        if (segment_header.type == SegmentType::EndOfFile)
            break;
    }

    if (context.organization == Organization::RandomAccess) {
        for (auto const& segment_header : segment_headers)
            TRY(store_and_skip_segment_data(segment_header));
    }

    if (segment_headers.size() != segment_datas.size())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Segment headers and segment datas have different sizes");
    for (size_t i = 0; i < segment_headers.size(); ++i) {
        context.segments.append({ segment_headers[i], segment_datas[i], {}, {} });
        context.segments_by_number.set(segment_headers[i].segment_number, context.segments.size() - 1);
    }

    return {};
}

// 7.4.1 Region segment information field
struct [[gnu::packed]] RegionSegmentInformationField {
    BigEndian<u32> width;
    BigEndian<u32> height;
    BigEndian<u32> x_location;
    BigEndian<u32> y_location;
    u8 flags;

    CombinationOperator external_combination_operator() const
    {
        VERIFY((flags & 0x7) <= 4);
        return static_cast<CombinationOperator>(flags & 0x7);
    }

    bool is_color_bitmap() const
    {
        return (flags & 0x8) != 0;
    }
};
static_assert(AssertSize<RegionSegmentInformationField, 17>());

static ErrorOr<RegionSegmentInformationField> decode_region_segment_information_field(ReadonlyBytes data)
{
    // 7.4.8 Page information segment syntax
    if (data.size() < sizeof(RegionSegmentInformationField))
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid region segment information field size");
    auto result = *(RegionSegmentInformationField const*)data.data();
    if ((result.flags & 0b1111'0000) != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid region segment information field flags");
    if ((result.flags & 0x7) > 4)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid region segment information field operator");

    // NOTE 3 – If the colour extension flag (COLEXTFLAG) is equal to 1, the external combination operator must be REPLACE.
    if (result.is_color_bitmap() && result.external_combination_operator() != CombinationOperator::Replace)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid colored region segment information field operator");

    return result;
}

// 7.4.8 Page information segment syntax
struct [[gnu::packed]] PageInformationSegment {
    BigEndian<u32> bitmap_width;
    BigEndian<u32> bitmap_height;
    BigEndian<u32> page_x_resolution; // In pixels/meter.
    BigEndian<u32> page_y_resolution; // In pixels/meter.
    u8 flags;
    BigEndian<u16> striping_information;
};
static_assert(AssertSize<PageInformationSegment, 19>());

static ErrorOr<PageInformationSegment> decode_page_information_segment(ReadonlyBytes data)
{
    // 7.4.8 Page information segment syntax
    if (data.size() != sizeof(PageInformationSegment))
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid page information segment size");
    return *(PageInformationSegment const*)data.data();
}

static ErrorOr<void> scan_for_page_size(JBIG2LoadingContext& context)
{
    // We only decode the first page at the moment.
    bool found_size = false;
    for (auto const& segment : context.segments) {
        if (segment.header.type != SegmentType::PageInformation || segment.header.page_association != 1)
            continue;
        auto page_information = TRY(decode_page_information_segment(segment.data));

        // FIXME: We're supposed to compute this from the striping information if it's not set.
        if (page_information.bitmap_height == 0xffff'ffff)
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot handle unknown page height yet");

        context.page.size = { page_information.bitmap_width, page_information.bitmap_height };
        found_size = true;
    }
    if (!found_size)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: No page information segment found for page 1");
    return {};
}

static ErrorOr<void> warn_about_multiple_pages(JBIG2LoadingContext& context)
{
    HashTable<u32> seen_pages;
    Vector<u32> pages;

    for (auto const& segment : context.segments) {
        if (segment.header.page_association == 0)
            continue;
        if (seen_pages.contains(segment.header.page_association))
            continue;
        seen_pages.set(segment.header.page_association);
        pages.append(segment.header.page_association);
    }

    // scan_for_page_size() already checked that there's a page 1.
    VERIFY(seen_pages.contains(1));
    if (pages.size() == 1)
        return {};

    StringBuilder builder;
    builder.appendff("JBIG2 file contains {} pages ({}", pages.size(), pages[0]);
    size_t i;
    for (i = 1; i < min(pages.size(), 10); ++i)
        builder.appendff(" {}", pages[i]);
    if (i != pages.size())
        builder.append(" ..."sv);
    builder.append("). We will only render page 1."sv);
    dbgln("JBIG2ImageDecoderPlugin: {}", TRY(builder.to_string()));

    return {};
}

struct AdaptiveTemplatePixel {
    i8 x { 0 };
    i8 y { 0 };
};

// Figure 7 – Field to which AT pixel locations are restricted
static ErrorOr<void> check_valid_adaptive_template_pixel(AdaptiveTemplatePixel const& adaptive_template_pixel)
{
    // Don't have to check < -127 or > 127: The offsets are stored in an i8, so they can't be out of those bounds.
    if (adaptive_template_pixel.y > 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Adaptive pixel y too big");
    if (adaptive_template_pixel.y == 0 && adaptive_template_pixel.x > -1)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Adaptive pixel x too big");
    return {};
}

// 6.2.2 Input parameters
// Table 2 – Parameters for the generic region decoding procedure
struct GenericRegionDecodingInputParameters {
    bool is_modified_modified_read { false }; // "MMR" in spec.
    u32 region_width { 0 };                   // "GBW" in spec.
    u32 region_height { 0 };                  // "GBH" in spec.
    u8 gb_template { 0 };
    bool is_typical_prediction_used { false };          // "TPGDON" in spec.
    bool is_extended_reference_template_used { false }; // "EXTTEMPLATE" in spec.
    Optional<BitBuffer const&> skip_pattern;            // "USESKIP", "SKIP" in spec.

    Array<AdaptiveTemplatePixel, 12> adaptive_template_pixels; // "GBATX" / "GBATY" in spec.
    // FIXME: GBCOLS, GBCOMBOP, COLEXTFLAG

    // If is_modified_modified_read is false, generic_region_decoding_procedure() reads data off this decoder.
    QMArithmeticDecoder* arithmetic_decoder { nullptr };
};

// 6.2 Generic region decoding procedure
static ErrorOr<NonnullOwnPtr<BitBuffer>> generic_region_decoding_procedure(GenericRegionDecodingInputParameters const& inputs, ReadonlyBytes data, Vector<QMArithmeticDecoder::Context>& contexts)
{
    if (inputs.is_modified_modified_read) {
        dbgln_if(JBIG2_DEBUG, "JBIG2ImageDecoderPlugin: MMR image data");

        // 6.2.6 Decoding using MMR coding
        auto buffer = TRY(CCITT::decode_ccitt_group4(data, inputs.region_width, inputs.region_height));
        auto result = TRY(BitBuffer::create(inputs.region_width, inputs.region_height));
        size_t bytes_per_row = ceil_div(inputs.region_width, 8);
        if (buffer.size() != bytes_per_row * inputs.region_height)
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Decoded MMR data has wrong size");

        // FIXME: Could probably just copy the ByteBuffer directly into the BitBuffer's internal ByteBuffer instead.
        for (size_t y = 0; y < inputs.region_height; ++y) {
            for (size_t x = 0; x < inputs.region_width; ++x) {
                bool bit = buffer[y * bytes_per_row + x / 8] & (1 << (7 - x % 8));
                result->set_bit(x, y, bit);
            }
        }
        return result;
    }

    // 6.2.5 Decoding using a template and arithmetic coding
    if (inputs.is_extended_reference_template_used)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode EXTTEMPLATE yet");

    int number_of_adaptive_template_pixels = inputs.gb_template == 0 ? 4 : 1;
    for (int i = 0; i < number_of_adaptive_template_pixels; ++i)
        TRY(check_valid_adaptive_template_pixel(inputs.adaptive_template_pixels[i]));

    if (inputs.skip_pattern.has_value())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode USESKIP yet");

    auto result = TRY(BitBuffer::create(inputs.region_width, inputs.region_height));

    static constexpr auto get_pixel = [](NonnullOwnPtr<BitBuffer> const& buffer, int x, int y) -> bool {
        if (x < 0 || x >= (int)buffer->width() || y < 0)
            return false;
        return buffer->get_bit(x, y);
    };

    // Figure 3(a) – Template when GBTEMPLATE = 0 and EXTTEMPLATE = 0,
    constexpr auto compute_context_0 = [](NonnullOwnPtr<BitBuffer> const& buffer, ReadonlySpan<AdaptiveTemplatePixel> adaptive_pixels, int x, int y) -> u16 {
        u16 result = 0;
        for (int i = 0; i < 4; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x + adaptive_pixels[i].x, y + adaptive_pixels[i].y);
        for (int i = 0; i < 3; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 1 + i, y - 2);
        for (int i = 0; i < 5; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 2 + i, y - 1);
        for (int i = 0; i < 4; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 4 + i, y);
        return result;
    };

    // Figure 4 – Template when GBTEMPLATE = 1
    auto compute_context_1 = [](NonnullOwnPtr<BitBuffer> const& buffer, ReadonlySpan<AdaptiveTemplatePixel> adaptive_pixels, int x, int y) -> u16 {
        u16 result = 0;
        result = (result << 1) | (u16)get_pixel(buffer, x + adaptive_pixels[0].x, y + adaptive_pixels[0].y);
        for (int i = 0; i < 4; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 1 + i, y - 2);
        for (int i = 0; i < 5; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 2 + i, y - 1);
        for (int i = 0; i < 3; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 3 + i, y);
        return result;
    };

    // Figure 5 – Template when GBTEMPLATE = 2
    auto compute_context_2 = [](NonnullOwnPtr<BitBuffer> const& buffer, ReadonlySpan<AdaptiveTemplatePixel> adaptive_pixels, int x, int y) -> u16 {
        u16 result = 0;
        result = (result << 1) | (u16)get_pixel(buffer, x + adaptive_pixels[0].x, y + adaptive_pixels[0].y);
        for (int i = 0; i < 3; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 1 + i, y - 2);
        for (int i = 0; i < 4; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 2 + i, y - 1);
        for (int i = 0; i < 2; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 2 + i, y);
        return result;
    };

    // Figure 6 – Template when GBTEMPLATE = 3
    auto compute_context_3 = [](NonnullOwnPtr<BitBuffer> const& buffer, ReadonlySpan<AdaptiveTemplatePixel> adaptive_pixels, int x, int y) -> u16 {
        u16 result = 0;
        result = (result << 1) | (u16)get_pixel(buffer, x + adaptive_pixels[0].x, y + adaptive_pixels[0].y);
        for (int i = 0; i < 5; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 3 + i, y - 1);
        for (int i = 0; i < 4; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 4 + i, y);
        return result;
    };

    u16 (*compute_context)(NonnullOwnPtr<BitBuffer> const&, ReadonlySpan<AdaptiveTemplatePixel>, int, int);
    if (inputs.gb_template == 0)
        compute_context = compute_context_0;
    else if (inputs.gb_template == 1)
        compute_context = compute_context_1;
    else if (inputs.gb_template == 2)
        compute_context = compute_context_2;
    else {
        VERIFY(inputs.gb_template == 3);
        compute_context = compute_context_3;
    }

    // "The values of the pixels in this neighbourhood define a context. Each context has its own adaptive probability estimate
    //  used by the arithmetic coder (see Annex E)."
    // "* Decode the current pixel by invoking the arithmetic entropy decoding procedure, with CX set to the value formed by
    //    concatenating the label "GB" and the 10-16 pixel values gathered in CONTEXT."
    // Implementor's note: What this is supposed to mean is that we have a bunch of independent contexts, and we pick the
    // context for the current pixel based on pixel values in the neighborhood. The "GB" part just means this context is
    // independent from other contexts in the spec. They are passed in to this function.

    // Figure 8 – Reused context for coding the SLTP value when GBTEMPLATE is 0
    constexpr u16 sltp_context_for_template_0 = 0b10011'0110010'0101;

    // Figure 9 – Reused context for coding the SLTP value when GBTEMPLATE is 1
    constexpr u16 sltp_context_for_template_1 = 0b0011'110010'101;

    // Figure 10 – Reused context for coding the SLTP value when GBTEMPLATE is 2
    constexpr u16 sltp_context_for_template_2 = 0b001'11001'01;

    // Figure 11 – Reused context for coding the SLTP value when GBTEMPLATE is 3
    constexpr u16 sltp_context_for_template_3 = 0b011001'0101;

    u16 sltp_context = [](u8 gb_template) {
        if (gb_template == 0)
            return sltp_context_for_template_0;
        if (gb_template == 1)
            return sltp_context_for_template_1;
        if (gb_template == 2)
            return sltp_context_for_template_2;
        VERIFY(gb_template == 3);
        return sltp_context_for_template_3;
    }(inputs.gb_template);

    // 6.2.5.7 Decoding the bitmap
    QMArithmeticDecoder& decoder = *inputs.arithmetic_decoder;
    bool ltp = false; // "LTP" in spec. "Line (uses) Typical Prediction" maybe?
    for (size_t y = 0; y < inputs.region_height; ++y) {
        if (inputs.is_typical_prediction_used) {
            // "SLTP" in spec. "Swap LTP" or "Switch LTP" maybe?
            bool sltp = decoder.get_next_bit(contexts[sltp_context]);
            ltp = ltp ^ sltp;
            if (ltp) {
                for (size_t x = 0; x < inputs.region_width; ++x)
                    result->set_bit(x, y, get_pixel(result, (int)x, (int)y - 1));
                continue;
            }
        }

        for (size_t x = 0; x < inputs.region_width; ++x) {
            u16 context = compute_context(result, inputs.adaptive_template_pixels, x, y);
            bool bit = decoder.get_next_bit(contexts[context]);
            result->set_bit(x, y, bit);
        }
    }

    return result;
}

// 6.3.2 Input parameters
// Table 6 – Parameters for the generic refinement region decoding procedure
struct GenericRefinementRegionDecodingInputParameters {
    u32 region_width { 0 };                                   // "GRW" in spec.
    u32 region_height { 0 };                                  // "GRH" in spec.
    u8 gr_template { 0 };                                     // "GRTEMPLATE" in spec.
    BitBuffer const* reference_bitmap { nullptr };            // "GRREFERENCE" in spec.
    i32 reference_x_offset { 0 };                             // "GRREFERENCEDX" in spec.
    i32 reference_y_offset { 0 };                             // "GRREFERENCEDY" in spec.
    bool is_typical_prediction_used { false };                // "TPGDON" in spec.
    Array<AdaptiveTemplatePixel, 2> adaptive_template_pixels; // "GRATX" / "GRATY" in spec.
};

// 6.3 Generic Refinement Region Decoding Procedure
static ErrorOr<NonnullOwnPtr<BitBuffer>> generic_refinement_region_decoding_procedure(GenericRefinementRegionDecodingInputParameters& inputs, QMArithmeticDecoder& decoder, Vector<QMArithmeticDecoder::Context>& contexts)
{
    VERIFY(inputs.gr_template == 0 || inputs.gr_template == 1);

    if (inputs.is_typical_prediction_used)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode typical prediction in generic refinement regions yet");

    if (inputs.gr_template == 0) {
        TRY(check_valid_adaptive_template_pixel(inputs.adaptive_template_pixels[0]));
        // inputs.adaptive_template_pixels[1] is allowed to contain any value.
    }
    // GRTEMPLATE 1 never uses adaptive pixels.

    // 6.3.5.3 Fixed templates and adaptive templates
    static constexpr auto get_pixel = [](BitBuffer const& buffer, int x, int y) -> bool {
        if (x < 0 || x >= (int)buffer.width() || y < 0 || y >= (int)buffer.height())
            return false;
        return buffer.get_bit(x, y);
    };

    // Figure 12 – 13-pixel refinement template showing the AT pixels at their nominal locations
    constexpr auto compute_context_0 = [](ReadonlySpan<AdaptiveTemplatePixel> adaptive_pixels, BitBuffer const& reference, int reference_x, int reference_y, BitBuffer const& buffer, int x, int y) -> u16 {
        u16 result = 0;

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dy == -1 && dx == -1)
                    result = (result << 1) | (u16)get_pixel(reference, reference_x + adaptive_pixels[1].x, reference_y + adaptive_pixels[1].y);
                else
                    result = (result << 1) | (u16)get_pixel(reference, reference_x + dx, reference_y + dy);
            }
        }

        result = (result << 1) | (u16)get_pixel(buffer, x + adaptive_pixels[0].x, y + adaptive_pixels[0].y);
        for (int i = 0; i < 2; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x + i, y - 1);
        result = (result << 1) | (u16)get_pixel(buffer, x - 1, y);

        return result;
    };

    // Figure 13 – 10-pixel refinement template
    constexpr auto compute_context_1 = [](ReadonlySpan<AdaptiveTemplatePixel>, BitBuffer const& reference, int reference_x, int reference_y, BitBuffer const& buffer, int x, int y) -> u16 {
        u16 result = 0;

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dy == -1 && (dx == -1 || dx == 1)) || (dy == 1 && dx == -1))
                    continue;
                result = (result << 1) | (u16)get_pixel(reference, reference_x + dx, reference_y + dy);
            }
        }

        for (int i = 0; i < 3; ++i)
            result = (result << 1) | (u16)get_pixel(buffer, x - 1 + i, y - 1);
        result = (result << 1) | (u16)get_pixel(buffer, x - 1, y);

        return result;
    };

    auto compute_context = inputs.gr_template == 0 ? compute_context_0 : compute_context_1;

    // 6.3.5.6 Decoding the refinement bitmap
    auto result = TRY(BitBuffer::create(inputs.region_width, inputs.region_height));
    for (size_t y = 0; y < result->height(); ++y) {
        for (size_t x = 0; x < result->width(); ++x) {
            u16 context = compute_context(inputs.adaptive_template_pixels, *inputs.reference_bitmap, x - inputs.reference_x_offset, y - inputs.reference_y_offset, *result, x, y);
            bool bit = decoder.get_next_bit(contexts[context]);
            result->set_bit(x, y, bit);
        }
    }

    return result;
}

// 6.4.2 Input parameters
// Table 9 – Parameters for the text region decoding procedure
struct TextRegionDecodingInputParameters {
    bool uses_huffman_encoding { false };     // "SBHUFF" in spec.
    bool uses_refinement_coding { false };    // "SBREFINE" in spec.
    u32 region_width { 0 };                   // "SBW" in spec.
    u32 region_height { 0 };                  // "SBH" in spec.
    u32 number_of_instances { 0 };            // "SBNUMINSTANCES" in spec.
    u32 size_of_symbol_instance_strips { 0 }; // "SBSTRIPS" in spec.
    // "SBNUMSYMS" is `symbols.size()` below.

    // FIXME: SBSYMCODES
    u32 id_symbol_code_length { 0 };       // "SBSYMCODELEN" in spec.
    Vector<NonnullRefPtr<Symbol>> symbols; // "SBNUMSYMS" / "SBSYMS" in spec.
    u8 default_pixel { 0 };                // "SBDEFPIXEL" in spec.

    CombinationOperator operator_ { CombinationOperator::Or }; // "SBCOMBOP" in spec.

    bool is_transposed { false }; // "TRANSPOSED" in spec.

    enum class Corner {
        BottomLeft = 0,
        TopLeft = 1,
        BottomRight = 2,
        TopRight = 3,
    };
    Corner reference_corner { Corner::TopLeft }; // "REFCORNER" in spec.

    i8 delta_s_offset { 0 }; // "SBDSOFFSET" in spec.
    // FIXME: SBHUFFFS, SBHUFFFDS, SBHUFFDT, SBHUFFRDW, SBHUFFRDH, SBHUFFRDX, SBHUFFRDY, SBHUFFRSIZE

    u8 refinement_template { 0 };                                        // "SBRTEMPLATE" in spec.
    Array<AdaptiveTemplatePixel, 2> refinement_adaptive_template_pixels; // "SBRATX" / "SBRATY" in spec.
    // FIXME: COLEXTFLAG, SBCOLS
};

// 6.4 Text Region Decoding Procedure
static ErrorOr<NonnullOwnPtr<BitBuffer>> text_region_decoding_procedure(TextRegionDecodingInputParameters const& inputs, ReadonlyBytes data)
{
    if (inputs.uses_huffman_encoding)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode huffman text regions yet");

    auto decoder = TRY(QMArithmeticDecoder::initialize(data));

    // 6.4.6 Strip delta T
    // "If SBHUFF is 1, decode a value using the Huffman table specified by SBHUFFDT and multiply the resulting value by SBSTRIPS.
    //  If SBHUFF is 0, decode a value using the IADT integer arithmetic decoding procedure (see Annex A) and multiply the resulting value by SBSTRIPS."
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder delta_t_integer_decoder(decoder);
    auto read_delta_t = [&]() -> ErrorOr<i32> {
        return TRY(delta_t_integer_decoder.decode_non_oob()) * inputs.size_of_symbol_instance_strips;
    };

    // 6.4.7 First symbol instance S coordinate
    // "If SBHUFF is 1, decode a value using the Huffman table specified by SBHUFFFS.
    //  If SBHUFF is 0, decode a value using the IAFS integer arithmetic decoding procedure (see Annex A)."
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder first_s_integer_decoder(decoder);
    auto read_first_s = [&]() -> ErrorOr<i32> {
        return first_s_integer_decoder.decode_non_oob();
    };

    // 6.4.8 Subsequent symbol instance S coordinate
    // "If SBHUFF is 1, decode a value using the Huffman table specified by SBHUFFDS.
    //  If SBHUFF is 0, decode a value using the IADS integer arithmetic decoding procedure (see Annex A).
    //  In either case it is possible that the result of this decoding is the out-of-band value OOB.""
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder subsequent_s_integer_decoder(decoder);
    auto read_subsequent_s = [&]() -> Optional<i32> {
        return subsequent_s_integer_decoder.decode();
    };

    // 6.4.9 Symbol instance T coordinate
    // "If SBSTRIPS == 1, then the value decoded is always zero. Otherwise:
    //  • If SBHUFF is 1, decode a value by reading ceil(log2(SBSTRIPS)) bits directly from the bitstream.
    //  • If SBHUFF is 0, decode a value using the IAIT integer arithmetic decoding procedure (see Annex A)."
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder instance_t_integer_decoder(decoder);
    auto read_instance_t = [&]() -> ErrorOr<i32> {
        if (inputs.size_of_symbol_instance_strips == 1)
            return 0;
        return instance_t_integer_decoder.decode_non_oob();
    };

    // 6.4.10 Symbol instance symbol ID
    // "If SBHUFF is 1, decode a value by reading one bit at a time until the resulting bit string is equal to one of the entries in
    //  SBSYMCODES. The resulting value, which is IDI, is the index of the entry in SBSYMCODES that is read.
    //  If SBHUFF is 0, decode a value using the IAID integer arithmetic decoding procedure (see Annex A). Set IDI to the
    //  resulting value.""
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerIDDecoder id_decoder(decoder, inputs.id_symbol_code_length);

    // 6.4.11.1 Symbol instance refinement delta width
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder refinement_delta_width_decoder(decoder);
    auto read_refinement_delta_width = [&]() -> ErrorOr<i32> {
        return refinement_delta_width_decoder.decode_non_oob();
    };

    // 6.4.11.2 Symbol instance refinement delta width
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder refinement_delta_height_decoder(decoder);
    auto read_refinement_delta_height = [&]() -> ErrorOr<i32> {
        return refinement_delta_height_decoder.decode_non_oob();
    };

    // 6.4.11.3 Symbol instance refinement X offset
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder refinement_x_offset_decoder(decoder);
    auto read_refinement_x_offset = [&]() -> ErrorOr<i32> {
        return refinement_x_offset_decoder.decode_non_oob();
    };

    // 6.4.11.4 Symbol instance refinement Y offset
    // FIXME: Implement support for SBHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder refinement_y_offset_decoder(decoder);
    auto read_refinement_y_offset = [&]() -> ErrorOr<i32> {
        return refinement_y_offset_decoder.decode_non_oob();
    };

    // 6.4.11 Symbol instance bitmap
    JBIG2::ArithmeticIntegerDecoder has_refinement_image_decoder(decoder);
    Vector<QMArithmeticDecoder::Context> refinement_contexts;
    if (inputs.uses_refinement_coding)
        refinement_contexts.resize(1 << (inputs.refinement_template == 0 ? 13 : 10));
    OwnPtr<BitBuffer> refinement_result;
    auto read_bitmap = [&](u32 id) -> ErrorOr<BitBuffer const*> {
        if (id >= inputs.symbols.size())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Symbol ID out of range");
        auto const& symbol = inputs.symbols[id]->bitmap();

        bool has_refinement_image = false; // "R_I" in spec.
        if (inputs.uses_refinement_coding) {
            // "• If SBHUFF is 1, then read one bit and set RI to the value of that bit.
            //  • If SBHUFF is 0, then decode one bit using the IARI integer arithmetic decoding procedure and set RI to the value of that bit."
            // FIXME: Implement support for SBHUFF = 1.
            has_refinement_image = TRY(has_refinement_image_decoder.decode_non_oob());
        }

        if (!has_refinement_image)
            return &symbol;

        auto refinement_delta_width = TRY(read_refinement_delta_width());
        auto refinement_delta_height = TRY(read_refinement_delta_height());
        auto refinement_x_offset = TRY(read_refinement_x_offset());
        auto refinement_y_offset = TRY(read_refinement_y_offset());
        // FIXME: This is missing some steps needed for the SBHUFF = 1 case.

        dbgln_if(JBIG2_DEBUG, "refinement delta width: {}, refinement delta height: {}, refinement x offset: {}, refinement y offset: {}", refinement_delta_width, refinement_delta_height, refinement_x_offset, refinement_y_offset);

        // Table 12 – Parameters used to decode a symbol instance's bitmap using refinement
        GenericRefinementRegionDecodingInputParameters refinement_inputs;
        refinement_inputs.region_width = symbol.width() + refinement_delta_width;
        refinement_inputs.region_height = symbol.height() + refinement_delta_height;
        refinement_inputs.gr_template = inputs.refinement_template;
        refinement_inputs.reference_bitmap = &symbol;
        refinement_inputs.reference_x_offset = refinement_delta_width / 2 + refinement_x_offset;
        refinement_inputs.reference_y_offset = refinement_delta_height / 2 + refinement_y_offset;
        refinement_inputs.is_typical_prediction_used = false;
        refinement_inputs.adaptive_template_pixels = inputs.refinement_adaptive_template_pixels;
        refinement_result = TRY(generic_refinement_region_decoding_procedure(refinement_inputs, decoder, refinement_contexts));
        return refinement_result.ptr();
    };

    // 6.4.5 Decoding the text region

    // "1) Fill a bitmap SBREG, of the size given by SBW and SBH, with the SBDEFPIXEL value."
    auto result = TRY(BitBuffer::create(inputs.region_width, inputs.region_height));
    if (inputs.default_pixel != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot handle SBDEFPIXEL not equal to 0 yet");
    result->fill(inputs.default_pixel != 0);

    // "2) Decode the initial STRIPT value as described in 6.4.6. Negate the decoded value and assign this negated value to the variable STRIPT.
    //     Assign the value 0 to FIRSTS. Assign the value 0 to NINSTANCES."
    i32 strip_t = -TRY(read_delta_t());
    i32 first_s = 0;
    u32 n_instances = 0;

    // "3) If COLEXTFLAG is 1, decode the colour section as described in 6.4.12."
    // FIXME: Implement support for colors one day.

    // "4) Decode each strip as follows:
    //      a) If NINSTANCES is equal to SBNUMINSTANCES then there are no more strips to decode,
    //         and the process of decoding the text region is complete; proceed to step 4)."
    // Implementor's note. The spec means "proceed to step 5)" at the end of 4a).
    while (n_instances < inputs.number_of_instances) {
        // "b) Decode the strip's delta T value as described in 6.4.6. Let DT be the decoded value. Set:
        //         STRIPT = STRIPT + DT"
        i32 delta_t = TRY(read_delta_t());
        strip_t += delta_t;

        i32 cur_s;
        bool is_first_symbol = true;
        while (true) {
            // "c) Decode each symbol instance in the strip as follows:
            //      i) If the current symbol instance is the first symbol instance in the strip, then decode the first
            //         symbol instance's S coordinate as described in 6.4.7. Let DFS be the decoded value. Set:
            //              FIRSTS = FIRSTS + DFS
            //              CURS = FIRSTS
            //      ii) Otherwise, if the current symbol instance is not the first symbol instance in the strip, decode
            //          the symbol instance's S coordinate as described in 6.4.8. If the result of this decoding is OOB
            //          then the last symbol instance of the strip has been decoded; proceed to step 3 d). Otherwise, let
            //          IDS be the decoded value. Set:
            //              CURS = CURS + IDS + SBDSOFFSET"
            // Implementor's note: The spec means "proceed to step 4 d)" in 4c ii).
            if (is_first_symbol) {
                i32 delta_first_s = TRY(read_first_s());
                first_s += delta_first_s;
                cur_s = first_s;
                is_first_symbol = false;
            } else {
                auto subsequent_s = read_subsequent_s();
                if (!subsequent_s.has_value())
                    break;
                i32 instance_delta_s = subsequent_s.value();
                cur_s += instance_delta_s + inputs.delta_s_offset;
            }

            //     "iii) Decode the symbol instance's T coordinate as described in 6.4.9. Let CURT be the decoded value. Set:
            //              TI = STRIPT + CURT"
            i32 cur_t = TRY(read_instance_t());
            i32 t_instance = strip_t + cur_t;

            //     "iv) Decode the symbol instance's symbol ID as described in 6.4.10. Let IDI be the decoded value."
            u32 id = id_decoder.decode();

            //     "v) Determine the symbol instance's bitmap IBI as described in 6.4.11. The width and height of this
            //         bitmap shall be denoted as WI and HI respectively."
            auto const& symbol = *TRY(read_bitmap(id));

            //     "vi) Update CURS as follows:
            //      • If TRANSPOSED is 0, and REFCORNER is TOPRIGHT or BOTTOMRIGHT, set:
            //              CURS = CURS + WI – 1
            //      • If TRANSPOSED is 1, and REFCORNER is BOTTOMLEFT or BOTTOMRIGHT, set:
            //              CURS = CURS + HI – 1
            //      • Otherwise, do not change CURS in this step."
            using enum TextRegionDecodingInputParameters::Corner;
            if (!inputs.is_transposed && (inputs.reference_corner == TopRight || inputs.reference_corner == BottomRight))
                cur_s += symbol.width() - 1;
            if (inputs.is_transposed && (inputs.reference_corner == BottomLeft || inputs.reference_corner == BottomRight))
                cur_s += symbol.height() - 1;

            //     "vii) Set:
            //              SI = CURS"
            auto s_instance = cur_s;

            //     "viii) Determine the location of the symbol instance bitmap with respect to SBREG as follows:
            //          • If TRANSPOSED is 0, then:
            //              – If REFCORNER is TOPLEFT then the top left pixel of the symbol instance bitmap
            //                IBI shall be placed at SBREG[SI, TI].
            //              – If REFCORNER is TOPRIGHT then the top right pixel of the symbol instance
            //                bitmap IBI shall be placed at SBREG[SI, TI].
            //              – If REFCORNER is BOTTOMLEFT then the bottom left pixel of the symbol
            //                instance bitmap IBI shall be placed at SBREG[SI, TI].
            //              – If REFCORNER is BOTTOMRIGHT then the bottom right pixel of the symbol
            //                instance bitmap IBI shall be placed at SBREG[SI, TI].
            //          • If TRANSPOSED is 1, then:
            //              – If REFCORNER is TOPLEFT then the top left pixel of the symbol instance bitmap
            //                IBI shall be placed at SBREG[TI, SI].
            //              – If REFCORNER is TOPRIGHT then the top right pixel of the symbol instance
            //                bitmap IBI shall be placed at SBREG[TI, SI].
            //              – If REFCORNER is BOTTOMLEFT then the bottom left pixel of the symbol
            //                instance bitmap IBI shall be placed at SBREG[TI, SI].
            //              – If REFCORNER is BOTTOMRIGHT then the bottom right pixel of the symbol
            //                instance bitmap IBI shall be placed at SBREG[TI, SI].
            //          If any part of IBI, when placed at this location, lies outside the bounds of SBREG, then ignore
            //          this part of IBI in step 3 c) ix)."
            // Implementor's note: The spec means "ignore this part of IBI in step 3 c) x)" in 3c viii)'s last sentence.
            if (inputs.is_transposed)
                swap(s_instance, t_instance);
            if (inputs.reference_corner == TopRight || inputs.reference_corner == BottomRight)
                s_instance -= symbol.width() - 1;
            if (inputs.reference_corner == BottomLeft || inputs.reference_corner == BottomRight)
                t_instance -= symbol.height() - 1;

            //     "ix) If COLEXTFLAG is 1, set the colour specified by SBCOLS[SBFGCOLID[NINSTANCES]]
            //          to the foreground colour of the symbol instance bitmap IBI."
            // FIXME: Implement support for colors one day.

            //     "x) Draw IBI into SBREG. Combine each pixel of IBI with the current value of the corresponding
            //         pixel in SBREG, using the combination operator specified by SBCOMBOP. Write the results
            //         of each combination into that pixel in SBREG."
            dbgln_if(JBIG2_DEBUG, "combining symbol {} ({}x{}) at ({}, {}) with operator {}", id, symbol.width(), symbol.height(), s_instance, t_instance, (int)inputs.operator_);
            composite_bitbuffer(*result, symbol, { s_instance, t_instance }, inputs.operator_);

            //     "xi) Update CURS as follows:
            //          • If TRANSPOSED is 0, and REFCORNER is TOPLEFT or BOTTOMLEFT, set:
            //              CURS = CURS + WI – 1
            //          • If TRANSPOSED is 1, and REFCORNER is TOPLEFT or TOPRIGHT, set:
            //              CURS = CURS + HI – 1
            //          • Otherwise, do not change CURS in this step."
            if (!inputs.is_transposed && (inputs.reference_corner == TopLeft || inputs.reference_corner == BottomLeft))
                cur_s += symbol.width() - 1;
            if (inputs.is_transposed && (inputs.reference_corner == TopLeft || inputs.reference_corner == TopRight))
                cur_s += symbol.height() - 1;

            //      "xii) Set:
            //              NINSTANCES = NINSTANCES + 1"
            ++n_instances;
        }
        //  "d) When the strip has been completely decoded, decode the next strip."
        // (Done in the next loop iteration.)
    }

    //  "5) After all the strips have been decoded, the current contents of SBREG are the results that shall be
    //      obtained by every decoder, whether it performs this exact sequence of steps or not."
    return result;
}

// 6.5.2 Input parameters
// Table 13 – Parameters for the symbol dictionary decoding procedure
struct SymbolDictionaryDecodingInputParameters {

    bool uses_huffman_encoding { false };               // "SDHUFF" in spec.
    bool uses_refinement_or_aggregate_coding { false }; // "SDREFAGG" in spec.

    Vector<NonnullRefPtr<Symbol>> input_symbols; // "SDNUMINSYMS", "SDINSYMS" in spec.

    u32 number_of_new_symbols { 0 };      // "SDNUMNEWSYMS" in spec.
    u32 number_of_exported_symbols { 0 }; // "SDNUMEXSYMS" in spec.

    // FIXME: SDHUFFDH, SDHUFFDW, SDHUFFBMSIZE, SDHUFFAGGINST

    u8 symbol_template { 0 };                                 // "SDTEMPLATE" in spec.
    Array<AdaptiveTemplatePixel, 4> adaptive_template_pixels; // "SDATX" / "SDATY" in spec.

    u8 refinement_template { 0 };                                        // "SDRTEMPLATE" in spec;
    Array<AdaptiveTemplatePixel, 2> refinement_adaptive_template_pixels; // "SDRATX" / "SDRATY" in spec.
};

// 6.5 Symbol Dictionary Decoding Procedure
static ErrorOr<Vector<NonnullRefPtr<Symbol>>> symbol_dictionary_decoding_procedure(SymbolDictionaryDecodingInputParameters const& inputs, ReadonlyBytes data)
{
    if (inputs.uses_huffman_encoding)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode huffman symbol dictionaries yet");

    auto decoder = TRY(QMArithmeticDecoder::initialize(data));
    Vector<QMArithmeticDecoder::Context> contexts;
    contexts.resize(1 << number_of_context_bits_for_template(inputs.symbol_template));

    // 6.5.6 Height class delta height
    // "If SDHUFF is 1, decode a value using the Huffman table specified by SDHUFFDH.
    //  If SDHUFF is 0, decode a value using the IADH integer arithmetic decoding procedure (see Annex A)."
    // FIXME: Implement support for SDHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder delta_height_integer_decoder(decoder);
    auto read_delta_height = [&]() -> ErrorOr<i32> {
        return delta_height_integer_decoder.decode_non_oob();
    };

    // 6.5.7 Delta width
    // "If SDHUFF is 1, decode a value using the Huffman table specified by SDHUFFDW.
    //  If SDHUFF is 0, decode a value using the IADW integer arithmetic decoding procedure (see Annex A).
    //  In either case it is possible that the result of this decoding is the out-of-band value OOB."
    // FIXME: Implement support for SDHUFF = 1.
    JBIG2::ArithmeticIntegerDecoder delta_width_integer_decoder(decoder);
    auto read_delta_width = [&]() -> Optional<i32> {
        return delta_width_integer_decoder.decode();
    };

    // 6.5.8 Symbol bitmap
    // "This field is only present if SDHUFF = 0 or SDREFAGG = 1. This field takes one of two forms; SDREFAGG
    //  determines which form is used."

    // 6.5.8.2.1 Number of symbol instances in aggregation
    // If SDHUFF is 1, decode a value using the Huffman table specified by SDHUFFAGGINST.
    // If SDHUFF is 0, decode a value using the IAAI integer arithmetic decoding procedure (see Annex A).
    // FIXME: Implement support for SDHUFF = 1.
    Optional<JBIG2::ArithmeticIntegerDecoder> number_of_symbol_instances_decoder;
    auto read_number_of_symbol_instances = [&]() -> ErrorOr<i32> {
        if (!number_of_symbol_instances_decoder.has_value())
            number_of_symbol_instances_decoder = JBIG2::ArithmeticIntegerDecoder(decoder);
        return number_of_symbol_instances_decoder->decode_non_oob();
    };

    // 6.5.8.1 Direct-coded symbol bitmap
    Optional<JBIG2::ArithmeticIntegerIDDecoder> id_decoder;
    Optional<JBIG2::ArithmeticIntegerDecoder> refinement_x_offset_decoder;
    Optional<JBIG2::ArithmeticIntegerDecoder> refinement_y_offset_decoder;

    // FIXME: When we implement REFAGGNINST > 1 support, do these need to be shared with
    // text_region_decoding_procedure() then?
    Vector<QMArithmeticDecoder::Context> refinement_contexts;

    // This belongs in 6.5.5 1) below, but also needs to be captured by read_bitmap here.
    Vector<NonnullRefPtr<Symbol>> new_symbols;

    auto read_symbol_bitmap = [&](u32 width, u32 height) -> ErrorOr<NonnullOwnPtr<BitBuffer>> {
        // "If SDREFAGG is 0, then decode the symbol's bitmap using a generic region decoding procedure as described in 6.2.
        //  Set the parameters to this decoding procedure as shown in Table 16."
        if (!inputs.uses_refinement_or_aggregate_coding) {
            // Table 16 – Parameters used to decode a symbol's bitmap using generic bitmap decoding
            GenericRegionDecodingInputParameters generic_inputs;
            generic_inputs.is_modified_modified_read = false;
            generic_inputs.region_width = width;
            generic_inputs.region_height = height;
            generic_inputs.gb_template = inputs.symbol_template;
            generic_inputs.is_extended_reference_template_used = false; // Missing from spec in table 16.
            for (int i = 0; i < 4; ++i)
                generic_inputs.adaptive_template_pixels[i] = inputs.adaptive_template_pixels[i];
            generic_inputs.arithmetic_decoder = &decoder;
            return generic_region_decoding_procedure(generic_inputs, {}, contexts);
        }

        // 6.5.8.2 Refinement/aggregate-coded symbol bitmap
        // "1) Decode the number of symbol instances contained in the aggregation, as specified in 6.5.8.2.1. Let REFAGGNINST be the value decoded."
        auto number_of_symbol_instances = TRY(read_number_of_symbol_instances()); // "REFAGGNINST" in spec.
        dbgln_if(JBIG2_DEBUG, "Number of symbol instances: {}", number_of_symbol_instances);

        if (number_of_symbol_instances > 1) {
            // "2) If REFAGGNINST is greater than one, then decode the bitmap itself using a text region decoding procedure
            //     as described in 6.4. Set the parameters to this decoding procedure as shown in Table 17."
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode symbol bitmaps with more than one symbol instance yet");
        }

        // "3) If REFAGGNINST is equal to one, then decode the bitmap as described in 6.5.8.2.2."

        // 6.5.8.2.3 Setting SBSYMCODES and SBSYMCODELEN
        // FIXME: Implement support for SDHUFF = 1
        u32 code_length = ceil(log2(inputs.input_symbols.size() + inputs.number_of_new_symbols));

        // 6.5.8.2.2 Decoding a bitmap when REFAGGNINST = 1
        // FIXME: This is missing some steps for the SDHUFF = 1 case.
        if (number_of_symbol_instances != 1)
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Unexpected number of symbol instances");

        if (!id_decoder.has_value())
            id_decoder = JBIG2::ArithmeticIntegerIDDecoder(decoder, code_length);
        u32 symbol_id = id_decoder->decode();

        if (!refinement_x_offset_decoder.has_value())
            refinement_x_offset_decoder = JBIG2::ArithmeticIntegerDecoder(decoder);
        i32 refinement_x_offset = TRY(refinement_x_offset_decoder->decode_non_oob());

        if (!refinement_y_offset_decoder.has_value())
            refinement_y_offset_decoder = JBIG2::ArithmeticIntegerDecoder(decoder);
        i32 refinement_y_offset = TRY(refinement_y_offset_decoder->decode_non_oob());

        if (symbol_id >= inputs.input_symbols.size() && symbol_id - inputs.input_symbols.size() >= new_symbols.size())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Refinement/aggregate symbol ID out of range");

        auto IBO = (symbol_id < inputs.input_symbols.size()) ? inputs.input_symbols[symbol_id] : new_symbols[symbol_id - inputs.input_symbols.size()];
        // Table 18 – Parameters used to decode a symbol's bitmap when REFAGGNINST = 1
        GenericRefinementRegionDecodingInputParameters refinement_inputs;
        refinement_inputs.region_width = width;
        refinement_inputs.region_height = height;
        refinement_inputs.gr_template = inputs.refinement_template;
        refinement_inputs.reference_bitmap = &IBO->bitmap();
        refinement_inputs.reference_x_offset = refinement_x_offset;
        refinement_inputs.reference_y_offset = refinement_y_offset;
        refinement_inputs.is_typical_prediction_used = false;
        refinement_inputs.adaptive_template_pixels = inputs.refinement_adaptive_template_pixels;
        if (refinement_contexts.is_empty())
            refinement_contexts.resize(1 << (inputs.refinement_template == 0 ? 13 : 10));
        return generic_refinement_region_decoding_procedure(refinement_inputs, decoder, refinement_contexts);
    };

    // 6.5.5 Decoding the symbol dictionary
    // "1) Create an array SDNEWSYMS of bitmaps, having SDNUMNEWSYMS entries."
    // Done above read_symbol_bitmap's definition.

    // "2) If SDHUFF is 1 and SDREFAGG is 0, create an array SDNEWSYMWIDTHS of integers, having SDNUMNEWSYMS entries."
    // FIXME: Implement support for SDHUFF = 1.

    // "3) Set:
    //      HCHEIGHT = 0
    //      NSYMSDECODED = 0"
    u32 height_class_height = 0;
    u32 number_of_symbols_decoded = 0;

    // "4) Decode each height class as follows:
    //      a) If NSYMSDECODED == SDNUMNEWSYMS then all the symbols in the dictionary have been decoded; proceed to step 5)."
    while (number_of_symbols_decoded < inputs.number_of_new_symbols) {
        // "b) Decode the height class delta height as described in 6.5.6. Let HCDH be the decoded value. Set:
        //      HCHEIGHT = HCEIGHT + HCDH
        //      SYMWIDTH = 0
        //      TOTWIDTH = 0
        //      HCFIRSTSYM = NSYMSDECODED"
        i32 delta_height = TRY(read_delta_height());
        height_class_height += delta_height;
        u32 symbol_width = 0;
        u32 total_width = 0;
        u32 height_class_first_symbol = number_of_symbols_decoded;
        // "c) Decode each symbol within the height class as follows:"
        while (true) {
            // "i) Decode the delta width for the symbol as described in 6.5.7."
            auto opt_delta_width = read_delta_width();
            // "   If the result of this decoding is OOB then all the symbols in this height class have been decoded; proceed to step 4 d)."
            if (!opt_delta_width.has_value())
                break;

            VERIFY(number_of_symbols_decoded < inputs.number_of_new_symbols);
            // "   Otherwise let DW be the decoded value and set:"
            //         SYMWIDTH = SYMWIDTH + DW
            //         TOTWIDTH = TOTWIDTH + SYMWIDTH"
            i32 delta_width = opt_delta_width.value();
            symbol_width += delta_width;
            total_width += symbol_width;

            // "ii) If SDHUFF is 0 or SDREFAGG is 1, then decode the symbol's bitmap as described in 6.5.8.
            //      Let BS be the decoded bitmap (this bitmap has width SYMWIDTH and height HCHEIGHT). Set:
            //          SDNEWSYMS[NSYMSDECODED] = BS"
            // FIXME: Implement support for SDHUFF = 1.
            // FIXME: Doing this eagerly is pretty wasteful. Decode on demand instead?
            auto bitmap = TRY(read_symbol_bitmap(symbol_width, height_class_height));
            new_symbols.append(Symbol::create(move(bitmap)));

            // "iii) If SDHUFF is 1 and SDREFAGG is 0, then set:
            //      SDNEWSYMWIDTHS[NSYMSDECODED] = SYMWIDTH"
            // FIXME: Implement support for SDHUFF = 1.
            (void)total_width;
            (void)height_class_first_symbol;

            // "iv) Set:
            //      NSYMSDECODED = NSYMSDECODED + 1"
            number_of_symbols_decoded++;
        }
        // "d) If SDHUFF is 1 and SDREFAGG is 0, [...long text elided...]"
        // FIXME: Implement support for SDHUFF = 1.
    }

    // "5) Determine which symbol bitmaps are exported from this symbol dictionary, as described in 6.5.10. These
    //     bitmaps can be drawn from the symbols that are used as input to the symbol dictionary decoding
    //     procedure as well as the new symbols produced by the decoding procedure."
    JBIG2::ArithmeticIntegerDecoder export_integer_decoder(decoder);

    // 6.5.10 Exported symbols
    Vector<bool> export_flags;
    export_flags.resize(inputs.input_symbols.size() + inputs.number_of_new_symbols);

    // "1) Set:
    //      EXINDEX = 0
    //      CUREXFLAG = 0"
    u32 exported_index = 0;
    bool current_export_flag = false;

    do {
        // "2) Decode a value using Table B.1 if SDHUFF is 1, or the IAEX integer arithmetic decoding procedure if
        //  SDHUFF is 0. Let EXRUNLENGTH be the decoded value."
        // FIXME: Implement support for SDHUFF = 1.
        i32 export_run_length = TRY(export_integer_decoder.decode_non_oob());

        // "3) Set EXFLAGS[EXINDEX] through EXFLAGS[EXINDEX + EXRUNLENGTH – 1] to CUREXFLAG.
        //  If EXRUNLENGTH = 0, then this step does not change any values."
        for (int i = 0; i < export_run_length; ++i)
            export_flags[exported_index + i] = current_export_flag;

        // "4) Set:
        //      EXINDEX = EXINDEX + EXRUNLENGTH
        //      CUREXFLAG = NOT(CUREXFLAG)"
        exported_index += export_run_length;
        current_export_flag = !current_export_flag;

        //  5) Repeat steps 2) through 4) until EXINDEX == SDNUMINSYMS + SDNUMNEWSYMS.
    } while (exported_index < inputs.input_symbols.size() + inputs.number_of_new_symbols);

    // "6) The array EXFLAGS now contains 1 for each symbol that is exported from the dictionary, and 0 for each
    //  symbol that is not exported."
    Vector<NonnullRefPtr<Symbol>> exported_symbols;

    // "7) Set:
    //      I = 0
    //      J = 0
    //  8) For each value of I from 0 to SDNUMINSYMS + SDNUMNEWSYMS – 1,"
    for (size_t i = 0; i < inputs.input_symbols.size() + inputs.number_of_new_symbols; ++i) {
        // "if EXFLAGS[I] == 1 then perform the following steps:"
        if (!export_flags[i])
            continue;
        //  "a) If I < SDNUMINSYMS then set:
        //       SDEXSYMS[J] = SDINSYMS[I]
        //       J = J + 1"
        if (i < inputs.input_symbols.size())
            exported_symbols.append(inputs.input_symbols[i]);

        //  "b) If I >= SDNUMINSYMS then set:
        //       SDEXSYMS[J] = SDNEWSYMS[I – SDNUMINSYMS]
        //       J = J + 1"
        if (i >= inputs.input_symbols.size())
            exported_symbols.append(move(new_symbols[i - inputs.input_symbols.size()]));
    }

    if (exported_symbols.size() != inputs.number_of_exported_symbols)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Unexpected number of exported symbols");

    return exported_symbols;
}

// Annex C Gray-scale image decoding procedure

// C.2 Input parameters
// Table C.1 – Parameters for the gray-scale image decoding procedure
struct GrayscaleInputParameters {
    bool uses_mmr { false }; // "GSMMR" in spec.

    Optional<BitBuffer const&> skip_pattern; // "GSUSESKIP" / "GSKIP" in spec.

    u8 bpp { 0 };         // "GSBPP" in spec.
    u32 width { 0 };      // "GSW" in spec.
    u32 height { 0 };     // "GSH" in spec.
    u8 template_id { 0 }; // "GSTEMPLATE" in spec.

    // If uses_mmr is false, grayscale_image_decoding_procedure() reads data off this decoder.
    QMArithmeticDecoder* arithmetic_decoder { nullptr };
};

static ErrorOr<Vector<u8>> grayscale_image_decoding_procedure(GrayscaleInputParameters const& inputs, ReadonlyBytes data, Vector<QMArithmeticDecoder::Context>& contexts)
{
    // FIXME: Support this. generic_region_decoding_procedure() currently doesn't tell us how much data it
    //        reads for MMR bitmaps, so we can't currently read more than one MMR bitplane here.
    if (inputs.uses_mmr)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode MMR grayscale images yet");

    // Table C.4 – Parameters used to decode a bitplane of the gray-scale image
    GenericRegionDecodingInputParameters generic_inputs;
    generic_inputs.is_modified_modified_read = inputs.uses_mmr;
    generic_inputs.region_width = inputs.width;
    generic_inputs.region_height = inputs.height;
    generic_inputs.gb_template = inputs.template_id;
    generic_inputs.is_typical_prediction_used = false;
    generic_inputs.is_extended_reference_template_used = false; // Missing from spec.
    generic_inputs.skip_pattern = inputs.skip_pattern;
    generic_inputs.adaptive_template_pixels[0].x = inputs.template_id <= 1 ? 3 : 2;
    generic_inputs.adaptive_template_pixels[0].y = -1;
    generic_inputs.adaptive_template_pixels[1].x = -3;
    generic_inputs.adaptive_template_pixels[1].y = -1;
    generic_inputs.adaptive_template_pixels[2].x = 2;
    generic_inputs.adaptive_template_pixels[2].y = -2;
    generic_inputs.adaptive_template_pixels[3].x = -2;
    generic_inputs.adaptive_template_pixels[3].y = -2;
    generic_inputs.arithmetic_decoder = inputs.arithmetic_decoder;

    // C.5 Decoding the gray-scale image
    // "The gray-scale image is obtained by decoding GSBPP bitplanes. These bitplanes are denoted (from least significant to
    //  most significant) GSPLANES[0], GSPLANES[1], . . . , GSPLANES[GSBPP – 1]. The bitplanes are Gray-coded, so
    //  that each bitplane's true value is equal to its coded value XORed with the next-more-significant bitplane."
    Vector<OwnPtr<BitBuffer>> bitplanes;
    bitplanes.resize(inputs.bpp);

    // "1) Decode GSPLANES[GSBPP – 1] using the generic region decoding procedure. The parameters to the
    //     generic region decoding procedure are as shown in Table C.4."
    bitplanes[inputs.bpp - 1] = TRY(generic_region_decoding_procedure(generic_inputs, data, contexts));

    // "2) Set J = GSBPP – 2."
    int j = inputs.bpp - 2;

    // "3) While J >= 0, perform the following steps:"
    while (j >= 0) {
        // "a) Decode GSPLANES[J] using the generic region decoding procedure. The parameters to the generic
        //     region decoding procedure are as shown in Table C.4."
        bitplanes[j] = TRY(generic_region_decoding_procedure(generic_inputs, data, contexts));

        // "b) For each pixel (x, y) in GSPLANES[J], set:
        //     GSPLANES[J][x, y] = GSPLANES[J + 1][x, y] XOR GSPLANES[J][x, y]"
        for (u32 y = 0; y < inputs.height; ++y) {
            for (u32 x = 0; x < inputs.width; ++x) {
                bool bit = bitplanes[j + 1]->get_bit(x, y) ^ bitplanes[j]->get_bit(x, y);
                bitplanes[j]->set_bit(x, y, bit);
            }
        }

        // "c) Set J = J – 1."
        j = j - 1;
    }

    // "4) For each (x, y), set:
    //     GSVALS [x, y] = sum_{J = 0}^{GSBPP - 1} GSPLANES[J][x,y] × 2**J)"
    Vector<u8> result;
    result.resize(inputs.width * inputs.height);
    for (u32 y = 0; y < inputs.height; ++y) {
        for (u32 x = 0; x < inputs.width; ++x) {
            u8 value = 0;
            for (int j = 0; j < inputs.bpp; ++j) {
                if (bitplanes[j]->get_bit(x, y))
                    value |= 1 << j;
            }
            result[y * inputs.width + x] = value;
        }
    }
    return result;
}

// 6.6.2 Input parameters
// Table 20 – Parameters for the halftone region decoding procedure
struct HalftoneRegionDecodingInputParameters {
    u32 region_width { 0 };                                               // "HBW" in spec.
    u32 region_height { 0 };                                              // "HBH" in spec.
    bool uses_mmr { false };                                              // "HMMR" in spec.
    u8 halftone_template { 0 };                                           // "HTEMPLATE" in spec.
    Vector<NonnullRefPtr<Symbol>> patterns;                               // "HNUMPATS" / "HPATS" in spec.
    bool default_pixel_value { false };                                   // "HDEFPIXEL" in spec.
    CombinationOperator combination_operator { CombinationOperator::Or }; // "HCOMBOP" in spec.
    bool enable_skip { false };                                           // "HENABLESKIP" in spec.
    u32 grayscale_width { 0 };                                            // "HGW" in spec.
    u32 grayscale_height { 0 };                                           // "HGH" in spec.
    i32 grid_origin_x_offset { 0 };                                       // "HGX" in spec.
    i32 grid_origin_y_offset { 0 };                                       // "HGY" in spec.
    u16 grid_vector_x { 0 };                                              // "HRY" in spec.
    u16 grid_vector_y { 0 };                                              // "HRX" in spec.
    u8 pattern_width { 0 };                                               // "HPW" in spec.
    u8 pattern_height { 0 };                                              // "HPH" in spec.
};

// 6.6 Halftone Region Decoding Procedure
static ErrorOr<NonnullOwnPtr<BitBuffer>> halftone_region_decoding_procedure(HalftoneRegionDecodingInputParameters const& inputs, ReadonlyBytes data, Vector<QMArithmeticDecoder::Context>& contexts)
{
    // 6.6.5 Decoding the halftone region
    // "1) Fill a bitmap HTREG, of the size given by HBW and HBH, with the HDEFPIXEL value."
    auto result = TRY(BitBuffer::create(inputs.region_width, inputs.region_height));
    result->fill(inputs.default_pixel_value);

    // "2) If HENABLESKIP equals 1, compute a bitmap HSKIP as shown in 6.6.5.1."
    Optional<BitBuffer const&> skip_pattern;
    OwnPtr<BitBuffer> skip_pattern_storage;
    if (inputs.enable_skip) {
        // FIXME: This is untested; I haven't found a sample that uses HENABLESKIP yet.
        //        But generic_region_decoding_procedure() currently doesn't implement skip_pattern anyways
        //        and errors out on it, so we'll notice when this gets hit.
        skip_pattern_storage = TRY(BitBuffer::create(inputs.pattern_width, inputs.pattern_height));
        skip_pattern = *skip_pattern_storage;

        // 6.6.5.1 Computing HSKIP
        // "1) For each value of mg between 0 and HGH – 1, beginning from 0, perform the following steps:"
        for (int m_g = 0; m_g < (int)inputs.grayscale_height; ++m_g) {
            // "a) For each value of ng between 0 and HGW – 1, beginning from 0, perform the following steps:"
            for (int n_g = 0; n_g < (int)inputs.grayscale_width; ++n_g) {
                // "i) Set:
                //      x = (HGX + m_g × HRY + n_g × HRX) >> 8
                //      y = (HGY + m_g × HRX – n_g × HRY) >> 8"
                auto x = (inputs.grid_origin_x_offset + m_g * inputs.grid_vector_y + n_g * inputs.grid_vector_x) >> 8;
                auto y = (inputs.grid_origin_y_offset + m_g * inputs.grid_vector_x - n_g * inputs.grid_vector_y) >> 8;

                // "ii) If ((x + HPW <= 0) OR (x >= HBW) OR (y + HPH <= 0) OR (y >= HBH)) then set:
                //          HSKIP[n_g, m_g] = 1
                //      Otherwise, set:
                //          HSKIP[n_g, m_g] = 0"
                if (x + inputs.pattern_width <= 0 || x >= (int)inputs.region_width || y + inputs.pattern_height <= 0 || y >= (int)inputs.region_height)
                    skip_pattern_storage->set_bit(n_g, m_g, true);
                else
                    skip_pattern_storage->set_bit(n_g, m_g, false);
            }
        }
    }

    // "3) Set HBPP to ⌈log2 (HNUMPATS)⌉."
    u8 bits_per_pattern = ceil(log2(inputs.patterns.size()));

    // "4) Decode an image GI of size HGW by HGH with HBPP bits per pixel using the gray-scale image decoding
    //     procedure as described in Annex C. Set the parameters to this decoding procedure as shown in Table 23.
    //     Let GI be the results of invoking this decoding procedure."
    GrayscaleInputParameters grayscale_inputs;
    grayscale_inputs.uses_mmr = inputs.uses_mmr;
    grayscale_inputs.width = inputs.grayscale_width;
    grayscale_inputs.height = inputs.grayscale_height;
    grayscale_inputs.bpp = bits_per_pattern;
    grayscale_inputs.skip_pattern = skip_pattern;
    grayscale_inputs.template_id = inputs.halftone_template;

    Optional<QMArithmeticDecoder> decoder;
    if (!inputs.uses_mmr) {
        decoder = TRY(QMArithmeticDecoder::initialize(data));
        grayscale_inputs.arithmetic_decoder = &decoder.value();
    }

    auto grayscale_image = TRY(grayscale_image_decoding_procedure(grayscale_inputs, data, contexts));

    // "5) Place sequentially the patterns corresponding to the values in GI into HTREG by the procedure described in 6.6.5.2.
    //     The rendering procedure is illustrated in Figure 26. The outline of two patterns are marked by dotted boxes."
    {
        // 6.6.5.2 Rendering the patterns
        // "Draw the patterns into HTREG using the following procedure:
        //  1) For each value of m_g between 0 and HGH – 1, beginning from 0, perform the following steps."
        for (int m_g = 0; m_g < (int)inputs.grayscale_height; ++m_g) {
            // "a) For each value of n_g between 0 and HGW – 1, beginning from 0, perform the following steps."
            for (int n_g = 0; n_g < (int)inputs.grayscale_width; ++n_g) {
                // "i) Set:
                //      x = (HGX + m_g × HRY + n_g × HRX) >> 8
                //      y = (HGY + m_g × HRX – n_g × HRY) >> 8"
                auto x = (inputs.grid_origin_x_offset + m_g * inputs.grid_vector_y + n_g * inputs.grid_vector_x) >> 8;
                auto y = (inputs.grid_origin_y_offset + m_g * inputs.grid_vector_x - n_g * inputs.grid_vector_y) >> 8;

                // "ii) Draw the pattern HPATS[GI[n_g, m_g]] into HTREG such that its upper left pixel is at location (x, y) in HTREG.
                //
                //      A pattern is drawn into HTREG as follows. Each pixel of the pattern shall be combined with
                //      the current value of the corresponding pixel in the halftone-coded bitmap, using the
                //      combination operator specified by HCOMBOP. The results of each combination shall be
                //      written into that pixel in the halftone-coded bitmap.
                //
                //      If any part of a decoded pattern, when placed at location (x, y) lies outside the actual halftone-
                //      coded bitmap, then this part of the pattern shall be ignored in the process of combining the
                //      pattern with the bitmap."
                u8 grayscale_value = grayscale_image[n_g + m_g * inputs.grayscale_width];
                if (grayscale_value >= inputs.patterns.size())
                    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Grayscale value out of range");
                auto const& pattern = inputs.patterns[grayscale_value];
                composite_bitbuffer(*result, pattern->bitmap(), { x, y }, inputs.combination_operator);
            }
        }
    }

    // "6) After all the patterns have been placed on the bitmap, the current contents of the halftone-coded bitmap are
    //     the results that shall be obtained by every decoder, whether it performs this exact sequence of steps or not."
    return result;
}

// 6.7.2 Input parameters
// Table 24 – Parameters for the pattern dictionary decoding procedure
struct PatternDictionaryDecodingInputParameters {
    bool uses_mmr { false }; // "HDMMR" in spec.
    u32 width { 0 };         // "HDPW" in spec.
    u32 height { 0 };        // "HDPH" in spec.
    u32 gray_max { 0 };      // "GRAYMAX" in spec.
    u8 hd_template { 0 };    // "HDTEMPLATE" in spec.
};

// 6.7 Pattern Dictionary Decoding Procedure
static ErrorOr<Vector<NonnullRefPtr<Symbol>>> pattern_dictionary_decoding_procedure(PatternDictionaryDecodingInputParameters const& inputs, ReadonlyBytes data, Vector<QMArithmeticDecoder::Context>& contexts)
{
    // Table 27 – Parameters used to decode a pattern dictionary's collective bitmap
    GenericRegionDecodingInputParameters generic_inputs;
    generic_inputs.is_modified_modified_read = inputs.uses_mmr;
    generic_inputs.region_width = (inputs.gray_max + 1) * inputs.width;
    generic_inputs.region_height = inputs.height;
    generic_inputs.gb_template = inputs.hd_template;
    generic_inputs.is_typical_prediction_used = false;
    generic_inputs.is_extended_reference_template_used = false; // Missing from spec in table 27.
    generic_inputs.skip_pattern = OptionalNone {};
    generic_inputs.adaptive_template_pixels[0].x = -inputs.width;
    generic_inputs.adaptive_template_pixels[0].y = 0;
    generic_inputs.adaptive_template_pixels[1].x = -3;
    generic_inputs.adaptive_template_pixels[1].y = -1;
    generic_inputs.adaptive_template_pixels[2].x = 2;
    generic_inputs.adaptive_template_pixels[2].y = -2;
    generic_inputs.adaptive_template_pixels[3].x = -2;
    generic_inputs.adaptive_template_pixels[3].y = -2;

    Optional<QMArithmeticDecoder> decoder;
    if (!inputs.uses_mmr) {
        decoder = TRY(QMArithmeticDecoder::initialize(data));
        generic_inputs.arithmetic_decoder = &decoder.value();
    }

    auto bitmap = TRY(generic_region_decoding_procedure(generic_inputs, data, contexts));

    Vector<NonnullRefPtr<Symbol>> patterns;
    for (u32 gray = 0; gray <= inputs.gray_max; ++gray) {
        int x = gray * inputs.width;
        auto pattern = TRY(bitmap->subbitmap({ x, 0, static_cast<int>(inputs.width), static_cast<int>(inputs.height) }));
        patterns.append(Symbol::create(move(pattern)));
    }

    dbgln_if(JBIG2_DEBUG, "Pattern dictionary: {} patterns", patterns.size());

    return patterns;
}

static ErrorOr<void> decode_symbol_dictionary(JBIG2LoadingContext& context, SegmentData& segment)
{
    // 7.4.2 Symbol dictionary segment syntax

    // 7.4.2.1 Symbol dictionary segment data header
    FixedMemoryStream stream(segment.data);

    // 7.4.2.1.1 Symbol dictionary flags
    u16 flags = TRY(stream.read_value<BigEndian<u16>>());
    bool uses_huffman_encoding = (flags & 1) != 0;               // "SDHUFF" in spec.
    bool uses_refinement_or_aggregate_coding = (flags & 2) != 0; // "SDREFAGG" in spec.

    u8 huffman_table_selection_for_height_differences = (flags >> 2) & 0b11; // "SDHUFFDH" in spec.
    if (huffman_table_selection_for_height_differences == 2)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid huffman_table_selection_for_height_differences");
    if (!uses_huffman_encoding && huffman_table_selection_for_height_differences != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid huffman_table_selection_for_height_differences");

    u8 huffman_table_selection_for_width_differences = (flags >> 4) & 0b11; // "SDHUFFDW" in spec.
    if (huffman_table_selection_for_width_differences == 2)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid huffman_table_selection_for_width_differences");
    if (!uses_huffman_encoding && huffman_table_selection_for_width_differences != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid huffman_table_selection_for_width_differences");

    bool uses_user_supplied_size_table = (flags >> 6) & 1; // "SDHUFFBMSIZE" in spec.
    if (!uses_huffman_encoding && uses_user_supplied_size_table)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid uses_user_supplied_size_table");

    bool uses_user_supplied_aggregate_table = (flags >> 7) & 1; // "SDHUFFAGGINST" in spec.
    if (!uses_huffman_encoding && uses_user_supplied_aggregate_table)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid uses_user_supplied_aggregate_table");

    bool bitmap_coding_context_used = (flags >> 8) & 1;
    if (uses_huffman_encoding && !uses_refinement_or_aggregate_coding && bitmap_coding_context_used)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid bitmap_coding_context_used");

    bool bitmap_coding_context_retained = (flags >> 9) & 1;
    if (uses_huffman_encoding && !uses_refinement_or_aggregate_coding && bitmap_coding_context_retained)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid bitmap_coding_context_retained");

    u8 template_used = (flags >> 10) & 0b11; // "SDTEMPLATE" in spec.
    if (uses_huffman_encoding && template_used != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid template_used");

    u8 refinement_template_used = (flags >> 12) & 0b11; // "SDREFTEMPLATE" in spec.
    if (!uses_refinement_or_aggregate_coding && refinement_template_used != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid refinement_template_used");

    if (flags & 0b1110'0000'0000'0000)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid symbol dictionary flags");

    // 7.4.2.1.2 Symbol dictionary AT flags
    Array<AdaptiveTemplatePixel, 4> adaptive_template {};
    if (!uses_huffman_encoding) {
        int number_of_adaptive_template_pixels = template_used == 0 ? 4 : 1;
        for (int i = 0; i < number_of_adaptive_template_pixels; ++i) {
            adaptive_template[i].x = TRY(stream.read_value<i8>());
            adaptive_template[i].y = TRY(stream.read_value<i8>());
        }
    }

    // 7.4.2.1.3 Symbol dictionary refinement AT flags
    Array<AdaptiveTemplatePixel, 2> adaptive_refinement_template {};
    if (uses_refinement_or_aggregate_coding && refinement_template_used == 0) {
        for (size_t i = 0; i < adaptive_refinement_template.size(); ++i) {
            adaptive_refinement_template[i].x = TRY(stream.read_value<i8>());
            adaptive_refinement_template[i].y = TRY(stream.read_value<i8>());
        }
    }

    // 7.4.2.1.4 Number of exported symbols (SDNUMEXSYMS)
    u32 number_of_exported_symbols = TRY(stream.read_value<BigEndian<u32>>());

    // 7.4.2.1.5 Number of new symbols (SDNUMNEWSYMS)
    u32 number_of_new_symbols = TRY(stream.read_value<BigEndian<u32>>());

    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: uses_huffman_encoding={}", uses_huffman_encoding);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: uses_refinement_or_aggregate_coding={}", uses_refinement_or_aggregate_coding);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: huffman_table_selection_for_height_differences={}", huffman_table_selection_for_height_differences);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: huffman_table_selection_for_width_differences={}", huffman_table_selection_for_width_differences);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: uses_user_supplied_size_table={}", uses_user_supplied_size_table);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: uses_user_supplied_aggregate_table={}", uses_user_supplied_aggregate_table);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: bitmap_coding_context_used={}", bitmap_coding_context_used);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: bitmap_coding_context_retained={}", bitmap_coding_context_retained);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: template_used={}", template_used);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: refinement_template_used={}", refinement_template_used);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: number_of_exported_symbols={}", number_of_exported_symbols);
    dbgln_if(JBIG2_DEBUG, "Symbol dictionary: number_of_new_symbols={}", number_of_new_symbols);

    // 7.4.2.1.6 Symbol dictionary segment Huffman table selection
    // FIXME

    // 7.4.2.2 Decoding a symbol dictionary segment
    // "1) Interpret its header, as described in 7.4.2.1."
    // Done!

    // "2) Decode (or retrieve the results of decoding) any referred-to symbol dictionary and tables segments."
    Vector<NonnullRefPtr<Symbol>> symbols;
    for (auto referred_to_segment_number : segment.header.referred_to_segment_numbers) {
        auto opt_referred_to_segment = context.segments_by_number.get(referred_to_segment_number);
        if (!opt_referred_to_segment.has_value())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Symbol segment refers to non-existent segment");
        dbgln_if(JBIG2_DEBUG, "Symbol segment refers to segment id {} index {}", referred_to_segment_number, opt_referred_to_segment.value());
        auto const& referred_to_segment = context.segments[opt_referred_to_segment.value()];
        if (!referred_to_segment.symbols.has_value())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Symbol segment referred-to segment without symbols");
        symbols.extend(referred_to_segment.symbols.value());
    }

    // "3) If the "bitmap coding context used" bit in the header was 1, ..."
    if (bitmap_coding_context_used)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode bitmap coding context segment yet");

    // "4) If the "bitmap coding context used" bit in the header was 0, then, as described in E.3.7,
    //     reset all the arithmetic coding statistics for the generic region and generic refinement region decoding procedures to zero."
    // Nothing to do.

    // "5) Reset the arithmetic coding statistics for all the contexts of all the arithmetic integer coders to zero."
    // FIXME

    // "6) Invoke the symbol dictionary decoding procedure described in 6.5, with the parameters to the symbol dictionary decoding procedure set as shown in Table 31."
    SymbolDictionaryDecodingInputParameters inputs;
    inputs.uses_huffman_encoding = uses_huffman_encoding;
    inputs.uses_refinement_or_aggregate_coding = uses_refinement_or_aggregate_coding;
    inputs.input_symbols = move(symbols);
    inputs.number_of_new_symbols = number_of_new_symbols;
    inputs.number_of_exported_symbols = number_of_exported_symbols;
    // FIXME: SDHUFFDH, SDHUFFDW, SDHUFFBMSIZE, SDHUFFAGGINST
    inputs.symbol_template = template_used;
    inputs.adaptive_template_pixels = adaptive_template;
    inputs.refinement_template = refinement_template_used;
    inputs.refinement_adaptive_template_pixels = adaptive_refinement_template;
    auto result = TRY(symbol_dictionary_decoding_procedure(inputs, segment.data.slice(TRY(stream.tell()))));

    // "7) If the "bitmap coding context retained" bit in the header was 1, then, as described in E.3.8, preserve the current contents
    //     of the arithmetic coding statistics for the generic region and generic refinement region decoding procedures."
    if (bitmap_coding_context_retained)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot retain bitmap coding context yet");

    segment.symbols = move(result);

    return {};
}

static ErrorOr<void> decode_intermediate_text_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode intermediate text region yet");
}

static ErrorOr<void> decode_immediate_text_region(JBIG2LoadingContext& context, SegmentData const& segment)
{
    // 7.4.3 Text region segment syntax
    auto data = segment.data;
    auto information_field = TRY(decode_region_segment_information_field(data));
    data = data.slice(sizeof(information_field));

    dbgln_if(JBIG2_DEBUG, "Text region: width={}, height={}, x={}, y={}, flags={:#x}", information_field.width, information_field.height, information_field.x_location, information_field.y_location, information_field.flags);

    FixedMemoryStream stream(data);

    // 7.4.3.1.1 Text region segment flags
    u16 text_region_segment_flags = TRY(stream.read_value<BigEndian<u16>>());
    bool uses_huffman_encoding = (text_region_segment_flags & 1) != 0;  // "SBHUFF" in spec.
    bool uses_refinement_coding = (text_region_segment_flags >> 1) & 1; // "SBREFINE" in spec.
    u8 log_strip_size = (text_region_segment_flags >> 2) & 3;           // "LOGSBSTRIPS" in spec.
    u8 strip_size = 1u << log_strip_size;
    u8 reference_corner = (text_region_segment_flags >> 4) & 3;     // "REFCORNER"
    bool is_transposed = (text_region_segment_flags >> 6) & 1;      // "TRANSPOSED" in spec.
    u8 combination_operator = (text_region_segment_flags >> 7) & 3; // "SBCOMBOP" in spec.
    if (combination_operator > 4)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid text region combination operator");

    u8 default_pixel_value = (text_region_segment_flags >> 9) & 1; // "SBDEFPIXEL" in spec.

    u8 delta_s_offset_value = (text_region_segment_flags >> 10) & 0x1f; // "SBDSOFFSET" in spec.
    i8 delta_s_offset = delta_s_offset_value;
    if (delta_s_offset_value & 0x10) {
        // This is converting a 5-bit two's complement number ot i8.
        // FIXME: There's probably a simpler way to do this? Probably just sign-extend by or-ing in the top 3 bits?
        delta_s_offset_value = (~delta_s_offset_value + 1) & 0x1f;
        delta_s_offset = -delta_s_offset_value;
    }

    u8 refinement_template = (text_region_segment_flags >> 15) != 0; // "SBRTEMPLATE" in spec.
    if (!uses_refinement_coding && refinement_template != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid refinement_template");

    // 7.4.3.1.2 Text region segment Huffman flags
    // "This field is only present if SBHUFF is 1."
    // FIXME: Support this eventually.
    if (uses_huffman_encoding)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode huffman text regions yet");

    // 7.4.3.1.3 Text region refinement AT flags
    // "This field is only present if SBREFINE is 1 and SBRTEMPLATE is 0."
    Array<AdaptiveTemplatePixel, 2> adaptive_refinement_template {};
    if (uses_refinement_coding && refinement_template == 0) {
        for (size_t i = 0; i < adaptive_refinement_template.size(); ++i) {
            adaptive_refinement_template[i].x = TRY(stream.read_value<i8>());
            adaptive_refinement_template[i].y = TRY(stream.read_value<i8>());
        }
    }

    // 7.4.3.1.4 Number of symbol instances (SBNUMINSTANCES)
    u32 number_of_symbol_instances = TRY(stream.read_value<BigEndian<u32>>());

    // 7.4.3.1.5 Text region segment symbol ID Huffman decoding table
    // "It is only present if SBHUFF is 1."
    // FIXME: Support this eventually.

    dbgln_if(JBIG2_DEBUG, "Text region: uses_huffman_encoding={}, uses_refinement_coding={}, strip_size={}, reference_corner={}, is_transposed={}", uses_huffman_encoding, uses_refinement_coding, strip_size, reference_corner, is_transposed);
    dbgln_if(JBIG2_DEBUG, "Text region: combination_operator={}, default_pixel_value={}, delta_s_offset={}, refinement_template={}, number_of_symbol_instances={}", combination_operator, default_pixel_value, delta_s_offset, refinement_template, number_of_symbol_instances);
    dbgln_if(JBIG2_DEBUG, "Text region: number_of_symbol_instances={}", number_of_symbol_instances);

    // 7.4.3.2 Decoding a text region segment
    // "1) Interpret its header, as described in 7.4.3.1."
    // Done!

    // "2) Decode (or retrieve the results of decoding) any referred-to symbol dictionary and tables segments."
    Vector<NonnullRefPtr<Symbol>> symbols;
    for (auto referred_to_segment_number : segment.header.referred_to_segment_numbers) {
        auto opt_referred_to_segment = context.segments_by_number.get(referred_to_segment_number);
        if (!opt_referred_to_segment.has_value())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Text segment refers to non-existent segment");
        dbgln_if(JBIG2_DEBUG, "Text segment refers to segment id {} index {}", referred_to_segment_number, opt_referred_to_segment.value());
        auto const& referred_to_segment = context.segments[opt_referred_to_segment.value()];
        if (!referred_to_segment.symbols.has_value())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Text segment referred-to segment without symbols");
        symbols.extend(referred_to_segment.symbols.value());
    }

    // "3) As described in E.3.7, reset all the arithmetic coding statistics to zero."
    // FIXME

    // "4) Invoke the text region decoding procedure described in 6.4, with the parameters to the text region decoding procedure set as shown in Table 34."
    TextRegionDecodingInputParameters inputs;
    inputs.uses_huffman_encoding = uses_huffman_encoding;
    inputs.uses_refinement_coding = uses_refinement_coding;
    inputs.default_pixel = default_pixel_value;
    inputs.operator_ = static_cast<CombinationOperator>(combination_operator);
    inputs.is_transposed = is_transposed;
    inputs.reference_corner = static_cast<TextRegionDecodingInputParameters::Corner>(reference_corner);
    inputs.delta_s_offset = delta_s_offset;
    inputs.region_width = information_field.width;
    inputs.region_height = information_field.height;
    inputs.number_of_instances = number_of_symbol_instances;
    inputs.size_of_symbol_instance_strips = strip_size;
    inputs.id_symbol_code_length = ceil(log2(symbols.size()));
    inputs.symbols = move(symbols);
    // FIXME: Huffman tables.
    inputs.refinement_template = refinement_template;
    inputs.refinement_adaptive_template_pixels = adaptive_refinement_template;

    auto result = TRY(text_region_decoding_procedure(inputs, data.slice(TRY(stream.tell()))));

    composite_bitbuffer(*context.page.bits, *result, { information_field.x_location, information_field.y_location }, information_field.external_combination_operator());

    return {};
}

static ErrorOr<void> decode_pattern_dictionary(JBIG2LoadingContext&, SegmentData& segment)
{
    // 7.4.4 Pattern dictionary segment syntax
    FixedMemoryStream stream(segment.data);

    // 7.4.4.1.1 Pattern dictionary flags
    u8 flags = TRY(stream.read_value<u8>());
    bool uses_mmr = flags & 1;
    u8 hd_template = (flags >> 1) & 3;
    if (uses_mmr && hd_template != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid hd_template");
    if (flags & 0b1111'1000)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid flags");

    // 7.4.4.1.2 Width of the patterns in the pattern dictionary (HDPW)
    u8 width = TRY(stream.read_value<u8>());
    if (width == 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid width");

    // 7.4.4.1.3 Height of the patterns in the pattern dictionary (HDPH)
    u8 height = TRY(stream.read_value<u8>());
    if (height == 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid height");

    // 7.4.4.1.4 Largest gray-scale value (GRAYMAX)
    u32 gray_max = TRY(stream.read_value<BigEndian<u32>>());

    // 7.4.4.2 Decoding a pattern dictionary segment
    dbgln_if(JBIG2_DEBUG, "Pattern dictionary: uses_mmr={}, hd_template={}, width={}, height={}, gray_max={}", uses_mmr, hd_template, width, height, gray_max);
    auto data = segment.data.slice(TRY(stream.tell()));

    // "1) Interpret its header, as described in 7.4.4.1."
    // Done!

    // "2) As described in E.3.7, reset all the arithmetic coding statistics to zero."
    Vector<QMArithmeticDecoder::Context> contexts;
    if (!uses_mmr)
        contexts.resize(1 << number_of_context_bits_for_template(hd_template));

    // "3) Invoke the pattern dictionary decoding procedure described in 6.7, with the parameters to the pattern
    //     dictionary decoding procedure set as shown in Table 35."
    PatternDictionaryDecodingInputParameters inputs;
    inputs.uses_mmr = uses_mmr;
    inputs.width = width;
    inputs.height = height;
    inputs.gray_max = gray_max;
    inputs.hd_template = hd_template;
    auto result = TRY(pattern_dictionary_decoding_procedure(inputs, data, contexts));

    segment.patterns = move(result);

    return {};
}

static ErrorOr<void> decode_intermediate_halftone_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode intermediate halftone region yet");
}

static ErrorOr<void> decode_immediate_halftone_region(JBIG2LoadingContext& context, SegmentData const& segment)
{
    // 7.4.5 Halftone region segment syntax
    auto data = segment.data;
    auto information_field = TRY(decode_region_segment_information_field(data));
    data = data.slice(sizeof(information_field));

    dbgln_if(JBIG2_DEBUG, "Halftone region: width={}, height={}, x={}, y={}, flags={:#x}", information_field.width, information_field.height, information_field.x_location, information_field.y_location, information_field.flags);

    FixedMemoryStream stream(data);

    // 7.4.5.1.1 Halftone region segment flags
    u8 flags = TRY(stream.read_value<u8>());
    bool uses_mmr = flags & 1;           // "HMMR" in spec.
    u8 template_used = (flags >> 1) & 3; // "HTTEMPLATE" in spec.
    if (uses_mmr && template_used != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid template_used");
    bool enable_skip = (flags >> 3) & 1;        // "HENABLESKIP" in spec.
    u8 combination_operator = (flags >> 4) & 7; // "HCOMBOP" in spec.
    if (combination_operator > 4)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid combination_operator");
    bool default_pixel_value = (flags >> 7) & 1; // "HDEFPIXEL" in spec.

    dbgln_if(JBIG2_DEBUG, "Halftone region: uses_mmr={}, template_used={}, enable_skip={}, combination_operator={}, default_pixel_value={}", uses_mmr, template_used, enable_skip, combination_operator, default_pixel_value);

    // 7.4.5.1.2 Halftone grid position and size
    // 7.4.5.1.2.1 Width of the gray-scale image (HGW)
    u32 gray_width = TRY(stream.read_value<BigEndian<u32>>());

    // 7.4.5.1.2.2 Height of the gray-scale image (HGH)
    u32 gray_height = TRY(stream.read_value<BigEndian<u32>>());

    // 7.4.5.1.2.3 Horizontal offset of the grid (HGX)
    i32 grid_x = TRY(stream.read_value<BigEndian<i32>>());

    // 7.4.5.1.2.4 Vertical offset of the grid (HGY)
    i32 grid_y = TRY(stream.read_value<BigEndian<i32>>());

    // 7.4.5.1.3 Halftone grid vector
    // 7.4.5.1.3.1 Horizontal coordinate of the halftone grid vector (HRX)
    u16 grid_vector_x = TRY(stream.read_value<BigEndian<u16>>());

    // 7.4.5.1.3.2 Vertical coordinate of the halftone grid vector (HRY)
    u16 grid_vector_y = TRY(stream.read_value<BigEndian<u16>>());

    dbgln_if(JBIG2_DEBUG, "Halftone region: gray_width={}, gray_height={}, grid_x={}, grid_y={}, grid_vector_x={}, grid_vector_y={}", gray_width, gray_height, grid_x, grid_y, grid_vector_x, grid_vector_y);

    // 7.4.5.2 Decoding a halftone region segment
    // "1) Interpret its header, as described in 7.4.5.1."
    // Done!

    // "2) Decode (or retrieve the results of decoding) the referred-to pattern dictionary segment."
    if (segment.header.referred_to_segment_numbers.size() != 1)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Halftone segment refers to wrong number of segments");
    auto opt_referred_to_segment = context.segments_by_number.get(segment.header.referred_to_segment_numbers[0]);
    if (!opt_referred_to_segment.has_value())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Halftone segment refers to non-existent segment");
    dbgln_if(JBIG2_DEBUG, "Halftone segment refers to segment id {} index {}", segment.header.referred_to_segment_numbers[0], opt_referred_to_segment.value());
    auto const& referred_to_segment = context.segments[opt_referred_to_segment.value()];
    if (!referred_to_segment.patterns.has_value())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Halftone segment referred-to segment without patterns");
    Vector<NonnullRefPtr<Symbol>> patterns = referred_to_segment.patterns.value();
    if (patterns.is_empty())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Halftone segment without patterns");

    // "3) As described in E.3.7, reset all the arithmetic coding statistics to zero."
    Vector<QMArithmeticDecoder::Context> contexts;
    if (!uses_mmr)
        contexts.resize(1 << number_of_context_bits_for_template(template_used));

    // "4) Invoke the halftone region decoding procedure described in 6.6, with the parameters to the halftone
    //     region decoding procedure set as shown in Table 36."
    data = data.slice(TRY(stream.tell()));
    HalftoneRegionDecodingInputParameters inputs;
    inputs.region_width = information_field.width;
    inputs.region_height = information_field.height;
    inputs.uses_mmr = uses_mmr;
    inputs.halftone_template = template_used;
    inputs.enable_skip = enable_skip;
    inputs.combination_operator = static_cast<CombinationOperator>(combination_operator);
    inputs.default_pixel_value = default_pixel_value;
    inputs.grayscale_width = gray_width;
    inputs.grayscale_height = gray_height;
    inputs.grid_origin_x_offset = grid_x;
    inputs.grid_origin_y_offset = grid_y;
    inputs.grid_vector_x = grid_vector_x;
    inputs.grid_vector_y = grid_vector_y;
    inputs.patterns = move(patterns);
    inputs.pattern_width = inputs.patterns[0]->bitmap().width();
    inputs.pattern_height = inputs.patterns[0]->bitmap().height();
    auto result = TRY(halftone_region_decoding_procedure(inputs, data, contexts));

    composite_bitbuffer(*context.page.bits, *result, { information_field.x_location, information_field.y_location }, information_field.external_combination_operator());

    return {};
}

static ErrorOr<void> decode_immediate_lossless_halftone_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode immediate lossless halftone region yet");
}

static ErrorOr<void> decode_intermediate_generic_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode intermediate generic region yet");
}

static ErrorOr<void> decode_immediate_generic_region(JBIG2LoadingContext& context, SegmentData const& segment)
{
    // 7.4.6 Generic region segment syntax
    auto data = segment.data;
    auto information_field = TRY(decode_region_segment_information_field(data));
    data = data.slice(sizeof(information_field));

    dbgln_if(JBIG2_DEBUG, "Generic region: width={}, height={}, x={}, y={}, flags={:#x}", information_field.width, information_field.height, information_field.x_location, information_field.y_location, information_field.flags);

    // 7.4.6.2 Generic region segment flags
    if (data.is_empty())
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: No segment data");
    u8 flags = data[0];
    bool uses_mmr = (flags & 1) != 0;
    u8 arithmetic_coding_template = (flags >> 1) & 3;               // "GBTEMPLATE"
    bool typical_prediction_generic_decoding_on = (flags >> 3) & 1; // "TPGDON"; "TPGD" is short for "Typical Prediction for Generic Direct coding"
    bool uses_extended_reference_template = (flags >> 4) & 1;       // "EXTTEMPLATE"
    if (flags & 0b1110'0000)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid flags");
    data = data.slice(sizeof(flags));

    // 7.4.6.3 Generic region segment AT flags
    Array<AdaptiveTemplatePixel, 12> adaptive_template_pixels {};
    if (!uses_mmr) {
        dbgln_if(JBIG2_DEBUG, "Non-MMR generic region, GBTEMPLATE={} TPGDON={} EXTTEMPLATE={}", arithmetic_coding_template, typical_prediction_generic_decoding_on, uses_extended_reference_template);

        if (arithmetic_coding_template == 0 && uses_extended_reference_template) {
            // This was added in T.88 Amendment 2 (https://www.itu.int/rec/T-REC-T.88-200306-S!Amd2/en) mid-2003.
            // I haven't seen it being used in the wild, and the spec says "32-byte field as shown below" and then shows 24 bytes,
            // so it's not clear how much data to read.
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: GBTEMPLATE=0 EXTTEMPLATE=1 not yet implemented");
        }

        size_t number_of_adaptive_template_pixels = arithmetic_coding_template == 0 ? 4 : 1;
        if (data.size() < 2 * number_of_adaptive_template_pixels)
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: No adaptive template data");
        for (size_t i = 0; i < number_of_adaptive_template_pixels; ++i) {
            adaptive_template_pixels[i].x = static_cast<i8>(data[2 * i]);
            adaptive_template_pixels[i].y = static_cast<i8>(data[2 * i + 1]);
        }
        data = data.slice(2 * number_of_adaptive_template_pixels);
    }

    // 7.4.6.4 Decoding a generic region segment
    // "1) Interpret its header, as described in 7.4.6.1"
    // Done above.
    // "2) As described in E.3.7, reset all the arithmetic coding statistics to zero."
    Vector<QMArithmeticDecoder::Context> contexts;
    contexts.resize(1 << number_of_context_bits_for_template(arithmetic_coding_template));

    // "3) Invoke the generic region decoding procedure described in 6.2, with the parameters to the generic region decoding procedure set as shown in Table 37."
    GenericRegionDecodingInputParameters inputs;
    inputs.is_modified_modified_read = uses_mmr;
    inputs.region_width = information_field.width;
    inputs.region_height = information_field.height;
    inputs.gb_template = arithmetic_coding_template;
    inputs.is_typical_prediction_used = typical_prediction_generic_decoding_on;
    inputs.is_extended_reference_template_used = uses_extended_reference_template;
    inputs.skip_pattern = OptionalNone {};
    inputs.adaptive_template_pixels = adaptive_template_pixels;

    Optional<QMArithmeticDecoder> decoder;
    if (!uses_mmr) {
        decoder = TRY(QMArithmeticDecoder::initialize(data));
        inputs.arithmetic_decoder = &decoder.value();
    }

    auto result = TRY(generic_region_decoding_procedure(inputs, data, contexts));

    // 8.2 Page image composition step 5)
    if (information_field.x_location + information_field.width > (u32)context.page.size.width()
        || information_field.y_location + information_field.height > (u32)context.page.size.height()) {
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Region bounds outsize of page bounds");
    }

    composite_bitbuffer(*context.page.bits, *result, { information_field.x_location, information_field.y_location }, information_field.external_combination_operator());

    return {};
}

static ErrorOr<void> decode_intermediate_generic_refinement_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode intermediate generic refinement region yet");
}

static ErrorOr<void> decode_immediate_generic_refinement_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode immediate generic refinement region yet");
}

static ErrorOr<void> decode_immediate_lossless_generic_refinement_region(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode immediate lossless generic refinement region yet");
}

static ErrorOr<void> decode_page_information(JBIG2LoadingContext& context, SegmentData const& segment)
{
    // 7.4.8 Page information segment syntax and 8.1 Decoder model steps 1) - 3).

    // "1) Decode the page information segment.""
    auto page_information = TRY(decode_page_information_segment(segment.data));

    bool page_is_striped = (page_information.striping_information & 0x80) != 0;
    if (page_information.bitmap_height == 0xffff'ffff && !page_is_striped)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Non-striped bitmaps of indeterminate height not allowed");

    u16 maximum_stripe_height = page_information.striping_information & 0x7F;
    u8 default_color = (page_information.flags >> 2) & 1;
    u8 default_combination_operator = (page_information.flags >> 3) & 3;
    context.page.default_combination_operator = static_cast<CombinationOperator>(default_combination_operator);

    dbgln_if(JBIG2_DEBUG, "Page information: width={}, height={}, is_striped={}, max_stripe_height={}, default_color={}, default_combination_operator={}", page_information.bitmap_width, page_information.bitmap_height, page_is_striped, maximum_stripe_height, default_color, default_combination_operator);

    // FIXME: Do something with the other fields in page_information.

    // "2) Create the page buffer, of the size given in the page information segment.
    //
    //     If the page height is unknown, then this is not possible. However, in this case the page must be striped,
    //     and the maximum stripe height specified, and the initial page buffer can be created with height initially
    //     equal to this maximum stripe height."
    size_t height = page_information.bitmap_height;
    if (height == 0xffff'ffff)
        height = maximum_stripe_height;
    context.page.bits = TRY(BitBuffer::create(page_information.bitmap_width, height));

    // "3) Fill the page buffer with the page's default pixel value."
    context.page.bits->fill(default_color != 0);

    return {};
}

static ErrorOr<void> decode_end_of_page(JBIG2LoadingContext&, SegmentData const& segment)
{
    // 7.4.9 End of page segment syntax
    if (segment.data.size() != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: End of page segment has non-zero size");
    // FIXME: If the page had unknown height, check that previous segment was end-of-stripe.
    // FIXME: Maybe mark page as completed and error if we see more segments for it?
    return {};
}

static ErrorOr<void> decode_end_of_stripe(JBIG2LoadingContext&, SegmentData const& segment)
{
    // 7.4.10 End of stripe segment syntax
    // "The segment data of an end of stripe segment consists of one four-byte value, specifying the Y coordinate of the end row."
    if (segment.data.size() != 4)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: End of strip segment has wrong size");

    // FIXME: Once we implement support for images with initially indeterminate height, we need these values to determine the height at the end.
    u32 y_coordinate = *reinterpret_cast<BigEndian<u32> const*>(segment.data.data());
    dbgln_if(JBIG2_DEBUG, "End of stripe: y={}", y_coordinate);

    return {};
}

static ErrorOr<void> decode_end_of_file(JBIG2LoadingContext&, SegmentData const& segment)
{
    // 7.4.11 End of file segment syntax
    if (segment.data.size() != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: End of file segment has non-zero size");
    return {};
}

static ErrorOr<void> decode_profiles(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode profiles yet");
}

static ErrorOr<void> decode_tables(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode tables yet");
}

static ErrorOr<void> decode_color_palette(JBIG2LoadingContext&, SegmentData const&)
{
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Cannot decode color palette yet");
}

static ErrorOr<void> decode_extension(JBIG2LoadingContext&, SegmentData const& segment)
{
    // 7.4.14 Extension segment syntax
    FixedMemoryStream stream { segment.data };

    enum ExtensionType {
        SingleByteCodedComment = 0x20000000,
        MultiByteCodedComment = 0x20000002,
    };
    u32 type = TRY(stream.read_value<BigEndian<u32>>());

    auto read_string = [&]<class T>() -> ErrorOr<Vector<T>> {
        Vector<T> result;
        do {
            result.append(TRY(stream.read_value<BigEndian<T>>()));
        } while (result.last());
        result.take_last();
        return result;
    };

    switch (type) {
    case SingleByteCodedComment: {
        // 7.4.15.1 Single-byte coded comment
        // Pairs of zero-terminated ISO/IEC 8859-1 (latin1) pairs, terminated by another \0.
        while (true) {
            auto first_bytes = TRY(read_string.template operator()<u8>());
            if (first_bytes.is_empty())
                break;

            auto second_bytes = TRY(read_string.template operator()<u8>());

            auto first = TRY(TextCodec::decoder_for_exact_name("ISO-8859-1"sv)->to_utf8(StringView { first_bytes }));
            auto second = TRY(TextCodec::decoder_for_exact_name("ISO-8859-1"sv)->to_utf8(StringView { second_bytes }));
            dbgln("JBIG2ImageDecoderPlugin: key '{}', value '{}'", first, second);
        }
        if (!stream.is_eof())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Trailing data after SingleByteCodedComment");
        return {};
    }
    case MultiByteCodedComment: {
        // 7.4.15.2 Multi-byte coded comment
        // Pairs of (two-byte-)zero-terminated UCS-2 pairs, terminated by another \0\0.
        while (true) {
            auto first_ucs2 = TRY(read_string.template operator()<u16>());
            if (first_ucs2.is_empty())
                break;

            auto second_ucs2 = TRY(read_string.template operator()<u16>());

            auto first = TRY(Utf16View(first_ucs2).to_utf8());
            auto second = TRY(Utf16View(second_ucs2).to_utf8());
            dbgln("JBIG2ImageDecoderPlugin: key '{}', value '{}'", first, second);
        }
        if (!stream.is_eof())
            return Error::from_string_literal("JBIG2ImageDecoderPlugin: Trailing data after MultiByteCodedComment");
        return {};
    }
    }

    // FIXME: If bit 31 in `type` is not set, the extension isn't necessary, and we could ignore it.
    dbgln("JBIG2ImageDecoderPlugin: Unknown extension type {:#x}", type);
    return Error::from_string_literal("JBIG2ImageDecoderPlugin: Unknown extension type");
}

static ErrorOr<void> decode_data(JBIG2LoadingContext& context)
{
    TRY(warn_about_multiple_pages(context));

    for (size_t i = 0; i < context.segments.size(); ++i) {
        auto& segment = context.segments[i];

        if (segment.header.page_association != 0 && segment.header.page_association != 1)
            continue;

        switch (segment.header.type) {
        case SegmentType::SymbolDictionary:
            TRY(decode_symbol_dictionary(context, segment));
            break;
        case SegmentType::IntermediateTextRegion:
            TRY(decode_intermediate_text_region(context, segment));
            break;
        case SegmentType::ImmediateTextRegion:
        case SegmentType::ImmediateLosslessTextRegion:
            // 7.4.3 Text region segment syntax
            // "The data parts of all three of the text region segment types ("intermediate text region", "immediate text region" and
            //  "immediate lossless text region") are coded identically, but are acted upon differently, see 8.2."
            // But 8.2 only describes a difference between intermediate and immediate regions as far as I can tell,
            // and calling the immediate text region handler for immediate lossless text regions seems to do the right thing (?).
            TRY(decode_immediate_text_region(context, segment));
            break;
        case SegmentType::PatternDictionary:
            TRY(decode_pattern_dictionary(context, segment));
            break;
        case SegmentType::IntermediateHalftoneRegion:
            TRY(decode_intermediate_halftone_region(context, segment));
            break;
        case SegmentType::ImmediateHalftoneRegion:
            TRY(decode_immediate_halftone_region(context, segment));
            break;
        case SegmentType::ImmediateLosslessHalftoneRegion:
            TRY(decode_immediate_lossless_halftone_region(context, segment));
            break;
        case SegmentType::IntermediateGenericRegion:
            TRY(decode_intermediate_generic_region(context, segment));
            break;
        case SegmentType::ImmediateGenericRegion:
        case SegmentType::ImmediateLosslessGenericRegion:
            // 7.4.6 Generic region segment syntax
            // "The data parts of all three of the generic region segment types ("intermediate generic region", "immediate generic region" and
            //  "immediate lossless generic region") are coded identically, but are acted upon differently, see 8.2."
            // But 8.2 only describes a difference between intermediate and immediate regions as far as I can tell,
            // and calling the immediate generic region handler for immediate generic lossless regions seems to do the right thing (?).
            TRY(decode_immediate_generic_region(context, segment));
            break;
        case SegmentType::IntermediateGenericRefinementRegion:
            TRY(decode_intermediate_generic_refinement_region(context, segment));
            break;
        case SegmentType::ImmediateGenericRefinementRegion:
            TRY(decode_immediate_generic_refinement_region(context, segment));
            break;
        case SegmentType::ImmediateLosslessGenericRefinementRegion:
            TRY(decode_immediate_lossless_generic_refinement_region(context, segment));
            break;
        case SegmentType::PageInformation:
            TRY(decode_page_information(context, segment));
            break;
        case SegmentType::EndOfPage:
            TRY(decode_end_of_page(context, segment));
            break;
        case SegmentType::EndOfStripe:
            TRY(decode_end_of_stripe(context, segment));
            break;
        case SegmentType::EndOfFile:
            TRY(decode_end_of_file(context, segment));
            // "If a file contains an end of file segment, it must be the last segment."
            if (i != context.segments.size() - 1)
                return Error::from_string_literal("JBIG2ImageDecoderPlugin: End of file segment not last segment");
            break;
        case SegmentType::Profiles:
            TRY(decode_profiles(context, segment));
            break;
        case SegmentType::Tables:
            TRY(decode_tables(context, segment));
            break;
        case SegmentType::ColorPalette:
            TRY(decode_color_palette(context, segment));
            break;
        case SegmentType::Extension:
            TRY(decode_extension(context, segment));
            break;
        }
    }

    return {};
}

JBIG2ImageDecoderPlugin::JBIG2ImageDecoderPlugin()
{
    m_context = make<JBIG2LoadingContext>();
}

JBIG2ImageDecoderPlugin::~JBIG2ImageDecoderPlugin() = default;

IntSize JBIG2ImageDecoderPlugin::size()
{
    return m_context->page.size;
}

bool JBIG2ImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return data.starts_with(id_string);
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JBIG2ImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JBIG2ImageDecoderPlugin()));
    TRY(decode_jbig2_header(*plugin->m_context, data));

    data = data.slice(sizeof(id_string) + sizeof(u8) + (plugin->m_context->number_of_pages.has_value() ? sizeof(u32) : 0));
    TRY(decode_segment_headers(*plugin->m_context, data));

    TRY(scan_for_page_size(*plugin->m_context));

    return plugin;
}

ErrorOr<ImageFrameDescriptor> JBIG2ImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    // FIXME: Use this for multi-page JBIG2 files?
    if (index != 0)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Invalid frame index");

    if (m_context->state == JBIG2LoadingContext::State::Error)
        return Error::from_string_literal("JBIG2ImageDecoderPlugin: Decoding failed");

    if (m_context->state < JBIG2LoadingContext::State::Decoded) {
        auto result = decode_data(*m_context);
        if (result.is_error()) {
            m_context->state = JBIG2LoadingContext::State::Error;
            return result.release_error();
        }
        m_context->state = JBIG2LoadingContext::State::Decoded;
    }

    auto bitmap = TRY(m_context->page.bits->to_gfx_bitmap());
    return ImageFrameDescriptor { move(bitmap), 0 };
}

ErrorOr<ByteBuffer> JBIG2ImageDecoderPlugin::decode_embedded(Vector<ReadonlyBytes> data)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) JBIG2ImageDecoderPlugin()));
    plugin->m_context->organization = Organization::Embedded;

    for (auto const& segment_data : data)
        TRY(decode_segment_headers(*plugin->m_context, segment_data));

    TRY(scan_for_page_size(*plugin->m_context));
    TRY(decode_data(*plugin->m_context));

    return plugin->m_context->page.bits->to_byte_buffer();
}

}
