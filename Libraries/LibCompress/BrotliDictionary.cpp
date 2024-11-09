/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Types.h>
#include <LibCompress/BrotliDictionary.h>

// Include the 119.9 KiB of dictionary data from a binary file
extern u8 const brotli_dictionary_data[];
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
asm(".const_data\n"
    ".globl _brotli_dictionary_data\n"
    "_brotli_dictionary_data:\n");
#elif defined(AK_OS_EMSCRIPTEN)
asm(".section .data, \"\",@\n"
    ".global brotli_dictionary_data\n"
    "brotli_dictionary_data:\n");
#else
asm(".section .rodata\n"
    ".global brotli_dictionary_data\n"
    "brotli_dictionary_data:\n");
#endif
asm(".incbin \"" __FILE__ ".dict.bin\"\n"
#if (!defined(AK_OS_WINDOWS) && !defined(AK_OS_EMSCRIPTEN))
    ".previous\n");
#else
);
#endif

namespace Compress {

static size_t const bits_by_length[25] {
    0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5
};

static size_t const offset_by_length[25] {
    0, 0, 0, 0, 0, 4096, 9216, 21504, 35840, 44032, 53248, 63488, 74752, 87040, 93696, 100864,
    104704, 106752, 108928, 113536, 115968, 118528, 119872, 121280, 122016
};

static int ferment(Bytes word, size_t pos)
{
    if (word[pos] < 192) {
        if (word[pos] >= 97 && word[pos] <= 122) {
            word[pos] = word[pos] ^ 32;
        }
        return 1;
    } else if (word[pos] < 224) {
        if (pos + 1 < word.size()) {
            word[pos + 1] = word[pos + 1] ^ 32;
        }
        return 2;
    } else {
        if (pos + 2 < word.size()) {
            word[pos + 2] = word[pos + 2] ^ 5;
        }
        return 3;
    }
}

static void ferment_first(Bytes word)
{
    if (word.size() > 0) {
        ferment(word, 0);
    }
}

[[maybe_unused]] static void ferment_all(Bytes word)
{
    size_t i = 0;
    while (i < word.size()) {
        i += ferment(word, i);
    }
}

using BrotliDictionary::TransformationOperation::FermentAll;
using BrotliDictionary::TransformationOperation::FermentFirst;
using BrotliDictionary::TransformationOperation::Identity;
using BrotliDictionary::TransformationOperation::OmitFirst;
using BrotliDictionary::TransformationOperation::OmitLast;
constexpr static BrotliDictionary::Transformation transformations[121] {
    //                                            ID       Prefix     Transform            Suffix
    //                                            --       ------     ---------            ------
    { ""sv, Identity, 0, ""sv },              //   0           ""     Identity                 ""
    { ""sv, Identity, 0, " "sv },             //   1           ""     Identity                " "
    { " "sv, Identity, 0, " "sv },            //   2          " "     Identity                " "
    { ""sv, OmitFirst, 1, ""sv },             //   3           ""     OmitFirst1               ""
    { ""sv, FermentFirst, 0, " "sv },         //   4           ""     FermentFirst            " "
    { ""sv, Identity, 0, " the "sv },         //   5           ""     Identity            " the "
    { " "sv, Identity, 0, ""sv },             //   6          " "     Identity                 ""
    { "s "sv, Identity, 0, " "sv },           //   7         "s "     Identity                " "
    { ""sv, Identity, 0, " of "sv },          //   8           ""     Identity             " of "
    { ""sv, FermentFirst, 0, ""sv },          //   9           ""     FermentFirst             ""
    { ""sv, Identity, 0, " and "sv },         //  10           ""     Identity            " and "
    { ""sv, OmitFirst, 2, ""sv },             //  11           ""     OmitFirst2               ""
    { ""sv, OmitLast, 1, ""sv },              //  12           ""     OmitLast1                ""
    { ", "sv, Identity, 0, " "sv },           //  13         ", "     Identity                " "
    { ""sv, Identity, 0, ", "sv },            //  14           ""     Identity               ", "
    { " "sv, FermentFirst, 0, " "sv },        //  15          " "     FermentFirst            " "
    { ""sv, Identity, 0, " in "sv },          //  16           ""     Identity             " in "
    { ""sv, Identity, 0, " to "sv },          //  17           ""     Identity             " to "
    { "e "sv, Identity, 0, " "sv },           //  18         "e "     Identity                " "
    { ""sv, Identity, 0, "\""sv },            //  19           ""     Identity               "\""
    { ""sv, Identity, 0, "."sv },             //  20           ""     Identity                "."
    { ""sv, Identity, 0, "\">"sv },           //  21           ""     Identity              "\">"
    { ""sv, Identity, 0, "\n"sv },            //  22           ""     Identity               "\n"
    { ""sv, OmitLast, 3, ""sv },              //  23           ""     OmitLast3                ""
    { ""sv, Identity, 0, "]"sv },             //  24           ""     Identity                "]"
    { ""sv, Identity, 0, " for "sv },         //  25           ""     Identity            " for "
    { ""sv, OmitFirst, 3, ""sv },             //  26           ""     OmitFirst3               ""
    { ""sv, OmitLast, 2, ""sv },              //  27           ""     OmitLast2                ""
    { ""sv, Identity, 0, " a "sv },           //  28           ""     Identity              " a "
    { ""sv, Identity, 0, " that "sv },        //  29           ""     Identity           " that "
    { " "sv, FermentFirst, 0, ""sv },         //  30          " "     FermentFirst             ""
    { ""sv, Identity, 0, ". "sv },            //  31           ""     Identity               ". "
    { "."sv, Identity, 0, ""sv },             //  32          "."     Identity                 ""
    { " "sv, Identity, 0, ", "sv },           //  33          " "     Identity               ", "
    { ""sv, OmitFirst, 4, ""sv },             //  34           ""     OmitFirst4               ""
    { ""sv, Identity, 0, " with "sv },        //  35           ""     Identity           " with "
    { ""sv, Identity, 0, "'"sv },             //  36           ""     Identity                "'"
    { ""sv, Identity, 0, " from "sv },        //  37           ""     Identity           " from "
    { ""sv, Identity, 0, " by "sv },          //  38           ""     Identity             " by "
    { ""sv, OmitFirst, 5, ""sv },             //  39           ""     OmitFirst5               ""
    { ""sv, OmitFirst, 6, ""sv },             //  40           ""     OmitFirst6               ""
    { " the "sv, Identity, 0, ""sv },         //  41      " the "     Identity                 ""
    { ""sv, OmitLast, 4, ""sv },              //  42           ""     OmitLast4                ""
    { ""sv, Identity, 0, ". The "sv },        //  43           ""     Identity           ". The "
    { ""sv, FermentAll, 0, ""sv },            //  44           ""     FermentAll               ""
    { ""sv, Identity, 0, " on "sv },          //  45           ""     Identity             " on "
    { ""sv, Identity, 0, " as "sv },          //  46           ""     Identity             " as "
    { ""sv, Identity, 0, " is "sv },          //  47           ""     Identity             " is "
    { ""sv, OmitLast, 7, ""sv },              //  48           ""     OmitLast7                ""
    { ""sv, OmitLast, 1, "ing "sv },          //  49           ""     OmitLast1            "ing "
    { ""sv, Identity, 0, "\n\t"sv },          //  50           ""     Identity             "\n\t"
    { ""sv, Identity, 0, ":"sv },             //  51           ""     Identity                ":"
    { " "sv, Identity, 0, ". "sv },           //  52          " "     Identity               ". "
    { ""sv, Identity, 0, "ed "sv },           //  53           ""     Identity              "ed "
    { ""sv, OmitFirst, 9, ""sv },             //  54           ""     OmitFirst9               ""
    { ""sv, OmitFirst, 7, ""sv },             //  55           ""     OmitFirst7               ""
    { ""sv, OmitLast, 6, ""sv },              //  56           ""     OmitLast6                ""
    { ""sv, Identity, 0, "("sv },             //  57           ""     Identity                "("
    { ""sv, FermentFirst, 0, ", "sv },        //  58           ""     FermentFirst           ", "
    { ""sv, OmitLast, 8, ""sv },              //  59           ""     OmitLast8                ""
    { ""sv, Identity, 0, " at "sv },          //  60           ""     Identity             " at "
    { ""sv, Identity, 0, "ly "sv },           //  61           ""     Identity              "ly "
    { " the "sv, Identity, 0, " of "sv },     //  62      " the "     Identity             " of "
    { ""sv, OmitLast, 5, ""sv },              //  63           ""     OmitLast5                ""
    { ""sv, OmitLast, 9, ""sv },              //  64           ""     OmitLast9                ""
    { " "sv, FermentFirst, 0, ", "sv },       //  65          " "     FermentFirst           ", "
    { ""sv, FermentFirst, 0, "\""sv },        //  66           ""     FermentFirst           "\""
    { "."sv, Identity, 0, "("sv },            //  67          "."     Identity                "("
    { ""sv, FermentAll, 0, " "sv },           //  68           ""     FermentAll            " "
    { ""sv, FermentFirst, 0, "\">"sv },       //  69           ""     FermentFirst          "\">"
    { ""sv, Identity, 0, "=\""sv },           //  70           ""     Identity              "=\""
    { " "sv, Identity, 0, "."sv },            //  71          " "     Identity                "."
    { ".com/"sv, Identity, 0, ""sv },         //  72      ".com/"     Identity                 ""
    { " the "sv, Identity, 0, " of the "sv }, //  73      " the "     Identity         " of the "
    { ""sv, FermentFirst, 0, "'"sv },         //  74           ""     FermentFirst            "'"
    { ""sv, Identity, 0, ". This "sv },       //  75           ""     Identity          ". This "
    { ""sv, Identity, 0, ","sv },             //  76           ""     Identity                ","
    { "."sv, Identity, 0, " "sv },            //  77          "."     Identity                " "
    { ""sv, FermentFirst, 0, "("sv },         //  78           ""     FermentFirst            "("
    { ""sv, FermentFirst, 0, "."sv },         //  79           ""     FermentFirst            "."
    { ""sv, Identity, 0, " not "sv },         //  80           ""     Identity            " not "
    { " "sv, Identity, 0, "=\""sv },          //  81          " "     Identity              "=\""
    { ""sv, Identity, 0, "er "sv },           //  82           ""     Identity              "er "
    { " "sv, FermentAll, 0, " "sv },          //  83          " "     FermentAll              " "
    { ""sv, Identity, 0, "al "sv },           //  84           ""     Identity              "al "
    { " "sv, FermentAll, 0, ""sv },           //  85          " "     FermentAll               ""
    { ""sv, Identity, 0, "='"sv },            //  86           ""     Identity               "='"
    { ""sv, FermentAll, 0, "\""sv },          //  87           ""     FermentAll             "\""
    { ""sv, FermentFirst, 0, ". "sv },        //  88           ""     FermentFirst           ". "
    { " "sv, Identity, 0, "("sv },            //  89          " "     Identity                "("
    { ""sv, Identity, 0, "ful "sv },          //  90           ""     Identity             "ful "
    { " "sv, FermentFirst, 0, ". "sv },       //  91          " "     FermentFirst           ". "
    { ""sv, Identity, 0, "ive "sv },          //  92           ""     Identity             "ive "
    { ""sv, Identity, 0, "less "sv },         //  93           ""     Identity            "less "
    { ""sv, FermentAll, 0, "'"sv },           //  94           ""     FermentAll              "'"
    { ""sv, Identity, 0, "est "sv },          //  95           ""     Identity             "est "
    { " "sv, FermentFirst, 0, "."sv },        //  96          " "     FermentFirst            "."
    { ""sv, FermentAll, 0, "\">"sv },         //  97           ""     FermentAll            "\">"
    { " "sv, Identity, 0, "='"sv },           //  98          " "     Identity               "='"
    { ""sv, FermentFirst, 0, ","sv },         //  99           ""     FermentFirst            ","
    { ""sv, Identity, 0, "ize "sv },          // 100           ""     Identity             "ize "
    { ""sv, FermentAll, 0, "."sv },           // 101           ""     FermentAll              "."
    { "\xc2\xa0"sv, Identity, 0, ""sv },      // 102   "\xc2\xa0"     Identity                 ""
    { " "sv, Identity, 0, ","sv },            // 103          " "     Identity                ","
    { ""sv, FermentFirst, 0, "=\""sv },       // 104           ""     FermentFirst          "=\""
    { ""sv, FermentAll, 0, "=\""sv },         // 105           ""     FermentAll            "=\""
    { ""sv, Identity, 0, "ous "sv },          // 106           ""     Identity             "ous "
    { ""sv, FermentAll, 0, ", "sv },          // 107           ""     FermentAll             ", "
    { ""sv, FermentFirst, 0, "='"sv },        // 108           ""     FermentFirst           "='"
    { " "sv, FermentFirst, 0, ","sv },        // 109          " "     FermentFirst            ","
    { " "sv, FermentAll, 0, "=\""sv },        // 110          " "     FermentAll            "=\""
    { " "sv, FermentAll, 0, ", "sv },         // 111          " "     FermentAll             ", "
    { ""sv, FermentAll, 0, ","sv },           // 112           ""     FermentAll              ","
    { ""sv, FermentAll, 0, "("sv },           // 113           ""     FermentAll              "("
    { ""sv, FermentAll, 0, ". "sv },          // 114           ""     FermentAll             ". "
    { " "sv, FermentAll, 0, "."sv },          // 115          " "     FermentAll              "."
    { ""sv, FermentAll, 0, "='"sv },          // 116           ""     FermentAll             "='"
    { " "sv, FermentAll, 0, ". "sv },         // 117          " "     FermentAll             ". "
    { " "sv, FermentFirst, 0, "=\""sv },      // 118          " "     FermentFirst          "=\""
    { " "sv, FermentAll, 0, "='"sv },         // 119          " "     FermentAll             "='"
    { " "sv, FermentFirst, 0, "='"sv },       // 120          " "     FermentFirst           "='"
};

ErrorOr<ByteBuffer> BrotliDictionary::lookup_word(size_t index, size_t length)
{
    if (length < 4 || length > 24)
        return Error::from_string_literal("invalid dictionary lookup length");

    size_t word_index = index % (1 << bits_by_length[length]);
    ReadonlyBytes base_word { brotli_dictionary_data + offset_by_length[length] + (word_index * length), length };
    size_t transform_id = index >> bits_by_length[length];

    if (transform_id >= 121)
        return Error::from_string_literal("invalid dictionary transformation");

    auto transformation = transformations[transform_id];
    ByteBuffer bb;
    bb.append(transformation.prefix.bytes());
    size_t prefix_length = bb.size();

    switch (transformation.operation) {
    case TransformationOperation::Identity:
        bb.append(base_word);
        break;
    case TransformationOperation::FermentFirst:
        bb.append(base_word);
        ferment_first(bb.bytes().slice(prefix_length));
        break;
    case TransformationOperation::FermentAll:
        bb.append(base_word);
        ferment_all(bb.bytes().slice(prefix_length));
        break;
    case TransformationOperation::OmitFirst:
        if (transformation.operation_data < base_word.size())
            bb.append(base_word.slice(transformation.operation_data));
        break;
    case TransformationOperation::OmitLast:
        if (transformation.operation_data < base_word.size())
            bb.append(base_word.slice(0, base_word.size() - transformation.operation_data));
        break;
    }

    bb.append(transformation.suffix.bytes());
    return bb;
}

}
