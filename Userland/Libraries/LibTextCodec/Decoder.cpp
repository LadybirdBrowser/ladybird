/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/LookupTables.h>

namespace TextCodec {

static constexpr u32 replacement_code_point = 0xfffd;

namespace {
Latin1Decoder s_latin1_decoder;
UTF8Decoder s_utf8_decoder;
UTF16BEDecoder s_utf16be_decoder;
UTF16LEDecoder s_utf16le_decoder;
Latin2Decoder s_latin2_decoder;
Latin9Decoder s_latin9_decoder;
PDFDocEncodingDecoder s_pdf_doc_encoding_decoder;
TurkishDecoder s_turkish_decoder;
XUserDefinedDecoder s_x_user_defined_decoder;
GB18030Decoder s_gb18030_decoder;
Big5Decoder s_big5_decoder;
EUCJPDecoder s_euc_jp_decoder;
ISO2022JPDecoder s_iso_2022_jp_decoder;
ShiftJISDecoder s_shift_jis_decoder;
EUCKRDecoder s_euc_kr_decoder;
ReplacementDecoder s_replacement_decoder;

// clang-format off
// https://encoding.spec.whatwg.org/index-ibm866.txt
SingleByteDecoder s_ibm866_decoder {{
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
    0x0401, 0x0451, 0x0404, 0x0454, 0x0407, 0x0457, 0x040E, 0x045E, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x2116, 0x00A4, 0x25A0, 0x00A0,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-3.txt
SingleByteDecoder s_latin3_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0126, 0x02D8, 0x00A3, 0x00A4, 0xFFFD, 0x0124, 0x00A7, 0x00A8, 0x0130, 0x015E, 0x011E, 0x0134, 0x00AD, 0xFFFD, 0x017B,
    0x00B0, 0x0127, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x0125, 0x00B7, 0x00B8, 0x0131, 0x015F, 0x011F, 0x0135, 0x00BD, 0xFFFD, 0x017C,
    0x00C0, 0x00C1, 0x00C2, 0xFFFD, 0x00C4, 0x010A, 0x0108, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0xFFFD, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x0120, 0x00D6, 0x00D7, 0x011C, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x016C, 0x015C, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0xFFFD, 0x00E4, 0x010B, 0x0109, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0xFFFD, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x0121, 0x00F6, 0x00F7, 0x011D, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x016D, 0x015D, 0x02D9,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-4.txt
SingleByteDecoder s_latin4_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0104, 0x0138, 0x0156, 0x00A4, 0x0128, 0x013B, 0x00A7, 0x00A8, 0x0160, 0x0112, 0x0122, 0x0166, 0x00AD, 0x017D, 0x00AF,
    0x00B0, 0x0105, 0x02DB, 0x0157, 0x00B4, 0x0129, 0x013C, 0x02C7, 0x00B8, 0x0161, 0x0113, 0x0123, 0x0167, 0x014A, 0x017E, 0x014B,
    0x0100, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x012E, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x0116, 0x00CD, 0x00CE, 0x012A,
    0x0110, 0x0145, 0x014C, 0x0136, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x0172, 0x00DA, 0x00DB, 0x00DC, 0x0168, 0x016A, 0x00DF,
    0x0101, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x012F, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x0117, 0x00ED, 0x00EE, 0x012B,
    0x0111, 0x0146, 0x014D, 0x0137, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x0173, 0x00FA, 0x00FB, 0x00FC, 0x0169, 0x016B, 0x02D9,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-5.txt
SingleByteDecoder s_latin_cyrillic_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x0409, 0x040A, 0x040B, 0x040C, 0x00AD, 0x040E, 0x040F,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
    0x2116, 0x0451, 0x0452, 0x0453, 0x0454, 0x0455, 0x0456, 0x0457, 0x0458, 0x0459, 0x045A, 0x045B, 0x045C, 0x00A7, 0x045E, 0x045F,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-6.txt
SingleByteDecoder s_latin_arabic_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0xFFFD, 0xFFFD, 0xFFFD, 0x00A4, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x060C, 0x00AD, 0xFFFD, 0xFFFD,
    0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x061B, 0xFFFD, 0xFFFD, 0xFFFD, 0x061F,
    0xFFFD, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627, 0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F,
    0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x0637, 0x0638, 0x0639, 0x063A, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
    0x0640, 0x0641, 0x0642, 0x0643, 0x0644, 0x0645, 0x0646, 0x0647, 0x0648, 0x0649, 0x064A, 0x064B, 0x064C, 0x064D, 0x064E, 0x064F,
    0x0650, 0x0651, 0x0652, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-7.txt
SingleByteDecoder s_latin_greek_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x2018, 0x2019, 0x00A3, 0x20AC, 0x20AF, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x037A, 0x00AB, 0x00AC, 0x00AD, 0xFFFD, 0x2015,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x0384, 0x0385, 0x0386, 0x00B7, 0x0388, 0x0389, 0x038A, 0x00BB, 0x038C, 0x00BD, 0x038E, 0x038F,
    0x0390, 0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F,
    0x03A0, 0x03A1, 0xFFFD, 0x03A3, 0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9, 0x03AA, 0x03AB, 0x03AC, 0x03AD, 0x03AE, 0x03AF,
    0x03B0, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF,
    0x03C0, 0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x03CA, 0x03CB, 0x03CC, 0x03CD, 0x03CE, 0xFFFD,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-8.txt
SingleByteDecoder s_latin_hebrew_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0xFFFD, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00D7, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00F7, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0xFFFD,
    0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
    0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x2017,
    0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7, 0x05D8, 0x05D9, 0x05DA, 0x05DB, 0x05DC, 0x05DD, 0x05DE, 0x05DF,
    0x05E0, 0x05E1, 0x05E2, 0x05E3, 0x05E4, 0x05E5, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, 0xFFFD, 0xFFFD, 0x200E, 0x200F, 0xFFFD,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-10.txt
SingleByteDecoder s_latin6_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0104, 0x0112, 0x0122, 0x012A, 0x0128, 0x0136, 0x00A7, 0x013B, 0x0110, 0x0160, 0x0166, 0x017D, 0x00AD, 0x016A, 0x014A,
    0x00B0, 0x0105, 0x0113, 0x0123, 0x012B, 0x0129, 0x0137, 0x00B7, 0x013C, 0x0111, 0x0161, 0x0167, 0x017E, 0x2015, 0x016B, 0x014B,
    0x0100, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x012E, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x0116, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x0145, 0x014C, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x0168, 0x00D8, 0x0172, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x0101, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x012F, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x0117, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x0146, 0x014D, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x0169, 0x00F8, 0x0173, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x0138,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-13.txt
SingleByteDecoder s_latin7_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x201D, 0x00A2, 0x00A3, 0x00A4, 0x201E, 0x00A6, 0x00A7, 0x00D8, 0x00A9, 0x0156, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00C6,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x201C, 0x00B5, 0x00B6, 0x00B7, 0x00F8, 0x00B9, 0x0157, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00E6,
    0x0104, 0x012E, 0x0100, 0x0106, 0x00C4, 0x00C5, 0x0118, 0x0112, 0x010C, 0x00C9, 0x0179, 0x0116, 0x0122, 0x0136, 0x012A, 0x013B,
    0x0160, 0x0143, 0x0145, 0x00D3, 0x014C, 0x00D5, 0x00D6, 0x00D7, 0x0172, 0x0141, 0x015A, 0x016A, 0x00DC, 0x017B, 0x017D, 0x00DF,
    0x0105, 0x012F, 0x0101, 0x0107, 0x00E4, 0x00E5, 0x0119, 0x0113, 0x010D, 0x00E9, 0x017A, 0x0117, 0x0123, 0x0137, 0x012B, 0x013C,
    0x0161, 0x0144, 0x0146, 0x00F3, 0x014D, 0x00F5, 0x00F6, 0x00F7, 0x0173, 0x0142, 0x015B, 0x016B, 0x00FC, 0x017C, 0x017E, 0x2019,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-14.txt
SingleByteDecoder s_latin8_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x1E02, 0x1E03, 0x00A3, 0x010A, 0x010B, 0x1E0A, 0x00A7, 0x1E80, 0x00A9, 0x1E82, 0x1E0B, 0x1EF2, 0x00AD, 0x00AE, 0x0178,
    0x1E1E, 0x1E1F, 0x0120, 0x0121, 0x1E40, 0x1E41, 0x00B6, 0x1E56, 0x1E81, 0x1E57, 0x1E83, 0x1E60, 0x1EF3, 0x1E84, 0x1E85, 0x1E61,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x0174, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x1E6A, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x0176, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x0175, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x1E6B, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x0177, 0x00FF,
}};
// https://encoding.spec.whatwg.org/index-iso-8859-16.txt
SingleByteDecoder s_latin10_decoder {{
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0104, 0x0105, 0x0141, 0x20AC, 0x201E, 0x0160, 0x00A7, 0x0161, 0x00A9, 0x0218, 0x00AB, 0x0179, 0x00AD, 0x017A, 0x017B,
    0x00B0, 0x00B1, 0x010C, 0x0142, 0x017D, 0x201D, 0x00B6, 0x00B7, 0x017E, 0x010D, 0x0219, 0x00BB, 0x0152, 0x0153, 0x0178, 0x017C,
    0x00C0, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0106, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x0110, 0x0143, 0x00D2, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x015A, 0x0170, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x0118, 0x021A, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x0107, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x0111, 0x0144, 0x00F2, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x015B, 0x0171, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x0119, 0x021B, 0x00FF,
}};
// https://encoding.spec.whatwg.org/index-windows-1250.txt
SingleByteDecoder s_centraleurope_decoder {{
    0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021, 0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7, 0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7, 0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
}};
// https://encoding.spec.whatwg.org/index-windows-1251.txt
SingleByteDecoder s_cyrillic_decoder {{
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
}};
// https://encoding.spec.whatwg.org/index-windows-1255.txt
SingleByteDecoder s_hebrew_decoder {{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x008A, 0x2039, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x009A, 0x203A, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x20AA, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00D7, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00F7, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x05B0, 0x05B1, 0x05B2, 0x05B3, 0x05B4, 0x05B5, 0x05B6, 0x05B7, 0x05B8, 0x05B9, 0x05BA, 0x05BB, 0x05BC, 0x05BD, 0x05BE, 0x05BF,
    0x05C0, 0x05C1, 0x05C2, 0x05C3, 0x05F0, 0x05F1, 0x05F2, 0x05F3, 0x05F4, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
    0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7, 0x05D8, 0x05D9, 0x05DA, 0x05DB, 0x05DC, 0x05DD, 0x05DE, 0x05DF,
    0x05E0, 0x05E1, 0x05E2, 0x05E3, 0x05E4, 0x05E5, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, 0xFFFD, 0xFFFD, 0x200E, 0x200F, 0xFFFD,
}};
// https://encoding.spec.whatwg.org/index-koi8-r.txt
SingleByteDecoder s_koi8r_decoder {{
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248, 0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,
    0x2550, 0x2551, 0x2552, 0xD191, 0x2553, 0x2554, 0x2555, 0x2556, 0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,
    0x255F, 0x2560, 0x2561, 0xD081, 0x2562, 0x2563, 0x2564, 0x2565, 0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433, 0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432, 0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0441, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413, 0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412, 0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,
}};
// https://encoding.spec.whatwg.org/index-koi8-u.txt
SingleByteDecoder s_koi8u_decoder {{
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248, 0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,
    0x2550, 0x2551, 0x2552, 0x0451, 0x0454, 0x2554, 0x0456, 0x0457, 0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x0491, 0x045E, 0x255E,
    0x255F, 0x2560, 0x2561, 0x0401, 0x0404, 0x2563, 0x0406, 0x0407, 0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x0490, 0x040E, 0x00A9,
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433, 0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432, 0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413, 0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412, 0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,
}};
// https://encoding.spec.whatwg.org/index-macintosh.txt
SingleByteDecoder s_mac_roman_decoder {{
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
}};
// https://encoding.spec.whatwg.org/index-windows-874.txt
SingleByteDecoder s_windows874_decoder {{
    0x20AC, 0x0081, 0x0082, 0x0083, 0x0084, 0x2026, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0E01, 0x0E02, 0x0E03, 0x0E04, 0x0E05, 0x0E06, 0x0E07, 0x0E08, 0x0E09, 0x0E0A, 0x0E0B, 0x0E0C, 0x0E0D, 0x0E0E, 0x0E0F,
    0x0E10, 0x0E11, 0x0E12, 0x0E13, 0x0E14, 0x0E15, 0x0E16, 0x0E17, 0x0E18, 0x0E19, 0x0E1A, 0x0E1B, 0x0E1C, 0x0E1D, 0x0E1E, 0x0E1F,
    0x0E20, 0x0E21, 0x0E22, 0x0E23, 0x0E24, 0x0E25, 0x0E26, 0x0E27, 0x0E28, 0x0E29, 0x0E2A, 0x0E2B, 0x0E2C, 0x0E2D, 0x0E2E, 0x0E2F,
    0x0E30, 0x0E31, 0x0E32, 0x0E33, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E38, 0x0E39, 0x0E3A, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x0E3F,
    0x0E40, 0x0E41, 0x0E42, 0x0E43, 0x0E44, 0x0E45, 0x0E46, 0x0E47, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E4D, 0x0E4E, 0x0E4F,
    0x0E50, 0x0E51, 0x0E52, 0x0E53, 0x0E54, 0x0E55, 0x0E56, 0x0E57, 0x0E58, 0x0E59, 0x0E5A, 0x0E5B, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
}};
// https://encoding.spec.whatwg.org/index-windows-1253.txt
SingleByteDecoder s_windows1253_decoder {{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x0088, 0x2030, 0x008A, 0x2039, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x009A, 0x203A, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0385, 0x0386, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0xFFFD, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x2015,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x0384, 0x00B5, 0x00B6, 0x00B7, 0x0388, 0x0389, 0x038A, 0x00BB, 0x038C, 0x00BD, 0x038E, 0x038F,
    0x0390, 0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F,
    0x03A0, 0x03A1, 0xFFFD, 0x03A3, 0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9, 0x03AA, 0x03AB, 0x03AC, 0x03AD, 0x03AE, 0x03AF,
    0x03B0, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF,
    0x03C0, 0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x03CA, 0x03CB, 0x03CC, 0x03CD, 0x03CE, 0xFFFD,
}};
// https://encoding.spec.whatwg.org/index-windows-1256.txt
SingleByteDecoder s_windows1256_decoder {{
    0x20AC, 0x067E, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0679, 0x2039, 0x0152, 0x0686, 0x0698, 0x0688,
    0x06AF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x06A9, 0x2122, 0x0691, 0x203A, 0x0153, 0x200C, 0x200D, 0x06BA,
    0x00A0, 0x060C, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x06BE, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x061B, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x061F,
    0x06C1, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627, 0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F,
    0x0630, 0x0631, 0x0632, 0x0633, 0x0634, 0x0635, 0x0636, 0x00D7, 0x0637, 0x0638, 0x0639, 0x063A, 0x0640, 0x0641, 0x0642, 0x0643,
    0x00E0, 0x0644, 0x00E2, 0x0645, 0x0646, 0x0647, 0x0648, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0649, 0x064A, 0x00EE, 0x00EF,
    0x064B, 0x064C, 0x064D, 0x064E, 0x00F4, 0x064F, 0x0650, 0x00F7, 0x0651, 0x00F9, 0x0652, 0x00FB, 0x00FC, 0x200E, 0x200F, 0x06D2,
}};
// https://encoding.spec.whatwg.org/index-windows-1257.txt
SingleByteDecoder s_windows1257_decoder {{
    0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021, 0x0088, 0x2030, 0x008A, 0x2039, 0x008C, 0x00A8, 0x02C7, 0x00B8,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x009A, 0x203A, 0x009C, 0x00AF, 0x02DB, 0x009F,
    0x00A0, 0xFFFD, 0x00A2, 0x00A3, 0x00A4, 0xFFFD, 0x00A6, 0x00A7, 0x00D8, 0x00A9, 0x0156, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00C6,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00F8, 0x00B9, 0x0157, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00E6,
    0x0104, 0x012E, 0x0100, 0x0106, 0x00C4, 0x00C5, 0x0118, 0x0112, 0x010C, 0x00C9, 0x0179, 0x0116, 0x0122, 0x0136, 0x012A, 0x013B,
    0x0160, 0x0143, 0x0145, 0x00D3, 0x014C, 0x00D5, 0x00D6, 0x00D7, 0x0172, 0x0141, 0x015A, 0x016A, 0x00DC, 0x017B, 0x017D, 0x00DF,
    0x0105, 0x012F, 0x0101, 0x0107, 0x00E4, 0x00E5, 0x0119, 0x0113, 0x010D, 0x00E9, 0x017A, 0x0117, 0x0123, 0x0137, 0x012B, 0x013C,
    0x0161, 0x0144, 0x0146, 0x00F3, 0x014D, 0x00F5, 0x00F6, 0x00F7, 0x0173, 0x0142, 0x015B, 0x016B, 0x00FC, 0x017C, 0x017E, 0x02D9,
}};
// https://encoding.spec.whatwg.org/index-windows-1258.txt
SingleByteDecoder s_windows1258_decoder {{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x008A, 0x2039, 0x0152, 0x008D, 0x008E, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x009A, 0x203A, 0x0153, 0x009D, 0x009E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x0300, 0x00CD, 0x00CE, 0x00CF,
    0x0110, 0x00D1, 0x0309, 0x00D3, 0x00D4, 0x01A0, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x01AF, 0x0303, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0301, 0x00ED, 0x00EE, 0x00EF,
    0x0111, 0x00F1, 0x0323, 0x00F3, 0x00F4, 0x01A1, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x01B0, 0x20AB, 0x00FF,
}};
// https://encoding.spec.whatwg.org/index-x-mac-cyrillic.txt
SingleByteDecoder s_mac_cyrillic_decoder {{
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x2020, 0x00B0, 0x0490, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x0406, 0x00AE, 0x00A9, 0x2122, 0x0402, 0x0452, 0x2260, 0x0403, 0x0453,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x0456, 0x00B5, 0x0491, 0x0408, 0x0404, 0x0454, 0x0407, 0x0457, 0x0409, 0x0459, 0x040A, 0x045A,
    0x0458, 0x0405, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x040B, 0x045B, 0x040C, 0x045C, 0x0455,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x201E, 0x040E, 0x045E, 0x040F, 0x045F, 0x2116, 0x0401, 0x0451, 0x044F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x20AC,
}};
// clang-format on

}

Optional<Decoder&> decoder_for(StringView a_encoding)
{
    auto encoding = get_standardized_encoding(a_encoding);
    if (encoding.has_value()) {
        if (encoding.value().equals_ignoring_ascii_case("windows-1252"sv))
            return s_latin1_decoder;
        if (encoding.value().equals_ignoring_ascii_case("utf-8"sv))
            return s_utf8_decoder;
        if (encoding.value().equals_ignoring_ascii_case("utf-16be"sv))
            return s_utf16be_decoder;
        if (encoding.value().equals_ignoring_ascii_case("utf-16le"sv))
            return s_utf16le_decoder;
        if (encoding.value().equals_ignoring_ascii_case("big5"sv))
            return s_big5_decoder;
        if (encoding.value().equals_ignoring_ascii_case("euc-jp"sv))
            return s_euc_jp_decoder;
        if (encoding.value().equals_ignoring_ascii_case("euc-kr"sv))
            return s_euc_kr_decoder;
        if (encoding.value().equals_ignoring_ascii_case("gbk"sv))
            return s_gb18030_decoder;
        if (encoding.value().equals_ignoring_ascii_case("gb18030"sv))
            return s_gb18030_decoder;
        if (encoding.value().equals_ignoring_ascii_case("ibm866"sv))
            return s_ibm866_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-2022-jp"sv))
            return s_iso_2022_jp_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-2"sv))
            return s_latin2_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-3"sv))
            return s_latin3_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-4"sv))
            return s_latin4_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-5"sv))
            return s_latin_cyrillic_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-6"sv))
            return s_latin_arabic_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-7"sv))
            return s_latin_greek_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-8"sv))
            return s_latin_hebrew_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-8-i"sv))
            return s_latin_hebrew_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-10"sv))
            return s_latin6_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-13"sv))
            return s_latin7_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-14"sv))
            return s_latin8_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-15"sv))
            return s_latin9_decoder;
        if (encoding.value().equals_ignoring_ascii_case("iso-8859-16"sv))
            return s_latin10_decoder;
        if (encoding.value().equals_ignoring_ascii_case("koi8-r"sv))
            return s_koi8r_decoder;
        if (encoding.value().equals_ignoring_ascii_case("koi8-u"sv))
            return s_koi8u_decoder;
        if (encoding.value().equals_ignoring_ascii_case("macintosh"sv))
            return s_mac_roman_decoder;
        if (encoding.value().equals_ignoring_ascii_case("PDFDocEncoding"sv))
            return s_pdf_doc_encoding_decoder;
        if (encoding.value().equals_ignoring_ascii_case("replacement"sv))
            return s_replacement_decoder;
        if (encoding.value().equals_ignoring_ascii_case("shift_jis"sv))
            return s_shift_jis_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-874"sv))
            return s_windows874_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1250"sv))
            return s_centraleurope_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1251"sv))
            return s_cyrillic_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1253"sv))
            return s_windows1253_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1254"sv))
            return s_turkish_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1255"sv))
            return s_hebrew_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1256"sv))
            return s_windows1256_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1257"sv))
            return s_windows1257_decoder;
        if (encoding.value().equals_ignoring_ascii_case("windows-1258"sv))
            return s_windows1258_decoder;
        if (encoding.value().equals_ignoring_ascii_case("x-mac-cyrillic"sv))
            return s_mac_cyrillic_decoder;
        if (encoding.value().equals_ignoring_ascii_case("x-user-defined"sv))
            return s_x_user_defined_decoder;
    }
    dbgln("TextCodec: No decoder implemented for encoding '{}'", a_encoding);
    return {};
}

// https://encoding.spec.whatwg.org/#concept-encoding-get
Optional<StringView> get_standardized_encoding(StringView encoding)
{
    // 1. Remove any leading and trailing ASCII whitespace from label.
    // https://infra.spec.whatwg.org/#ascii-whitespace: ASCII whitespace is U+0009 TAB, U+000A LF, U+000C FF, U+000D CR, or U+0020 SPACE.
    encoding = encoding.trim("\t\n\f\r "sv);

    // 2. If label is an ASCII case-insensitive match for any of the labels listed in the table below, then return the corresponding encoding; otherwise return failure.
    if (encoding.is_one_of_ignoring_ascii_case("unicode-1-1-utf-8"sv, "unicode11utf8"sv, "unicode20utf8"sv, "utf-8"sv, "utf8"sv, "x-unicode20utf8"sv))
        return "UTF-8"sv;
    if (encoding.is_one_of_ignoring_ascii_case("866"sv, "cp866"sv, "csibm866"sv, "ibm866"sv))
        return "IBM866"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatin2"sv, "iso-8859-2"sv, "iso-ir-101"sv, "iso8859-2"sv, "iso88592"sv, "iso_8859-2"sv, "iso_8859-2:1987"sv, "l2"sv, "latin2"sv))
        return "ISO-8859-2"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatin3"sv, "iso-8859-3"sv, "iso-ir-109"sv, "iso8859-3"sv, "iso88593"sv, "iso_8859-3"sv, "iso_8859-3:1988"sv, "l3"sv, "latin3"sv))
        return "ISO-8859-3"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatin4"sv, "iso-8859-4"sv, "iso-ir-110"sv, "iso8859-4"sv, "iso88594"sv, "iso_8859-4"sv, "iso_8859-4:1988"sv, "l4"sv, "latin4"sv))
        return "ISO-8859-4"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatincyrillic"sv, "cyrillic"sv, "iso-8859-5"sv, "iso-ir-144"sv, "iso8859-5"sv, "iso88595"sv, "iso_8859-5"sv, "iso_8859-5:1988"sv))
        return "ISO-8859-5"sv;
    if (encoding.is_one_of_ignoring_ascii_case("arabic"sv, "asmo-708"sv, "csiso88596e"sv, "csiso88596i"sv, "csisolatinarabic"sv, "ecma-114"sv, "iso-8859-6"sv, "iso-8859-6-e"sv, "iso-8859-6-i"sv, "iso-ir-127"sv, "iso8859-6"sv, "iso88596"sv, "iso_8859-6"sv, "iso_8859-6:1987"sv))
        return "ISO-8859-6"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatingreek"sv, "ecma-118"sv, "elot_928"sv, "greek"sv, "greek8"sv, "iso-8859-7"sv, "iso-ir-126"sv, "iso8859-7"sv, "iso88597"sv, "iso_8859-7"sv, "iso_8859-7:1987"sv, "sun_eu_greek"sv))
        return "ISO-8859-7"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csiso88598e"sv, "csisolatinhebrew"sv, "hebrew"sv, "iso-8859-8"sv, "iso-8859-8-e"sv, "iso-ir-138"sv, "iso8859-8"sv, "iso88598"sv, "iso_8859-8"sv, "iso_8859-8:1988"sv, "visual"sv))
        return "ISO-8859-8"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csiso88598i"sv, "iso-8859-8-i"sv, "logical"sv))
        return "ISO-8859-8-I"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatin6"sv, "iso-8859-10"sv, "iso-ir-157"sv, "iso8859-10"sv, "iso885910"sv, "l6"sv, "latin6"sv))
        return "ISO-8859-10"sv;
    if (encoding.is_one_of_ignoring_ascii_case("iso-8859-13"sv, "iso8859-13"sv, "iso885913"sv))
        return "ISO-8859-13"sv;
    if (encoding.is_one_of_ignoring_ascii_case("iso-8859-14"sv, "iso8859-14"sv, "iso885914"sv))
        return "ISO-8859-14"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csisolatin9"sv, "iso-8859-15"sv, "iso8859-15"sv, "iso885915"sv, "iso_8859-15"sv, "l9"sv))
        return "ISO-8859-15"sv;
    if (encoding.is_one_of_ignoring_ascii_case("iso-8859-16"sv))
        return "ISO-8859-16"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cskoi8r"sv, "koi"sv, "koi8"sv, "koi8-r"sv, "koi8_r"sv))
        return "KOI8-R"sv;
    if (encoding.is_one_of_ignoring_ascii_case("koi8-ru"sv, "koi8-u"sv))
        return "KOI8-U"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csmacintosh"sv, "mac"sv, "macintosh"sv, "x-mac-roman"sv))
        return "macintosh"sv;
    if (encoding.is_one_of_ignoring_ascii_case("pdfdocencoding"sv))
        return "PDFDocEncoding"sv;
    if (encoding.is_one_of_ignoring_ascii_case("dos-874"sv, "iso-8859-11"sv, "iso8859-11"sv, "iso885911"sv, "tis-620"sv, "windows-874"sv))
        return "windows-874"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1250"sv, "windows-1250"sv, "x-cp1250"sv))
        return "windows-1250"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1251"sv, "windows-1251"sv, "x-cp1251"sv))
        return "windows-1251"sv;
    if (encoding.is_one_of_ignoring_ascii_case("ansi_x3.4-1968"sv, "ascii"sv, "cp1252"sv, "cp819"sv, "csisolatin1"sv, "ibm819"sv, "iso-8859-1"sv, "iso-ir-100"sv, "iso8859-1"sv, "iso88591"sv, "iso_8859-1"sv, "iso_8859-1:1987"sv, "l1"sv, "latin1"sv, "us-ascii"sv, "windows-1252"sv, "x-cp1252"sv))
        return "windows-1252"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1253"sv, "windows-1253"sv, "x-cp1253"sv))
        return "windows-1253"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1254"sv, "csisolatin5"sv, "iso-8859-9"sv, "iso-ir-148"sv, "iso8859-9"sv, "iso88599"sv, "iso_8859-9"sv, "iso_8859-9:1989"sv, "l5"sv, "latin5"sv, "windows-1254"sv, "x-cp1254"sv))
        return "windows-1254"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1255"sv, "windows-1255"sv, "x-cp1255"sv))
        return "windows-1255"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1256"sv, "windows-1256"sv, "x-cp1256"sv))
        return "windows-1256"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1257"sv, "windows-1257"sv, "x-cp1257"sv))
        return "windows-1257"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cp1258"sv, "windows-1258"sv, "x-cp1258"sv))
        return "windows-1258"sv;
    if (encoding.is_one_of_ignoring_ascii_case("x-mac-cyrillic"sv, "x-mac-ukrainian"sv))
        return "x-mac-cyrillic"sv;
    if (encoding.is_one_of_ignoring_ascii_case("koi8-r"sv, "koi8r"sv))
        return "koi8-r"sv;
    if (encoding.is_one_of_ignoring_ascii_case("chinese"sv, "csgb2312"sv, "csiso58gb231280"sv, "gb2312"sv, "gb_2312"sv, "gb_2312-80"sv, "gbk"sv, "iso-ir-58"sv, "x-gbk"sv))
        return "GBK"sv;
    if (encoding.is_one_of_ignoring_ascii_case("gb18030"sv))
        return "gb18030"sv;
    if (encoding.is_one_of_ignoring_ascii_case("big5"sv, "big5-hkscs"sv, "cn-big5"sv, "csbig5"sv, "x-x-big5"sv))
        return "Big5"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cseucpkdfmtjapanese"sv, "euc-jp"sv, "x-euc-jp"sv))
        return "EUC-JP"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csiso2022jp"sv, "iso-2022-jp"sv))
        return "ISO-2022-JP"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csshiftjis"sv, "ms932"sv, "ms_kanji"sv, "shift-jis"sv, "shift_jis"sv, "sjis"sv, "windows-31j"sv, "x-sjis"sv))
        return "Shift_JIS"sv;
    if (encoding.is_one_of_ignoring_ascii_case("cseuckr"sv, "csksc56011987"sv, "euc-kr"sv, "iso-ir-149"sv, "korean"sv, "ks_c_5601-1987"sv, "ks_c_5601-1989"sv, "ksc5601"sv, "ksc_5601"sv, "windows-949"sv))
        return "EUC-KR"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csiso2022kr"sv, "hz-gb-2312"sv, "iso-2022-cn"sv, "iso-2022-cn-ext"sv, "iso-2022-kr"sv, "replacement"sv))
        return "replacement"sv;
    if (encoding.is_one_of_ignoring_ascii_case("unicodefffe"sv, "utf-16be"sv))
        return "UTF-16BE"sv;
    if (encoding.is_one_of_ignoring_ascii_case("csunicode"sv, "iso-10646-ucs-2"sv, "ucs-2"sv, "unicode"sv, "unicodefeff"sv, "utf-16"sv, "utf-16le"sv))
        return "UTF-16LE"sv;
    if (encoding.is_one_of_ignoring_ascii_case("x-user-defined"sv))
        return "x-user-defined"sv;

    dbgln("TextCodec: Unrecognized encoding: {}", encoding);
    return {};
}

// https://encoding.spec.whatwg.org/#bom-sniff
Optional<Decoder&> bom_sniff_to_decoder(StringView input)
{
    // 1. Let BOM be the result of peeking 3 bytes from ioQueue, converted to a byte sequence.
    // 2. For each of the rows in the table below, starting with the first one and going down,
    //    if BOM starts with the bytes given in the first column, then return the encoding given
    //    in the cell in the second column of that row. Otherwise, return null.

    // Byte Order Mark | Encoding
    // --------------------------
    // 0xEF 0xBB 0xBF  | UTF-8
    // 0xFE 0xFF       | UTF-16BE
    // 0xFF 0xFE       | UTF-16LE

    auto bytes = input.bytes();
    if (bytes.size() < 2)
        return {};

    auto first_byte = bytes[0];

    switch (first_byte) {
    case 0xEF: // UTF-8
        if (bytes.size() < 3)
            return {};
        if (bytes[1] == 0xBB && bytes[2] == 0xBF)
            return s_utf8_decoder;
        return {};
    case 0xFE: // UTF-16BE
        if (bytes[1] == 0xFF)
            return s_utf16be_decoder;
        return {};
    case 0xFF: // UTF-16LE
        if (bytes[1] == 0xFE)
            return s_utf16le_decoder;
        return {};
    }

    return {};
}

// https://encoding.spec.whatwg.org/#decode
ErrorOr<String> convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder& fallback_decoder, StringView input)
{
    Decoder* actual_decoder = &fallback_decoder;

    // 1. Let BOMEncoding be the result of BOM sniffing ioQueue.
    // 2. If BOMEncoding is non-null:
    if (auto unicode_decoder = bom_sniff_to_decoder(input); unicode_decoder.has_value()) {
        // 1. Set encoding to BOMEncoding.
        actual_decoder = &unicode_decoder.value();

        // 2. Read three bytes from ioQueue, if BOMEncoding is UTF-8; otherwise read two bytes. (Do nothing with those bytes.)
        // FIXME: I imagine this will be pretty slow for large inputs, as it's regenerating the input without the first 2/3 bytes.
        input = input.substring_view(&unicode_decoder.value() == &s_utf8_decoder ? 3 : 2);
    }

    VERIFY(actual_decoder);

    // 3. Process a queue with an instance of encoding’s decoder, ioQueue, output, and "replacement".
    // FIXME: This isn't the exact same as the spec, which is written in terms of I/O queues.
    auto output = TRY(actual_decoder->to_utf8(input));

    // 4. Return output.
    return output;
}

