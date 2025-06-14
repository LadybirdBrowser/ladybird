/*
 * Copyright (c) 2022, mat
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibUnicode/Normalize.h>

using namespace Unicode;

TEST_CASE(normalize_nfd)
{
    EXPECT_EQ(normalize(""_sv, NormalizationForm::NFD), ""_sv);

    EXPECT_EQ(normalize("Hello"_sv, NormalizationForm::NFD), "Hello"_sv);

    EXPECT_EQ(normalize("Amélie"_sv, NormalizationForm::NFD), "Ame\u0301lie"_sv);

    EXPECT_EQ(normalize("Oﬀice"_sv, NormalizationForm::NFD), "Oﬀice"_sv);

    EXPECT_EQ(normalize("\u1E9B\u0323"_sv, NormalizationForm::NFD), "\u017F\u0323\u0307"_sv);

    EXPECT_EQ(normalize("\u0112\u0300"_sv, NormalizationForm::NFD), "\u0045\u0304\u0300"_sv);

    EXPECT_EQ(normalize("\u03D3"_sv, NormalizationForm::NFD), "\u03D2\u0301"_sv);
    EXPECT_EQ(normalize("\u03D4"_sv, NormalizationForm::NFD), "\u03D2\u0308"_sv);

    EXPECT_EQ(normalize("닭"_sv, NormalizationForm::NFD), "\u1103\u1161\u11B0"_sv);
    EXPECT_EQ(normalize("\u1100\uAC00\u11A8"_sv, NormalizationForm::NFD), "\u1100\u1100\u1161\u11A8"_sv);

    // Composition exclusions.
    EXPECT_EQ(normalize("\u0958"_sv, NormalizationForm::NFD), "\u0915\u093C"_sv);
    EXPECT_EQ(normalize("\u2126"_sv, NormalizationForm::NFD), "\u03A9"_sv);
}

TEST_CASE(normalize_nfc)
{
    EXPECT_EQ(normalize(""_sv, NormalizationForm::NFC), ""_sv);

    EXPECT_EQ(normalize("Hello"_sv, NormalizationForm::NFC), "Hello"_sv);

    EXPECT_EQ(normalize("Office"_sv, NormalizationForm::NFC), "Office"_sv);

    EXPECT_EQ(normalize("\u1E9B\u0323"_sv, NormalizationForm::NFC), "\u1E9B\u0323"_sv);
    EXPECT_EQ(normalize("\u0044\u0307"_sv, NormalizationForm::NFC), "\u1E0A"_sv);

    EXPECT_EQ(normalize("\u0044\u0307\u0323"_sv, NormalizationForm::NFC), "\u1E0C\u0307"_sv);
    EXPECT_EQ(normalize("\u0044\u0323\u0307"_sv, NormalizationForm::NFC), "\u1E0C\u0307"_sv);

    EXPECT_EQ(normalize("\u0112\u0300"_sv, NormalizationForm::NFC), "\u1E14"_sv);
    EXPECT_EQ(normalize("\u1E14\u0304"_sv, NormalizationForm::NFC), "\u1E14\u0304"_sv);

    EXPECT_EQ(normalize("\u05B8\u05B9\u05B1\u0591\u05C3\u05B0\u05AC\u059F"_sv, NormalizationForm::NFC), "\u05B1\u05B8\u05B9\u0591\u05C3\u05B0\u05AC\u059F"_sv);
    EXPECT_EQ(normalize("\u0592\u05B7\u05BC\u05A5\u05B0\u05C0\u05C4\u05AD"_sv, NormalizationForm::NFC), "\u05B0\u05B7\u05BC\u05A5\u0592\u05C0\u05AD\u05C4"_sv);

    EXPECT_EQ(normalize("\u03D3"_sv, NormalizationForm::NFC), "\u03D3"_sv);
    EXPECT_EQ(normalize("\u03D4"_sv, NormalizationForm::NFC), "\u03D4"_sv);

    EXPECT_EQ(normalize("\u0958"_sv, NormalizationForm::NFC), "\u0915\u093C"_sv);
    EXPECT_EQ(normalize("\u2126"_sv, NormalizationForm::NFC), "\u03A9"_sv);

    EXPECT_EQ(normalize("\u1103\u1161\u11B0"_sv, NormalizationForm::NFC), "닭"_sv);
    EXPECT_EQ(normalize("\u1100\uAC00\u11A8"_sv, NormalizationForm::NFC), "\u1100\uAC01"_sv);
    EXPECT_EQ(normalize("\u1103\u1161\u11B0\u11B0"_sv, NormalizationForm::NFC), "닭\u11B0");
}

TEST_CASE(normalize_nfkd)
{
    EXPECT_EQ(normalize(""_sv, NormalizationForm::NFKD), ""_sv);

    EXPECT_EQ(normalize("Oﬀice"_sv, NormalizationForm::NFKD), "Office"_sv);

    EXPECT_EQ(normalize("¼"_sv, NormalizationForm::NFKD), "1\u20444"_sv);

    EXPECT_EQ(normalize("\u03D3"_sv, NormalizationForm::NFKD), "\u03A5\u0301"_sv);
    EXPECT_EQ(normalize("\u03D4"_sv, NormalizationForm::NFKD), "\u03A5\u0308"_sv);

    EXPECT_EQ(normalize("\u0958"_sv, NormalizationForm::NFKD), "\u0915\u093C"_sv);
    EXPECT_EQ(normalize("\u2126"_sv, NormalizationForm::NFKD), "\u03A9"_sv);

    EXPECT_EQ(normalize("\uFDFA"_sv, NormalizationForm::NFKD), "\u0635\u0644\u0649\u0020\u0627\u0644\u0644\u0647\u0020\u0639\u0644\u064A\u0647\u0020\u0648\u0633\u0644\u0645"_sv);
}

TEST_CASE(normalize_nfkc)
{
    EXPECT_EQ(normalize(""_sv, NormalizationForm::NFKC), ""_sv);

    EXPECT_EQ(normalize("\u03D3"_sv, NormalizationForm::NFKC), "\u038E"_sv);
    EXPECT_EQ(normalize("\u03D4"_sv, NormalizationForm::NFKC), "\u03AB"_sv);

    EXPECT_EQ(normalize("\u0958"_sv, NormalizationForm::NFKC), "\u0915\u093C"_sv);
    EXPECT_EQ(normalize("\u2126"_sv, NormalizationForm::NFKC), "\u03A9"_sv);
}
