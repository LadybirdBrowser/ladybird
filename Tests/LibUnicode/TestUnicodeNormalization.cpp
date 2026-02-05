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
    EXPECT_EQ(normalize(""sv, NormalizationForm::NFD), ""sv);

    EXPECT_EQ(normalize("Hello"sv, NormalizationForm::NFD), "Hello"sv);

    EXPECT_EQ(normalize("Amélie"sv, NormalizationForm::NFD), "Ame\u0301lie"sv);

    EXPECT_EQ(normalize("Oﬀice"sv, NormalizationForm::NFD), "Oﬀice"sv);

    EXPECT_EQ(normalize("\u1E9B\u0323"sv, NormalizationForm::NFD), "\u017F\u0323\u0307"sv);

    EXPECT_EQ(normalize("\u0112\u0300"sv, NormalizationForm::NFD), "\u0045\u0304\u0300"sv);

    EXPECT_EQ(normalize("\u03D3"sv, NormalizationForm::NFD), "\u03D2\u0301"sv);
    EXPECT_EQ(normalize("\u03D4"sv, NormalizationForm::NFD), "\u03D2\u0308"sv);

    EXPECT_EQ(normalize("닭"sv, NormalizationForm::NFD), "\u1103\u1161\u11B0"sv);
    EXPECT_EQ(normalize("\u1100\uAC00\u11A8"sv, NormalizationForm::NFD), "\u1100\u1100\u1161\u11A8"sv);

    // Composition exclusions.
    EXPECT_EQ(normalize("\u0958"sv, NormalizationForm::NFD), "\u0915\u093C"sv);
    EXPECT_EQ(normalize("\u2126"sv, NormalizationForm::NFD), "\u03A9"sv);
}

TEST_CASE(normalize_nfc)
{
    EXPECT_EQ(normalize(""sv, NormalizationForm::NFC), ""sv);

    EXPECT_EQ(normalize("Hello"sv, NormalizationForm::NFC), "Hello"sv);

    EXPECT_EQ(normalize("Office"sv, NormalizationForm::NFC), "Office"sv);

    EXPECT_EQ(normalize("\u1E9B\u0323"sv, NormalizationForm::NFC), "\u1E9B\u0323"sv);
    EXPECT_EQ(normalize("\u0044\u0307"sv, NormalizationForm::NFC), "\u1E0A"sv);

    EXPECT_EQ(normalize("\u0044\u0307\u0323"sv, NormalizationForm::NFC), "\u1E0C\u0307"sv);
    EXPECT_EQ(normalize("\u0044\u0323\u0307"sv, NormalizationForm::NFC), "\u1E0C\u0307"sv);

    EXPECT_EQ(normalize("\u0112\u0300"sv, NormalizationForm::NFC), "\u1E14"sv);
    EXPECT_EQ(normalize("\u1E14\u0304"sv, NormalizationForm::NFC), "\u1E14\u0304"sv);

    EXPECT_EQ(normalize("\u05B8\u05B9\u05B1\u0591\u05C3\u05B0\u05AC\u059F"sv, NormalizationForm::NFC), "\u05B1\u05B8\u05B9\u0591\u05C3\u05B0\u05AC\u059F"sv);
    EXPECT_EQ(normalize("\u0592\u05B7\u05BC\u05A5\u05B0\u05C0\u05C4\u05AD"sv, NormalizationForm::NFC), "\u05B0\u05B7\u05BC\u05A5\u0592\u05C0\u05AD\u05C4"sv);

    EXPECT_EQ(normalize("\u03D3"sv, NormalizationForm::NFC), "\u03D3"sv);
    EXPECT_EQ(normalize("\u03D4"sv, NormalizationForm::NFC), "\u03D4"sv);

    EXPECT_EQ(normalize("\u0958"sv, NormalizationForm::NFC), "\u0915\u093C"sv);
    EXPECT_EQ(normalize("\u2126"sv, NormalizationForm::NFC), "\u03A9"sv);

    EXPECT_EQ(normalize("\u1103\u1161\u11B0"sv, NormalizationForm::NFC), "닭"sv);
    EXPECT_EQ(normalize("\u1100\uAC00\u11A8"sv, NormalizationForm::NFC), "\u1100\uAC01"sv);
    EXPECT_EQ(normalize("\u1103\u1161\u11B0\u11B0"sv, NormalizationForm::NFC), "닭\u11B0");
}

TEST_CASE(normalize_nfkd)
{
    EXPECT_EQ(normalize(""sv, NormalizationForm::NFKD), ""sv);

    EXPECT_EQ(normalize("Oﬀice"sv, NormalizationForm::NFKD), "Office"sv);

    EXPECT_EQ(normalize("¼"sv, NormalizationForm::NFKD), "1\u20444"sv);

    EXPECT_EQ(normalize("\u03D3"sv, NormalizationForm::NFKD), "\u03A5\u0301"sv);
    EXPECT_EQ(normalize("\u03D4"sv, NormalizationForm::NFKD), "\u03A5\u0308"sv);

    EXPECT_EQ(normalize("\u0958"sv, NormalizationForm::NFKD), "\u0915\u093C"sv);
    EXPECT_EQ(normalize("\u2126"sv, NormalizationForm::NFKD), "\u03A9"sv);

    EXPECT_EQ(normalize("\uFDFA"sv, NormalizationForm::NFKD), "\u0635\u0644\u0649\u0020\u0627\u0644\u0644\u0647\u0020\u0639\u0644\u064A\u0647\u0020\u0648\u0633\u0644\u0645"sv);
}

TEST_CASE(normalize_nfkc)
{
    EXPECT_EQ(normalize(""sv, NormalizationForm::NFKC), ""sv);

    EXPECT_EQ(normalize("\u03D3"sv, NormalizationForm::NFKC), "\u038E"sv);
    EXPECT_EQ(normalize("\u03D4"sv, NormalizationForm::NFKC), "\u03AB"sv);

    EXPECT_EQ(normalize("\u0958"sv, NormalizationForm::NFKC), "\u0915\u093C"sv);
    EXPECT_EQ(normalize("\u2126"sv, NormalizationForm::NFKC), "\u03A9"sv);
}