// https://encoding.spec.whatwg.org/#get-an-output-encoding
StringView get_output_encoding(StringView encoding)
{
    // 1. If encoding is replacement or UTF-16BE/LE, then return UTF-8.
    if (encoding.is_one_of_ignoring_ascii_case("replacement"sv, "utf-16le"sv, "utf-16be"sv))
        return "UTF-8"sv;

    // 2. Return encoding.
    return encoding;
}

bool Decoder::validate(StringView input)
{
    auto result = this->process(input, [](auto code_point) -> ErrorOr<void> {
        if (code_point == replacement_code_point)
            return Error::from_errno(EINVAL);
        return {};
    });

    return !result.is_error();
}

ErrorOr<String> Decoder::to_utf8(StringView input)
{
    StringBuilder builder(input.length());
    TRY(process(input, [&builder](u32 c) { return builder.try_append_code_point(c); }));
    return builder.to_string_without_validation();
}

ErrorOr<void> UTF8Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    for (auto c : Utf8View(input)) {
        TRY(on_code_point(c));
    }
    return {};
}

bool UTF8Decoder::validate(StringView input)
{
    return Utf8View(input).validate();
}

ErrorOr<String> UTF8Decoder::to_utf8(StringView input)
{
    // Discard the BOM
    auto bomless_input = input;
    if (auto bytes = input.bytes(); bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bomless_input = input.substring_view(3);
    }

    return Decoder::to_utf8(bomless_input);
}

