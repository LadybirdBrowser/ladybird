/*
 * Copyright (c) 2022, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibTest/TestCase.h>

#define EXPECT_DATETIME(sv, y, mo, d, h, mi, s) \
    EXPECT_EQ(Crypto::ASN1::parse_utc_time(sv).value(), UnixDateTime::from_unix_time_parts(y, mo, d, h, mi, s, 0))

TEST_CASE(test_utc_boring)
{
    // YYMMDDhhmm[ss]Z
    EXPECT_DATETIME("010101010101Z"sv, 2001, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("010203040506Z"sv, 2001, 2, 3, 4, 5, 6);
    EXPECT_DATETIME("020406081012Z"sv, 2002, 4, 6, 8, 10, 12);
    EXPECT_DATETIME("0204060810Z"sv, 2002, 4, 6, 8, 10, 0);
    EXPECT_DATETIME("220911220000Z"sv, 2022, 9, 11, 22, 0, 0);
}

TEST_CASE(test_utc_year_rollover)
{
    // YYMMDDhhmm[ss]Z
    EXPECT_DATETIME("000101010101Z"sv, 2000, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("010101010101Z"sv, 2001, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("020101010101Z"sv, 2002, 1, 1, 1, 1, 1);
    // ...
    EXPECT_DATETIME("480101010101Z"sv, 2048, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("490101010101Z"sv, 2049, 1, 1, 1, 1, 1);
    // This Y2050-problem is hardcoded in the spec. Oh no.
    EXPECT_DATETIME("500101010101Z"sv, 1950, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("510101010101Z"sv, 1951, 1, 1, 1, 1, 1);
    // ...
    EXPECT_DATETIME("970101010101Z"sv, 1997, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("980101010101Z"sv, 1998, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("990101010101Z"sv, 1999, 1, 1, 1, 1, 1);
}

TEST_CASE(test_utc_offset)
{
    // YYMMDDhhmm[ss](+|-)hhmm
    // We don't yet support storing the offset anywhere and instead just assume that the offset is just +0000.
    EXPECT_DATETIME("010101010101+0000"sv, 2001, 1, 1, 1, 1, 1);
    EXPECT_DATETIME("010203040506+0000"sv, 2001, 2, 3, 4, 5, 6);
    EXPECT_DATETIME("020406081012+0000"sv, 2002, 4, 6, 8, 10, 12);
    EXPECT_DATETIME("0204060810+0000"sv, 2002, 4, 6, 8, 10, 0);
    EXPECT_DATETIME("220911220000+0000"sv, 2022, 9, 11, 22, 0, 0);
    // Designed to fail once we support offsets:
    EXPECT_DATETIME("220911220000+0600"sv, 2022, 9, 11, 22, 0, 0);
}

TEST_CASE(test_utc_missing_z)
{
    // YYMMDDhhmm[ss]
    // We don't actually need to parse this correctly; rejecting these inputs is fine.
    // This test just makes sure that we don't crash.
    (void)Crypto::ASN1::parse_utc_time("010101010101"sv);
    (void)Crypto::ASN1::parse_utc_time("010203040506"sv);
    (void)Crypto::ASN1::parse_utc_time("020406081012"sv);
    (void)Crypto::ASN1::parse_utc_time("0204060810"sv);
    (void)Crypto::ASN1::parse_utc_time("220911220000"sv);
}

#undef EXPECT_DATETIME
#define EXPECT_DATETIME(sv, y, mo, d, h, mi, s, ms) \
    EXPECT_EQ(Crypto::ASN1::parse_generalized_time(sv).value(), UnixDateTime::from_unix_time_parts(y, mo, d, h, mi, s, ms))

TEST_CASE(test_generalized_boring)
{
    // YYYYMMDDhh[mm[ss[.fff]]]
    EXPECT_DATETIME("20010101010101Z"sv, 2001, 1, 1, 1, 1, 1, 0);
    EXPECT_DATETIME("20010203040506Z"sv, 2001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("20020406081012Z"sv, 2002, 4, 6, 8, 10, 12, 0);
    EXPECT_DATETIME("200204060810Z"sv, 2002, 4, 6, 8, 10, 0, 0);
    EXPECT_DATETIME("2002040608Z"sv, 2002, 4, 6, 8, 0, 0, 0);
    EXPECT_DATETIME("20020406081012.567Z"sv, 2002, 4, 6, 8, 10, 12, 567);
    EXPECT_DATETIME("20220911220000Z"sv, 2022, 9, 11, 22, 0, 0, 0);
}

TEST_CASE(test_generalized_offset)
{
    // YYYYMMDDhh[mm[ss[.fff]]](+|-)hhmm
    // We don't yet support storing the offset anywhere and instead just assume that the offset is just +0000.
    EXPECT_DATETIME("20010101010101+0000"sv, 2001, 1, 1, 1, 1, 1, 0);
    EXPECT_DATETIME("20010203040506+0000"sv, 2001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("20020406081012+0000"sv, 2002, 4, 6, 8, 10, 12, 0);
    EXPECT_DATETIME("200204060810+0000"sv, 2002, 4, 6, 8, 10, 0, 0);
    EXPECT_DATETIME("2002040608+0000"sv, 2002, 4, 6, 8, 0, 0, 0);
    EXPECT_DATETIME("20020406081012.567+0000"sv, 2002, 4, 6, 8, 10, 12, 567);
    EXPECT_DATETIME("20220911220000+0000"sv, 2022, 9, 11, 22, 0, 0, 0);
    // Designed to fail once we support offsets:
    EXPECT_DATETIME("20220911220000+0600"sv, 2022, 9, 11, 22, 0, 0, 0);
}

TEST_CASE(test_generalized_missing_z)
{
    // YYYYMMDDhh[mm[ss[.fff]]]
    EXPECT_DATETIME("20010101010101"sv, 2001, 1, 1, 1, 1, 1, 0);
    EXPECT_DATETIME("20010203040506"sv, 2001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("20020406081012"sv, 2002, 4, 6, 8, 10, 12, 0);
    EXPECT_DATETIME("200204060810"sv, 2002, 4, 6, 8, 10, 0, 0);
    EXPECT_DATETIME("2002040608"sv, 2002, 4, 6, 8, 0, 0, 0);
    EXPECT_DATETIME("20020406081012.567"sv, 2002, 4, 6, 8, 10, 12, 567);
    EXPECT_DATETIME("20220911220000"sv, 2022, 9, 11, 22, 0, 0, 0);
}

TEST_CASE(test_generalized_unusual_year)
{
    // Towards the positive
    EXPECT_DATETIME("20010203040506Z"sv, 2001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("20110203040506Z"sv, 2011, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("21010203040506Z"sv, 2101, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("30010203040506Z"sv, 3001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("40010203040506Z"sv, 4001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("90010203040506Z"sv, 9001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("99990203040506Z"sv, 9999, 2, 3, 4, 5, 6, 0);

    // Towards zero
    EXPECT_DATETIME("20010203040506Z"sv, 2001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("19990203040506Z"sv, 1999, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("19500203040506Z"sv, 1950, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("19010203040506Z"sv, 1901, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("18010203040506Z"sv, 1801, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("15010203040506Z"sv, 1501, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("10010203040506Z"sv, 1001, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("01010203040506Z"sv, 101, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("00110203040506Z"sv, 11, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("00010203040506Z"sv, 1, 2, 3, 4, 5, 6, 0);
    EXPECT_DATETIME("00000203040506Z"sv, 0, 2, 3, 4, 5, 6, 0);

    // Problematic dates
    EXPECT_DATETIME("20200229040506Z"sv, 2020, 2, 29, 4, 5, 6, 0);
    EXPECT_DATETIME("20000229040506Z"sv, 2000, 2, 29, 4, 5, 6, 0);
    EXPECT_DATETIME("24000229040506Z"sv, 2400, 2, 29, 4, 5, 6, 0);
}

TEST_CASE(test_generalized_nonexistent_dates)
{
    // The following dates don't exist. I'm not sure what the "correct" result is,
    // but we need to make sure that we don't crash.
    (void)Crypto::ASN1::parse_generalized_time("20210229040506Z"sv); // Not a leap year (not divisible by 4)
    (void)Crypto::ASN1::parse_generalized_time("21000229040506Z"sv); // Not a leap year (divisible by 100)
    (void)Crypto::ASN1::parse_generalized_time("20220230040506Z"sv); // Never exists
    (void)Crypto::ASN1::parse_generalized_time("20220631040506Z"sv); // Never exists
    (void)Crypto::ASN1::parse_generalized_time("20220732040506Z"sv); // Never exists

    // https://www.timeanddate.com/calendar/julian-gregorian-switch.html
    (void)Crypto::ASN1::parse_generalized_time("15821214040506Z"sv); // Gregorian switch; France
    (void)Crypto::ASN1::parse_generalized_time("15821011040506Z"sv); // Gregorian switch; Italy, Poland, Portugal, Spain
    (void)Crypto::ASN1::parse_generalized_time("15830105040506Z"sv); // Gregorian switch; Germany (Catholic)
    (void)Crypto::ASN1::parse_generalized_time("15831011040506Z"sv); // Gregorian switch; Austria
    (void)Crypto::ASN1::parse_generalized_time("15871026040506Z"sv); // Gregorian switch; Hungary
    (void)Crypto::ASN1::parse_generalized_time("16100826040506Z"sv); // Gregorian switch; Germany (old Prussia)
    (void)Crypto::ASN1::parse_generalized_time("17000223040506Z"sv); // Gregorian switch; Germany (Protestant)
    (void)Crypto::ASN1::parse_generalized_time("17520908040506Z"sv); // Gregorian switch; US, Canada, UK
    (void)Crypto::ASN1::parse_generalized_time("18711225040506Z"sv); // Gregorian switch; Japan
    (void)Crypto::ASN1::parse_generalized_time("19160407040506Z"sv); // Gregorian switch; Bulgaria
    (void)Crypto::ASN1::parse_generalized_time("19180207040506Z"sv); // Gregorian switch; Estonia, Russia
    (void)Crypto::ASN1::parse_generalized_time("19230222040506Z"sv); // Gregorian switch; Greece
    (void)Crypto::ASN1::parse_generalized_time("19261224040506Z"sv); // Gregorian switch; Turkey
}

TEST_CASE(test_encoder_primitives)
{
    auto roundtrip_value = [](auto value) {
        Crypto::ASN1::Encoder encoder;
        MUST(encoder.write(value));
        auto encoded = encoder.finish();
        Crypto::ASN1::Decoder decoder(encoded);
        auto decoded = MUST(decoder.read<decltype(value)>());
        EXPECT_EQ(decoded, value);
    };

    roundtrip_value(false);
    roundtrip_value(true);

    roundtrip_value(Crypto::UnsignedBigInteger { 0 });
    roundtrip_value(Crypto::UnsignedBigInteger { 1 });
    roundtrip_value(TRY_OR_FAIL(Crypto::UnsignedBigInteger { 2 }.shift_left(128)));
    roundtrip_value(TRY_OR_FAIL(Crypto::UnsignedBigInteger { 2 }.shift_left(256)));

    roundtrip_value(Vector { 1, 2, 840, 113549, 1, 1, 1 });
    roundtrip_value(Vector { 1, 2, 840, 113549, 1, 1, 11 });

    roundtrip_value(ByteString { "Hello, World!\n" });

    roundtrip_value(nullptr);

    roundtrip_value(Crypto::ASN1::BitStringView { { { 0x00, 0x01, 0x02, 0x03 } }, 3 });
}

TEST_CASE(test_encoder_constructed)
{
    Crypto::ASN1::Encoder encoder;
    /*
     * RSAPrivateKey ::= SEQUENCE {
     *   version           Version,  -- Version ::= INTEGER { two-prime(0), multi(1) }
     *   modulus           INTEGER,  -- n
     *   publicExponent    INTEGER,  -- e
     *   privateExponent   INTEGER,  -- d
     *   prime1            INTEGER,  -- p
     *   prime2            INTEGER,  -- q
     *   exponent1         INTEGER,  -- d mod (p-1)
     *   exponent2         INTEGER,  -- d mod (q-1)
     *   coefficient       INTEGER,  -- (inverse of q) mod p
     *   otherPrimeInfos   OtherPrimeInfos OPTIONAL
     * }
     */
    (void)encoder.write_constructed(Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::Sequence, [&] {
        MUST(encoder.write(0u));       // version
        MUST(encoder.write(0x1234u));  // modulus
        MUST(encoder.write(0x10001u)); // publicExponent
        MUST(encoder.write(0x5678u));  // privateExponent
        MUST(encoder.write(0x9abcu));  // prime1
        MUST(encoder.write(0xdef0u));  // prime2
        MUST(encoder.write(0x1234u));  // exponent1
        MUST(encoder.write(0x5678u));  // exponent2
        MUST(encoder.write(0x9abcu));  // coefficient
    });
    auto encoded = encoder.finish();
    Crypto::ASN1::Decoder decoder(encoded);
    MUST(decoder.enter());                                                 // Sequence
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0u);       // version
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x1234u);  // modulus
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x10001u); // publicExponent
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x5678u);  // privateExponent
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x9abcu);  // prime1
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0xdef0u);  // prime2
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x1234u);  // exponent1
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x5678u);  // exponent2
    EXPECT_EQ(MUST(decoder.read<Crypto::UnsignedBigInteger>()), 0x9abcu);  // coefficient
    EXPECT(decoder.eof());                                                 // no otherPrimeInfos
    MUST(decoder.leave());                                                 // Sequence
    EXPECT(decoder.eof());                                                 // no other data
}