ErrorOr<void> UTF16BEDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // rfc2781, 2.2 Decoding UTF-16
    size_t utf16_length = input.length() - (input.length() % 2);
    for (size_t i = 0; i < utf16_length; i += 2) {
        // 1) If W1 < 0xD800 or W1 > 0xDFFF, the character value U is the value
        //    of W1. Terminate.
        u16 w1 = (static_cast<u8>(input[i]) << 8) | static_cast<u8>(input[i + 1]);
        if (!is_unicode_surrogate(w1)) {
            TRY(on_code_point(w1));
            continue;
        }

        // 2) Determine if W1 is between 0xD800 and 0xDBFF. If not, the sequence
        //    is in error and no valid character can be obtained using W1.
        //    Terminate.
        // 3) If there is no W2 (that is, the sequence ends with W1), or if W2
        //    is not between 0xDC00 and 0xDFFF, the sequence is in error.
        //    Terminate.
        if (!Utf16View::is_high_surrogate(w1) || i + 2 == utf16_length) {
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        u16 w2 = (static_cast<u8>(input[i + 2]) << 8) | static_cast<u8>(input[i + 3]);
        if (!Utf16View::is_low_surrogate(w2)) {
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 4) Construct a 20-bit unsigned integer U', taking the 10 low-order
        //    bits of W1 as its 10 high-order bits and the 10 low-order bits of
        //    W2 as its 10 low-order bits.
        // 5) Add 0x10000 to U' to obtain the character value U. Terminate.
        TRY(on_code_point(Utf16View::decode_surrogate_pair(w1, w2)));
        i += 2;
    }

    return {};
}

bool UTF16BEDecoder::validate(StringView input)
{
    size_t utf16_length = input.length() - (input.length() % 2);
    for (size_t i = 0; i < utf16_length; i += 2) {
        u16 w1 = (static_cast<u8>(input[i]) << 8) | static_cast<u8>(input[i + 1]);
        if (!is_unicode_surrogate(w1))
            continue;

        if (!Utf16View::is_high_surrogate(w1) || i + 2 == utf16_length)
            return false;

        u16 w2 = (static_cast<u8>(input[i + 2]) << 8) | static_cast<u8>(input[i + 3]);
        if (!Utf16View::is_low_surrogate(w2))
            return false;

        i += 2;
    }
    return true;
}

ErrorOr<String> UTF16BEDecoder::to_utf8(StringView input)
{
    // Discard the BOM
    auto bomless_input = input;
    if (auto bytes = input.bytes(); bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
        bomless_input = input.substring_view(2);

    StringBuilder builder(bomless_input.length() / 2);
    TRY(process(bomless_input, [&builder](u32 c) { return builder.try_append_code_point(c); }));
    return builder.to_string();
}

ErrorOr<void> UTF16LEDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // rfc2781, 2.2 Decoding UTF-16
    size_t utf16_length = input.length() - (input.length() % 2);
    for (size_t i = 0; i < utf16_length; i += 2) {
        // 1) If W1 < 0xD800 or W1 > 0xDFFF, the character value U is the value
        //    of W1. Terminate.
        u16 w1 = static_cast<u8>(input[i]) | (static_cast<u8>(input[i + 1]) << 8);
        if (!is_unicode_surrogate(w1)) {
            TRY(on_code_point(w1));
            continue;
        }

        // 2) Determine if W1 is between 0xD800 and 0xDBFF. If not, the sequence
        //    is in error and no valid character can be obtained using W1.
        //    Terminate.
        // 3) If there is no W2 (that is, the sequence ends with W1), or if W2
        //    is not between 0xDC00 and 0xDFFF, the sequence is in error.
        //    Terminate.
        if (!Utf16View::is_high_surrogate(w1) || i + 2 == utf16_length) {
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        u16 w2 = static_cast<u8>(input[i + 2]) | (static_cast<u8>(input[i + 3]) << 8);
        if (!Utf16View::is_low_surrogate(w2)) {
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 4) Construct a 20-bit unsigned integer U', taking the 10 low-order
        //    bits of W1 as its 10 high-order bits and the 10 low-order bits of
        //    W2 as its 10 low-order bits.
        // 5) Add 0x10000 to U' to obtain the character value U. Terminate.
        TRY(on_code_point(Utf16View::decode_surrogate_pair(w1, w2)));
        i += 2;
    }

    return {};
}

bool UTF16LEDecoder::validate(StringView input)
{
    size_t utf16_length = input.length() - (input.length() % 2);
    for (size_t i = 0; i < utf16_length; i += 2) {
        u16 w1 = static_cast<u8>(input[i]) | (static_cast<u8>(input[i + 1]) << 8);
        if (!is_unicode_surrogate(w1))
            continue;

        if (!Utf16View::is_high_surrogate(w1) || i + 2 == utf16_length)
            return false;

        u16 w2 = static_cast<u8>(input[i + 2]) | (static_cast<u8>(input[i + 3]) << 8);
        if (!Utf16View::is_low_surrogate(w2))
            return false;

        i += 2;
    }
    return true;
}

ErrorOr<String> UTF16LEDecoder::to_utf8(StringView input)
{
    // Discard the BOM
    auto bomless_input = input;
    if (auto bytes = input.bytes(); bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        bomless_input = input.substring_view(2);

    StringBuilder builder(bomless_input.length() / 2);
    TRY(process(bomless_input, [&builder](u32 c) { return builder.try_append_code_point(c); }));
    return builder.to_string();
}

ErrorOr<void> Latin1Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    for (u8 ch : input) {
        // Latin1 is the same as the first 256 Unicode code_points, so no mapping is needed, just utf-8 encoding.
        TRY(on_code_point(ch));
    }

    return {};
}

namespace {
u32 convert_latin2_to_utf8(u8 in)
{
    switch (in) {

#define MAP(X, Y) \
    case X:       \
        return Y

        MAP(0xA1, 0x104);
        MAP(0xA2, 0x2D8);
        MAP(0xA3, 0x141);
        MAP(0xA5, 0x13D);
        MAP(0xA6, 0x15A);
        MAP(0xA9, 0x160);
        MAP(0xAA, 0x15E);
        MAP(0xAB, 0x164);
        MAP(0xAC, 0x179);
        MAP(0xAE, 0x17D);
        MAP(0xAF, 0x17B);

        MAP(0xB1, 0x105);
        MAP(0xB2, 0x2DB);
        MAP(0xB3, 0x142);
        MAP(0xB5, 0x13E);
        MAP(0xB6, 0x15B);
        MAP(0xB7, 0x2C7);
        MAP(0xB9, 0x161);
        MAP(0xBA, 0x15F);
        MAP(0xBB, 0x165);
        MAP(0xBC, 0x17A);
        MAP(0xBD, 0x2DD);
        MAP(0xBE, 0x17E);
        MAP(0xBF, 0x17C);

        MAP(0xC0, 0x154);
        MAP(0xC3, 0x102);
        MAP(0xC5, 0x139);
        MAP(0xC6, 0x106);
        MAP(0xC8, 0x10C);
        MAP(0xCA, 0x118);
        MAP(0xCC, 0x11A);
        MAP(0xCF, 0x10E);

        MAP(0xD0, 0x110);
        MAP(0xD1, 0x143);
        MAP(0xD2, 0x147);
        MAP(0xD5, 0x150);
        MAP(0xD8, 0x158);
        MAP(0xD9, 0x16E);
        MAP(0xDB, 0x170);
        MAP(0xDE, 0x162);

        MAP(0xE0, 0x155);
        MAP(0xE3, 0x103);
        MAP(0xE5, 0x13A);
        MAP(0xE6, 0x107);
        MAP(0xE8, 0x10D);
        MAP(0xEA, 0x119);
        MAP(0xEC, 0x11B);
        MAP(0xEF, 0x10F);

        MAP(0xF0, 0x111);
        MAP(0xF1, 0x144);
        MAP(0xF2, 0x148);
        MAP(0xF5, 0x151);
        MAP(0xF8, 0x159);
        MAP(0xF9, 0x16F);
        MAP(0xFB, 0x171);
        MAP(0xFE, 0x163);
        MAP(0xFF, 0x2D9);
#undef MAP

    default:
        return in;
    }
}
}

ErrorOr<void> Latin2Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    for (auto c : input) {
        TRY(on_code_point(convert_latin2_to_utf8(c)));
    }

    return {};
}

ErrorOr<void> Latin9Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    auto convert_latin9_to_utf8 = [](u8 ch) -> u32 {
        // Latin9 is the same as the first 256 Unicode code points, except for 8 characters.
        switch (ch) {
        case 0xA4:
            return 0x20AC;
        case 0xA6:
            return 0x160;
        case 0xA8:
            return 0x161;
        case 0xB4:
            return 0x17D;
        case 0xB8:
            return 0x17E;
        case 0xBC:
            return 0x152;
        case 0xBD:
            return 0x153;
        case 0xBE:
            return 0x178;
        default:
            return ch;
        }
    };

    for (auto ch : input) {
        TRY(on_code_point(convert_latin9_to_utf8(ch)));
    }

    return {};
}

ErrorOr<void> PDFDocEncodingDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // PDF 1.7 spec, Appendix D.2 "PDFDocEncoding Character Set"
    // Character codes 0-8, 11-12, 14-23, 127, 159, 173 are not defined per spec.
    // clang-format off
    static constexpr Array<u32, 256> translation_table = {
        0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
        0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
        0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0017, 0x0017,
        0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC,
        0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
        0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
        0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
        0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
        0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
        0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
        0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
        0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
        0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
        0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
        0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
        0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFC,
        0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044,
        0x2039, 0x203A, 0x2212, 0x2030, 0x201E, 0x201C, 0x201D, 0x2018,
        0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160,
        0x0178, 0x017D, 0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0xFFFC,
        0x20AC, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
        0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0xFFFC, 0x00AE, 0x00AF,
        0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
        0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
        0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
        0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
        0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
        0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
        0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
        0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
        0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
        0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
    };
    // clang-format on

    for (u8 ch : input)
        TRY(on_code_point(translation_table[ch]));

    return {};
}

ErrorOr<void> TurkishDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    auto convert_turkish_to_utf8 = [](u8 ch) -> u32 {
        // Turkish (aka ISO-8859-9, Windows-1254) is the same as the first 256 Unicode code points, except for 6 characters.
        switch (ch) {
        case 0xD0:
            return 0x11E;
        case 0xDD:
            return 0x130;
        case 0xDE:
            return 0x15E;
        case 0xF0:
            return 0x11F;
        case 0xFD:
            return 0x131;
        case 0xFE:
            return 0x15F;
        default:
            return ch;
        }
    };

    for (auto ch : input) {
        TRY(on_code_point(convert_turkish_to_utf8(ch)));
    }

    return {};
}

// https://encoding.spec.whatwg.org/#x-user-defined-decoder
ErrorOr<void> XUserDefinedDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    auto convert_x_user_defined_to_utf8 = [](u8 ch) -> u32 {
        // 2. If byte is an ASCII byte, return a code point whose value is byte.
        // https://infra.spec.whatwg.org/#ascii-byte
        // An ASCII byte is a byte in the range 0x00 (NUL) to 0x7F (DEL), inclusive.
        // NOTE: This doesn't check for ch >= 0x00, as that would always be true due to being unsigned.
        if (ch <= 0x7f)
            return ch;

        // 3. Return a code point whose value is 0xF780 + byte − 0x80.
        return 0xF780 + ch - 0x80;
    };

    for (auto ch : input) {
        TRY(on_code_point(convert_x_user_defined_to_utf8(ch)));
    }

    // 1. If byte is end-of-queue, return finished.

    return {};
}

// https://encoding.spec.whatwg.org/#single-byte-decoder
ErrorOr<void> SingleByteDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    for (u8 const byte : input) {
        if (byte < 0x80) {
            // 2. If byte is an ASCII byte, return a code point whose value is byte.
            TRY(on_code_point(byte));
        } else {
            // 3. Let code point be the index code point for byte − 0x80 in index single-byte.
            auto code_point = m_translation_table[byte - 0x80];

            // 4. If code point is null, return error.
            // NOTE: Error is communicated with 0xFFFD

            // 5. Return a code point whose value is code point.
            TRY(on_code_point(code_point));
        }
    }
    // 1. If byte is end-of-queue, return finished.
    return {};
}

// https://encoding.spec.whatwg.org/#index-gb18030-ranges-code-point
static Optional<u32> index_gb18030_ranges_code_point(u32 pointer)
{
    // 1. If pointer is greater than 39419 and less than 189000, or pointer is greater than 1237575, return null.
    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575)
        return {};

    // 2. If pointer is 7457, return code point U+E7C7.
    if (pointer == 7457)
        return 0xE7C7;

    // FIXME: Encoding specification is not updated to GB-18030-2022 yet (https://github.com/whatwg/encoding/issues/312)
    // NOTE: This matches https://commits.webkit.org/266173@main
    switch (pointer) {
    case 19057:
        return 0xE81E; // 82 35 90 37
    case 19058:
        return 0xE826; // 82 35 90 38
    case 19059:
        return 0xE82B; // 82 35 90 39
    case 19060:
        return 0xE82C; // 82 35 91 30
    case 19061:
        return 0xE832; // 82 35 91 31
    case 19062:
        return 0xE843; // 82 35 91 32
    case 19063:
        return 0xE854; // 82 35 91 33
    case 19064:
        return 0xE864; // 82 35 91 34
    case 39076:
        return 0xE78D; // 84 31 82 36
    case 39077:
        return 0xE78F; // 84 31 82 37
    case 39078:
        return 0xE78E; // 84 31 82 38
    case 39079:
        return 0xE790; // 84 31 82 39
    case 39080:
        return 0xE791; // 84 31 83 30
    case 39081:
        return 0xE792; // 84 31 83 31
    case 39082:
        return 0xE793; // 84 31 83 32
    case 39083:
        return 0xE794; // 84 31 83 33
    case 39084:
        return 0xE795; // 84 31 83 34
    case 39085:
        return 0xE796; // 84 31 83 35
    default:
        break;
    }

    // 3. Let offset be the last pointer in index gb18030 ranges that is less than or equal to pointer and let code point offset be its corresponding code point.
    size_t last_index;
    binary_search(s_gb18030_ranges, pointer, &last_index, [](auto const pointer, auto const& entry) {
        return pointer - entry.pointer;
    });
    auto offset = s_gb18030_ranges[last_index].pointer;
    auto code_point_offset = s_gb18030_ranges[last_index].code_point;

    // 4. Return a code point whose value is code point offset + pointer − offset.
    return code_point_offset + pointer - offset;
}

// https://encoding.spec.whatwg.org/#gb18030-decoder
ErrorOr<void> GB18030Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // gb18030’s decoder has an associated gb18030 first, gb18030 second, and gb18030 third (all initially 0x00).
    u8 first = 0x00;
    u8 second = 0x00;
    u8 third = 0x00;

    // gb18030’s decoder’s handler, given ioQueue and byte, runs these steps:
    size_t index = 0;
    while (true) {
        // 1. If byte is end-of-queue and gb18030 first, gb18030 second, and gb18030 third are 0x00, return finished.
        if (index >= input.length() && first == 0x00 && second == 0x00 && third == 0x00)
            return {};

        // 2. If byte is end-of-queue, and gb18030 first, gb18030 second, or gb18030 third is not 0x00, set gb18030 first, gb18030 second, and gb18030 third to 0x00, and return error.
        if (index >= input.length() && (first != 0x00 || second != 0x00 || third != 0x00)) {
            first = 0x00;
            second = 0x00;
            third = 0x00;
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        u8 const byte = input[index++];
        // 3. If gb18030 third is not 0x00, then:
        if (third != 0x00) {
            // 1. If byte is not in the range 0x30 to 0x39, inclusive, then:
            if (byte < 0x30 || byte > 0x39) {
                // 1. Restore « gb18030 second, gb18030 third, byte » to ioQueue.
                index -= 3;

                // 2. Set gb18030 first, gb18030 second, and gb18030 third to 0x00.
                first = 0x00;
                second = 0x00;
                third = 0x00;

                // 3. Return error.
                TRY(on_code_point(replacement_code_point));
                continue;
            }

            // 2. Let code point be the index gb18030 ranges code point for ((gb18030 first − 0x81) × (10 × 126 × 10)) + ((gb18030 second − 0x30) × (10 × 126)) + ((gb18030 third − 0x81) × 10) + byte − 0x30.
            auto code_point = index_gb18030_ranges_code_point(((first - 0x81) * (10 * 126 * 10)) + ((second - 0x30) * (10 * 126)) + ((third - 0x81) * 10) + byte - 0x30);

            // 3. Set gb18030 first, gb18030 second, and gb18030 third to 0x00.
            first = 0x00;
            second = 0x00;
            third = 0x00;

            // 4. If code point is null, return error.
            if (!code_point.has_value()) {
                TRY(on_code_point(replacement_code_point));
                continue;
            }

            // 5. Return a code point whose value is code point.
            TRY(on_code_point(code_point.value()));
            continue;
        }

        // 4. If gb18030 second is not 0x00, then:
        if (second != 0x00) {
            // 1. If byte is in the range 0x81 to 0xFE, inclusive, set gb18030 third to byte and return continue.
            if (byte >= 0x81 && byte <= 0xFE) {
                third = byte;
                continue;
            }

            // 2. Restore « gb18030 second, byte » to ioQueue, set gb18030 first and gb18030 second to 0x00, and return error.
            index -= 2;
            first = 0x00;
            second = 0x00;
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 5. If gb18030 first is not 0x00, then:
        if (first != 0x00) {
            // 1. If byte is in the range 0x30 to 0x39, inclusive, set gb18030 second to byte and return continue.
            if (byte >= 0x30 && byte <= 0x39) {
                second = byte;
                continue;
            }

            // 2. Let lead be gb18030 first, let pointer be null, and set gb18030 first to 0x00.
            auto lead = first;
            Optional<u32> pointer;
            first = 0x00;

            // 3. Let offset be 0x40 if byte is less than 0x7F, otherwise 0x41.
            u8 const offset = byte < 0x7F ? 0x40 : 0x41;

            // 4. If byte is in the range 0x40 to 0x7E, inclusive, or 0x80 to 0xFE, inclusive, set pointer to (lead − 0x81) × 190 + (byte − offset).
            if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFE))
                pointer = (lead - 0x81) * 190 + (byte - offset);

            // 5. Let code point be null if pointer is null, otherwise the index code point for pointer in index gb18030.
            auto code_point = pointer.has_value() ? index_gb18030_code_point(pointer.value()) : Optional<u32> {};

            // 6. If code point is non-null, return a code point whose value is code point.
            if (code_point.has_value()) {
                TRY(on_code_point(code_point.value()));
                continue;
            }

            // 7. If byte is an ASCII byte, restore byte to ioQueue.
            if (byte <= 0x7F)
                index--;

            // 8. Return error.
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 6. If byte is an ASCII byte, return a code point whose value is byte.
        if (byte <= 0x7F) {
            TRY(on_code_point(byte));
            continue;
        }

        // 7. If byte is 0x80, return code point U+20AC.
        if (byte == 0x80) {
            TRY(on_code_point(0x20AC));
            continue;
        }

        // 8. If byte is in the range 0x81 to 0xFE, inclusive, set gb18030 first to byte and return continue.
        if (byte >= 0x81 && byte <= 0xFE) {
            first = byte;
            continue;
        }

        // 9. Return error.
        TRY(on_code_point(replacement_code_point));
    }
}

// https://encoding.spec.whatwg.org/#big5-decoder
ErrorOr<void> Big5Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // Big5’s decoder has an associated Big5 lead (initially 0x00).
    u8 big5_lead = 0x00;

    // Big5’s decoder’s handler, given ioQueue and byte, runs these steps:
    size_t index = 0;
    while (true) {
        // 1. If byte is end-of-queue and Big5 lead is not 0x00, set Big5 lead to 0x00 and return error.
        if (index >= input.length() && big5_lead != 0x00) {
            big5_lead = 0x00;
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 2. If byte is end-of-queue and Big5 lead is 0x00, return finished.
        if (index >= input.length() && big5_lead == 0x00)
            return {};

        u8 const byte = input[index++];

        // 3. If Big5 lead is not 0x00, let lead be Big5 lead, let pointer be null, set Big5 lead to 0x00, and then:
        if (big5_lead != 0x00) {
            auto lead = big5_lead;
            Optional<u32> pointer;
            big5_lead = 0x00;

            // 1. Let offset be 0x40 if byte is less than 0x7F, otherwise 0x62.
            u8 const offset = byte < 0x7F ? 0x40 : 0x62;

            // 2. If byte is in the range 0x40 to 0x7E, inclusive, or 0xA1 to 0xFE, inclusive, set pointer to (lead − 0x81) × 157 + (byte − offset).
            if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0xA1 && byte <= 0xFE))
                pointer = (lead - 0x81) * 157 + (byte - offset);

            // 3. If there is a row in the table below whose first column is pointer, return the two code points listed in its second column (the third column is irrelevant):
            if (pointer.has_value() && pointer.value() == 1133) {
                TRY(on_code_point(0x00CA));
                TRY(on_code_point(0x0304));
                continue;
            }
            if (pointer.has_value() && pointer.value() == 1135) {
                TRY(on_code_point(0x00CA));
                TRY(on_code_point(0x030C));
                continue;
            }
            if (pointer.has_value() && pointer.value() == 1164) {
                TRY(on_code_point(0x00EA));
                TRY(on_code_point(0x0304));
                continue;
            }
            if (pointer.has_value() && pointer.value() == 1166) {
                TRY(on_code_point(0x00EA));
                TRY(on_code_point(0x030C));
                continue;
            }

            // 4. Let code point be null if pointer is null, otherwise the index code point for pointer in index Big5.
            auto code_pointer = pointer.has_value() ? index_big5_code_point(pointer.value()) : Optional<u32> {};

            // 5. If code point is non-null, return a code point whose value is code point.
            if (code_pointer.has_value()) {
                TRY(on_code_point(code_pointer.value()));
                continue;
            }

            // 6. If byte is an ASCII byte, restore byte to ioQueue.
            if (byte <= 0x7F)
                index--;

            // 7. Return error.
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 4. If byte is an ASCII byte, return a code point whose value is byte.
        if (byte <= 0x7F) {
            TRY(on_code_point(byte));
            continue;
        }

        // 5. If byte is in the range 0x81 to 0xFE, inclusive, set Big5 lead to byte and return continue.
        if (byte >= 0x81 && byte <= 0xFE) {
            big5_lead = byte;
            continue;
        }

        // 6. Return error
        TRY(on_code_point(replacement_code_point));
    }
}

// https://encoding.spec.whatwg.org/#euc-jp-decoder
ErrorOr<void> EUCJPDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // EUC-JP’s decoder has an associated EUC-JP jis0212 (initially false) and EUC-JP lead (initially 0x00).
    bool jis0212 = false;
    u8 euc_jp_lead = 0x00;

    // EUC-JP’s decoder’s handler, given ioQueue and byte, runs these steps:
    size_t index = 0;
    while (true) {
        // 1. If byte is end-of-queue and EUC-JP lead is not 0x00, set EUC-JP lead to 0x00, and return error.
        if (index >= input.length() && euc_jp_lead != 0x00) {
            euc_jp_lead = 0x00;
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 2. If byte is end-of-queue and EUC-JP lead is 0x00, return finished.
        if (index >= input.length() && euc_jp_lead == 0x00)
            return {};

        u8 const byte = input[index++];

        // 3. If EUC-JP lead is 0x8E and byte is in the range 0xA1 to 0xDF, inclusive, set EUC-JP lead to 0x00 and return a code point whose value is 0xFF61 − 0xA1 + byte.
        if (euc_jp_lead == 0x8E && byte >= 0xA1 && byte <= 0xDF) {
            euc_jp_lead = 0x00;
            TRY(on_code_point(0xFF61 - 0xA1 + byte));
            continue;
        }

        // 4. If EUC-JP lead is 0x8F and byte is in the range 0xA1 to 0xFE, inclusive, set EUC-JP jis0212 to true, set EUC-JP lead to byte, and return continue.
        if (euc_jp_lead == 0x8F && byte >= 0xA1 && byte <= 0xFE) {
            jis0212 = true;
            euc_jp_lead = byte;
            continue;
        }

        // 5. If EUC-JP lead is not 0x00, let lead be EUC-JP lead, set EUC-JP lead to 0x00, and then:
        if (euc_jp_lead != 0x00) {
            auto lead = euc_jp_lead;
            euc_jp_lead = 0x00;

            // 1. Let code point be null.
            Optional<u32> code_point;

            // 2. If lead and byte are both in the range 0xA1 to 0xFE, inclusive, then set code point to the index code point for (lead − 0xA1) × 94 + byte − 0xA1 in index jis0208 if EUC-JP jis0212 is false and in index jis0212 otherwise.
            if (lead >= 0xA1 && lead <= 0xFE && byte >= 0xA1 && byte <= 0xFE) {
                auto pointer = (lead - 0xA1) * 94 + byte - 0xA1;
                code_point = jis0212 ? index_jis0212_code_point(pointer) : index_jis0208_code_point(pointer);
            }

            // 3. Set EUC-JP jis0212 to false.
            jis0212 = false;

            // 4. If code point is non-null, return a code point whose value is code point.
            if (code_point.has_value()) {
                TRY(on_code_point(code_point.value()));
                continue;
            }

            // 5. If byte is an ASCII byte, restore byte to ioQueue.
            if (byte <= 0x7F)
                index--;

            // 6. Return error.
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 6. If byte is an ASCII byte, return a code point whose value is byte.
        if (byte <= 0x7F) {
            TRY(on_code_point(byte));
            continue;
        }

        // 7. If byte is 0x8E, 0x8F, or in the range 0xA1 to 0xFE, inclusive, set EUC-JP lead to byte and return continue.
        if (byte == 0x8E || byte == 0x8F || (byte >= 0xA1 && byte <= 0xFE)) {
            euc_jp_lead = byte;
            continue;
        }

        // 8. Return error.
        TRY(on_code_point(replacement_code_point));
    }
}

enum class ISO2022JPState {
    ASCII,
    Roman,
    Katakana,
    LeadByte,
    TrailByte,
    EscapeStart,
    Escape,
};

// https://encoding.spec.whatwg.org/#iso-2022-jp-decoder
ErrorOr<void> ISO2022JPDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // ISO-2022-JP’s decoder has an associated ISO-2022-JP decoder state (initially ASCII), ISO-2022-JP decoder output state (initially ASCII), ISO-2022-JP lead (initially 0x00), and ISO-2022-JP output (initially false).
    auto decoder_state = ISO2022JPState::ASCII;
    auto output_state = ISO2022JPState::ASCII;
    u8 iso2022_jp_lead = 0x00;
    bool iso2022_jp_output = false;

    size_t index = 0;
    while (true) {
        Optional<u8> byte;
        if (index < input.length())
            byte = input[index++];

        // ISO-2022-JP’s decoder’s handler, given ioQueue and byte, runs these steps, switching on ISO-2022-JP decoder state:
        switch (decoder_state) {
        case ISO2022JPState::ASCII:
            // Based on byte:
            // 0x1B: Set ISO-2022-JP decoder state to escape start and return continue.
            if (byte == 0x1B) {
                decoder_state = ISO2022JPState::EscapeStart;
                continue;
            }

            // 0x00 to 0x7F, excluding 0x0E, 0x0F, and 0x1B: Set ISO-2022-JP output to false and return a code point whose value is byte.
            if (byte.has_value() && byte.value() <= 0x7F && byte != 0x0E && byte != 0x0F) {
                iso2022_jp_output = false;
                TRY(on_code_point(byte.value()));
                continue;
            }

            // end-of-queue: Return finished.
            if (!byte.has_value())
                return {};

            // Otherwise: Set ISO-2022-JP output to false and return error.
            iso2022_jp_output = false;
            TRY(on_code_point(replacement_code_point));
            break;
        case ISO2022JPState::Roman:
            // Based on byte:
            // 0x1B: Set ISO-2022-JP decoder state to escape start and return continue.
            if (byte == 0x1B) {
                decoder_state = ISO2022JPState::EscapeStart;
                continue;
            }

            // 0x5C: Set ISO-2022-JP output to false and return code point U+00A5.
            if (byte == 0x5C) {
                iso2022_jp_output = false;
                TRY(on_code_point(0x00A5));
                continue;
            }

            // 0x7E: Set ISO-2022-JP output to false and return code point U+203E.
            if (byte == 0x7E) {
                iso2022_jp_output = false;
                TRY(on_code_point(0x203E));
                continue;
            }

            // 0x00 to 0x7F, excluding 0x0E, 0x0F, 0x1B, 0x5C, and 0x7E: Set ISO-2022-JP output to false and return a code point whose value is byte.
            if (byte.has_value() && byte.value() <= 0x7F && byte != 0x0E && byte != 0x0F) {
                iso2022_jp_output = false;
                TRY(on_code_point(byte.value()));
                continue;
            }

            // end-of-queue: Return finished.
            if (!byte.has_value())
                return {};

            // Otherwise: Set ISO-2022-JP output to false and return error.
            iso2022_jp_output = false;
            TRY(on_code_point(replacement_code_point));
            break;
        case ISO2022JPState::Katakana:
            // Based on byte:
            // 0x1B: Set ISO-2022-JP decoder state to escape start and return continue.
            if (byte == 0x1B) {
                decoder_state = ISO2022JPState::EscapeStart;
                continue;
            }

            // 0x21 to 0x5F: Set ISO-2022-JP output to false and return a code point whose value is 0xFF61 − 0x21 + byte.
            if (byte.has_value() && byte.value() >= 0x21 && byte.value() <= 0x5F) {
                iso2022_jp_output = false;
                TRY(on_code_point(0xFF61 - 0x21 + byte.value()));
                continue;
            }

            // end-of-queue: Return finished.
            if (!byte.has_value())
                return {};

            // Otherwise: Set ISO-2022-JP output to false and return error.
            iso2022_jp_output = false;
            TRY(on_code_point(replacement_code_point));
            break;
        case ISO2022JPState::LeadByte:
            // Based on byte:
            // 0x1B: Set ISO-2022-JP decoder state to escape start and return continue.
            if (byte == 0x1B) {
                decoder_state = ISO2022JPState::EscapeStart;
                continue;
            }

            // 0x21 to 0x7E: Set ISO-2022-JP output to false, ISO-2022-JP lead to byte, ISO-2022-JP decoder state to trail byte, and return continue.
            if (byte.has_value() && byte.value() >= 0x21 && byte.value() <= 0x7E) {
                iso2022_jp_output = false;
                iso2022_jp_lead = byte.value();
                decoder_state = ISO2022JPState::TrailByte;
                continue;
            }

            // end-of-queue: Return finished.
            if (!byte.has_value())
                return {};

            // Otherwise: Set ISO-2022-JP output to false and return error.
            iso2022_jp_output = false;
            TRY(on_code_point(replacement_code_point));
            break;
        case ISO2022JPState::TrailByte:
            // Based on byte:

            // 0x1B: Set ISO-2022-JP decoder state to escape start and return error.
            if (byte == 0x1B) {
                decoder_state = ISO2022JPState::EscapeStart;
                TRY(on_code_point(replacement_code_point));
                continue;
            }

            // 0x21 to 0x7E:
            if (byte.has_value() && byte.value() >= 0x21 && byte.value() <= 0x7E) {
                // 1. Set the ISO-2022-JP decoder state to lead byte.
                decoder_state = ISO2022JPState::LeadByte;

                // 2. Let pointer be (ISO-2022-JP lead − 0x21) × 94 + byte − 0x21.
                u32 pointer = (iso2022_jp_lead - 0x21) * 94 + byte.value() - 0x21;

                // 3. Let code point be the index code point for pointer in index jis0208.
                auto code_point = index_jis0208_code_point(pointer);

                // 4. If code point is null, return error.
                if (!code_point.has_value()) {
                    TRY(on_code_point(replacement_code_point));
                    continue;
                }

                // 5. Return a code point whose value is code point.
                TRY(on_code_point(code_point.value()));
                continue;
            }

            // end-of-queue: Set the ISO-2022-JP decoder state to lead byte and return error.
            if (!byte.has_value()) {
                decoder_state = ISO2022JPState::LeadByte;
                TRY(on_code_point(replacement_code_point));
                continue;
            }

            // Otherwise: Set ISO-2022-JP decoder state to lead byte and return error.
            decoder_state = ISO2022JPState::LeadByte;
            TRY(on_code_point(replacement_code_point));
            break;
        case ISO2022JPState::EscapeStart:
            // 1. If byte is either 0x24 or 0x28, set ISO-2022-JP lead to byte, ISO-2022-JP decoder state to escape, and return continue.
            if (byte == 0x24 || byte == 0x28) {
                iso2022_jp_lead = byte.value();
                decoder_state = ISO2022JPState::Escape;
                continue;
            }

            // 2. If byte is not end-of-queue, then restore byte to ioQueue.
            if (byte.has_value())
                index--;

            // 3. Set ISO-2022-JP output to false, ISO-2022-JP decoder state to ISO-2022-JP decoder output state, and return error.
            iso2022_jp_output = false;
            decoder_state = output_state;
            TRY(on_code_point(replacement_code_point));
            break;
        case ISO2022JPState::Escape: {
            // 1. Let lead be ISO-2022-JP lead and set ISO-2022-JP lead to 0x00.
            auto lead = iso2022_jp_lead;
            iso2022_jp_lead = 0x00;

            // 2. Let state be null.
            Optional<ISO2022JPState> state;

            // 3. If lead is 0x28 and byte is 0x42, set state to ASCII.
            if (lead == 0x28 && byte == 0x42)
                state = ISO2022JPState::ASCII;

            // 4. If lead is 0x28 and byte is 0x4A, set state to Roman.
            if (lead == 0x28 && byte == 0x4A)
                state = ISO2022JPState::Roman;

            // 5. If lead is 0x28 and byte is 0x49, set state to katakana.
            if (lead == 0x28 && byte == 0x49)
                state = ISO2022JPState::Katakana;

            // 6. If lead is 0x24 and byte is either 0x40 or 0x42, set state to lead byte.
            if (lead == 0x24 && (byte == 0x40 || byte == 0x42))
                state = ISO2022JPState::LeadByte;

            // 7. If state is non-null, then:
            if (state.has_value()) {
                // 1. Set ISO-2022-JP decoder state and ISO-2022-JP decoder output state to state.
                decoder_state = state.value();
                output_state = state.value();

                // 2. Let output be the value of ISO-2022-JP output.
                auto output = iso2022_jp_output;

                // 3. Set ISO-2022-JP output to true.
                iso2022_jp_output = true;

                // 4. Return continue, if output is false, and error otherwise.
                if (output)
                    TRY(on_code_point(replacement_code_point));
                continue;
            }

            // 8. If byte is end-of-queue, then restore lead to ioQueue; otherwise, restore « lead, byte » to ioQueue.
            index -= byte.has_value() ? 2 : 1;

            // 9. Set ISO-2022-JP output to false, ISO-2022-JP decoder state to ISO-2022-JP decoder output state and return error.
            iso2022_jp_output = false;
            decoder_state = output_state;
            TRY(on_code_point(replacement_code_point));
            break;
        }
        }
    }
}

// https://encoding.spec.whatwg.org/#shift_jis-decoder
ErrorOr<void> ShiftJISDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // Shift_JIS’s decoder has an associated Shift_JIS lead (initially 0x00).
    u8 shift_jis_lead = 0x00;

    // Shift_JIS’s decoder’s handler, given ioQueue and byte, runs these steps:
    size_t index = 0;
    while (true) {
        // 1. If byte is end-of-queue and Shift_JIS lead is not 0x00, set Shift_JIS lead to 0x00 and return error.
        if (index >= input.length() && shift_jis_lead != 0x00) {
            shift_jis_lead = 0x00;
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 2. If byte is end-of-queue and Shift_JIS lead is 0x00, return finished.
        if (index >= input.length() && shift_jis_lead == 0x00)
            return {};

        u8 const byte = input[index++];

        // 3. If Shift_JIS lead is not 0x00, let lead be Shift_JIS lead, let pointer be null, set Shift_JIS lead to 0x00, and then:
        if (shift_jis_lead != 0x00) {
            auto lead = shift_jis_lead;
            Optional<u32> pointer;
            shift_jis_lead = 0x00;

            // 1. Let offset be 0x40 if byte is less than 0x7F, otherwise 0x41.
            u8 const offset = byte < 0x7F ? 0x40 : 0x41;

            // 2. Let lead offset be 0x81 if lead is less than 0xA0, otherwise 0xC1.
            u8 const lead_offset = lead < 0xA0 ? 0x81 : 0xC1;

            // 3. If byte is in the range 0x40 to 0x7E, inclusive, or 0x80 to 0xFC, inclusive, set pointer to (lead − lead offset) × 188 + byte − offset.
            if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFC))
                pointer = (lead - lead_offset) * 188 + byte - offset;

            // 4. If pointer is in the range 8836 to 10715, inclusive, return a code point whose value is 0xE000 − 8836 + pointer.
            if (pointer.has_value() && pointer.value() >= 8836 && pointer.value() <= 10715) {
                TRY(on_code_point(0xE000 - 8836 + pointer.value()));
                continue;
            }

            // 5. Let code point be null if pointer is null, otherwise the index code point for pointer in index jis0208.
            auto code_point = pointer.has_value() ? index_jis0208_code_point(pointer.value()) : Optional<u32> {};

            // 6. If code point is non-null, return a code point whose value is code point.
            if (code_point.has_value()) {
                TRY(on_code_point(code_point.value()));
                continue;
            }

            // 7. If byte is an ASCII byte, restore byte to ioQueue.
            if (byte <= 0x7F)
                index--;

            // 8. Return error.
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 4. If byte is an ASCII byte or 0x80, return a code point whose value is byte.
        if (byte <= 0x80) {
            TRY(on_code_point(byte));
            continue;
        }

        // 5. If byte is in the range 0xA1 to 0xDF, inclusive, return a code point whose value is 0xFF61 − 0xA1 + byte.
        if (byte >= 0xA1 && byte <= 0xDF) {
            TRY(on_code_point(0xFF61 - 0xA1 + byte));
            continue;
        }

        // 6. If byte is in the range 0x81 to 0x9F, inclusive, or 0xE0 to 0xFC, inclusive, set Shift_JIS lead to byte and return continue.
        if ((byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC)) {
            shift_jis_lead = byte;
            continue;
        }

        // 7. Return error.
        TRY(on_code_point(replacement_code_point));
    }
}

// https://encoding.spec.whatwg.org/#euc-kr-decoder
ErrorOr<void> EUCKRDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // EUC-KR’s decoder has an associated EUC-KR lead (initially 0x00).
    u8 euc_kr_lead = 0x00;

    // EUC-KR’s decoder’s handler, given ioQueue and byte, runs these steps:
    size_t index = 0;
    while (true) {
        // 1. If byte is end-of-queue and EUC-KR lead is not 0x00, set EUC-KR lead to 0x00 and return error.
        if (index >= input.length() && euc_kr_lead != 0x00) {
            euc_kr_lead = 0x00;
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 2. If byte is end-of-queue and EUC-KR lead is 0x00, return finished.
        if (index >= input.length() && euc_kr_lead == 0x00)
            return {};

        u8 const byte = input[index++];

        // 3. If EUC-KR lead is not 0x00, let lead be EUC-KR lead, let pointer be null, set EUC-KR lead to 0x00, and then:
        if (euc_kr_lead != 0x00) {
            auto lead = euc_kr_lead;
            Optional<u32> pointer;
            euc_kr_lead = 0x00;

            // 1. If byte is in the range 0x41 to 0xFE, inclusive, set pointer to (lead − 0x81) × 190 + (byte − 0x41).
            if (byte >= 0x41 && byte <= 0xFE)
                pointer = (lead - 0x81) * 190 + (byte - 0x41);

            // 2. Let code point be null if pointer is null, otherwise the index code point for pointer in index EUC-KR.
            auto code_point = pointer.has_value() ? index_euc_kr_code_point(pointer.value()) : Optional<u32> {};

            // 3. If code point is non-null, return a code point whose value is code point.
            if (code_point.has_value()) {
                TRY(on_code_point(code_point.value()));
                continue;
            }

            // 4. If byte is an ASCII byte, restore byte to ioQueue.
            if (byte <= 0x7F)
                index--;

            // 5. Return error.
            TRY(on_code_point(replacement_code_point));
            continue;
        }

        // 4. If byte is an ASCII byte, return a code point whose value is byte.
        if (byte <= 0x7F) {
            TRY(on_code_point(byte));
            continue;
        }

        // 5. If byte is in the range 0x81 to 0xFE, inclusive, set EUC-KR lead to byte and return continue.
        if (byte >= 0x81 && byte <= 0xFE) {
            euc_kr_lead = byte;
            continue;
        }

        // 6. Return error.
        TRY(on_code_point(replacement_code_point));
    }
}

// https://encoding.spec.whatwg.org/#replacement-decoder
ErrorOr<void> ReplacementDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    // replacement’s decoder has an associated replacement error returned (initially false).
    // replacement’s decoder’s handler, given ioQueue and byte, runs these steps:
    // 1. If byte is end-of-queue, return finished.
    // 2. If replacement error returned is false, set replacement error returned to true and return error.
    // 3. Return finished.

    if (!input.is_empty())
        return on_code_point(replacement_code_point);
    return {};
}

}
