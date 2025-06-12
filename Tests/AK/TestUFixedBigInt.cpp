/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/NumericLimits.h>
#include <AK/Random.h>
#include <AK/UFixedBigInt.h>
#include <AK/UFixedBigIntDivision.h>

constexpr int test_iterations = 32;

TEST_CASE(one_plus_one)
{
    u256 a = 1u;
    u256 b = 1u;
    EXPECT_EQ(a + b, u256(2u));
}

TEST_CASE(identities)
{
    srand(0);
    for (int i = 0; i < test_iterations; ++i) {
        auto x = get_random<u256>();
        if ((x >> 255u) & 1u) {
            // ignore numbers that could overflow
            --i;
            continue;
        }
        EXPECT_EQ((x << 0u), x);
        EXPECT_EQ((x >> 0u), x);
        EXPECT_EQ((x / 1u), x);
        EXPECT_EQ((x % (x + 1u)), x);
        EXPECT_EQ((x << 1u) >> 1u, x);
        EXPECT_EQ((x * 2u) / 2u, x);
        EXPECT_EQ((x + 2u) - 2u, x);
    }
}

TEST_CASE(add_overflow_propagation)
{
    u256 a = NumericLimits<u128>::max();
    u256 b = a + a;
    u256 c = a * 2u;

    EXPECT_EQ(b.low(), NumericLimits<u128>::max() - 1u);
    EXPECT_EQ(b.high(), 1u);
    EXPECT_EQ(b, a << 1u);
    EXPECT_EQ(b, c);
}

TEST_CASE(simple_multiplication)
{
    srand(0);
    for (int i = 0; i < test_iterations; ++i) {
        u256 a = get_random<u256>();

        EXPECT_EQ(a * 0u, 0u);
        EXPECT_EQ(a * 1u, a);
        EXPECT_EQ(a >> 1u, a / 2u);
        if (!(a >> 255u & 1u)) {
            EXPECT_EQ(a << 1u, a * 2u);
        }
    }
}

TEST_CASE(div_mod)
{
    srand(0);
    for (int i = 0; i < test_iterations; ++i) {
        u256 a = get_random<u256>();
        u256 b = get_random<u256>();
        u256 mod;
        u256 div = a.div_mod(b, mod);
        EXPECT_EQ(a, div * b + mod);
    }
}

TEST_CASE(div_anti_knuth)
{
    EXPECT_EQ((u256 { { 0ull, 0xffffffffffffffffull, 1ull, 0ull } } / u128 { 0x8000000000000001ull, 0xffffffffffffffffull }), 1u);
    EXPECT_EQ((u128 { { 0xffffffff00000000ull, 1ull } } / u128 { 0xffffffff80000001ull }), 1u);

    srand(0);

    auto generate_u512 = [] {
        using namespace AK::Detail;

        u512 number;
        auto& storage = get_storage_of(number);

        static constexpr u32 interesting_words_count = 14;
        static constexpr NativeWord interesting_words[interesting_words_count] = {
            0,
            0,
            1,
            2,
            3,
            max_native_word / 4 - 1,
            max_native_word / 4,
            max_native_word / 2 - 1,
            max_native_word / 2,
            max_native_word / 2 + 1,
            max_native_word / 2 + 2,
            max_native_word - 3,
            max_native_word - 2,
            max_native_word - 1,
        };
        for (size_t i = 0; i < storage.size(); ++i) {
            u32 type = get_random_uniform(interesting_words_count + 1);
            NativeWord& next_word = storage[i];
            if (type == interesting_words_count)
                next_word = get_random<NativeWord>();
            else
                next_word = interesting_words[type];
        }

        return number;
    };

    for (int i = 0; i < 16384; ++i) {
        u512 a = generate_u512(), b = generate_u512();
        if (b == 0)
            continue;

        u512 mod;
        u512 div = a.div_mod(b, mod);

        EXPECT_EQ(div * b + mod, a);
        EXPECT_EQ(div.wide_multiply(b) + mod, a);
        EXPECT(0 <= mod && mod < b);
    }
}

TEST_CASE(shifts)
{
    u128 val { 0x1234ULL };
    EXPECT_EQ(val << 1u, u128(0x2468ull));
    EXPECT_EQ(val << 4u, u128(0x12340ull));
    EXPECT_EQ(val << 64u, u128(0ull, 0x1234ull));
}

TEST_CASE(constexpr_truncae)
{
    static constexpr u256 wide = u256(u128 { 0x8a4b08d32f8b8e48ULL, 0x8459322f67b8e26dULL }, u128 { 0xeea82af4312d1931ULL, 0x654fb5cfe82dbd58ULL });
    static constexpr u64 val = static_cast<u64>(wide);
    EXPECT_EQ(val, 0x8a4b08d32f8b8e48ULL);
}

TEST_CASE(mod_hardcoded)
{
    EXPECT_EQ(u256(u128 { 0x8a4b08d32f8b8e48ULL, 0x8459322f67b8e26dULL }, u128 { 0xeea82af4312d1931ULL, 0x654fb5cfe82dbd58ULL }) % u256(u128 { 0x40a58652868d5d66ULL, 0x81d674bf7d6d6861ULL }, u128 { 0xa8314900e6188a82ULL, 0xc273ca947237b4aaULL }), u256(u128 { 0x8a4b08d32f8b8e48ULL, 0x8459322f67b8e26dULL }, u128 { 0xeea82af4312d1931ULL, 0x654fb5cfe82dbd58ULL }));
    EXPECT_EQ(u256(u128 { 0xda06d295caa75a3bULL, 0xe3ae0d460049948eULL }, u128 { 0x9a89d29a0325f27fULL, 0x1c8d90ebadec5607ULL }) % u256(u128 { 0x38bd4d49ff59fdf8ULL, 0xcba9acf09110de14ULL }, u128 { 0x51a376c68c4702feULL, 0x0d1b59dec8d2338bULL }), u256(u128 { 0x688c3801cbf35e4bULL, 0x4c5ab364de27d866ULL }, u128 { 0xf742e50cea97ec82ULL, 0x0256dd2e1c47eef0ULL }));
    EXPECT_EQ(u256(u128 { 0xdfb56d42706bdb28ULL, 0x6c3bd5ea790c7ef5ULL }, u128 { 0xfebec271d7c757baULL, 0x7dbd745d56bc9e0eULL }) % u256(u128 { 0x30a309a58aed2c01ULL, 0x64d58c8b485c113dULL }, u128 { 0xfa01f558732e9b78ULL, 0x5862b502ebb2dbe9ULL }), u256(u128 { 0xaf12639ce57eaf27ULL, 0x0766495f30b06db8ULL }, u128 { 0x04bccd196498bc42ULL, 0x255abf5a6b09c225ULL }));
    EXPECT_EQ(u256(u128 { 0x0a8473d84131f420ULL, 0x0471632bb018c1a2ULL }, u128 { 0x22865980ccd1014fULL, 0xcade79df2adf8fdfULL }) % u256(u128 { 0xd7da811f35db7de0ULL, 0x4e3d98062eae954fULL }, u128 { 0x23946cd23d470d7eULL, 0x6645d41afdc1f2e8ULL }), u256(u128 { 0x32a9f2b90b567640ULL, 0xb633cb25816a2c52ULL }, u128 { 0xfef1ecae8f89f3d0ULL, 0x6498a5c42d1d9cf6ULL }));
    EXPECT_EQ(u256(u128 { 0x68636d8d1b7ac40bULL, 0xcb04084ddc684d42ULL }, u128 { 0xaa43c0f6e4e0178cULL, 0x49edae817f27c32aULL }) % u256(u128 { 0xbcc52d96070b7046ULL, 0x2f8255f3c6f8d4bdULL }, u128 { 0x2423bb472eced919ULL, 0x2ed9534c1570b7faULL }), u256(u128 { 0xab9e3ff7146f53c5ULL, 0x9b81b25a156f7884ULL }, u128 { 0x862005afb6113e73ULL, 0x1b145b3569b70b30ULL }));
    EXPECT_EQ(u256(u128 { 0xad34ce382cd00226ULL, 0x39b1986d56a064afULL }, u128 { 0xa9410bbd86d9ab21ULL, 0x0fb980a5a7d4b99fULL }) % u256(u128 { 0xa7561893be8cd299ULL, 0x9c3cb9184f45878aULL }, u128 { 0x1e066270a27414efULL, 0xe0fbaa0b739890b8ULL }), u256(u128 { 0xad34ce382cd00226ULL, 0x39b1986d56a064afULL }, u128 { 0xa9410bbd86d9ab21ULL, 0x0fb980a5a7d4b99fULL }));
    EXPECT_EQ(u256(u128 { 0x69a0ab23d9f81040ULL, 0xf509000f44fcadb3ULL }, u128 { 0x544310cc56ea051aULL, 0x968a003529f513c0ULL }) % u256(u128 { 0xd6db169628ba28edULL, 0xcf2417c98b765531ULL }, u128 { 0x27865ebfca2d945aULL, 0xcbd1257363cb86a1ULL }), u256(u128 { 0x69a0ab23d9f81040ULL, 0xf509000f44fcadb3ULL }, u128 { 0x544310cc56ea051aULL, 0x968a003529f513c0ULL }));
    EXPECT_EQ(u256(u128 { 0x5d41bcd96e47dfbdULL, 0x623a7c82c903789bULL }, u128 { 0x57c3723bfcfd7eeeULL, 0x8b1f21a0739fa6a8ULL }) % u256(u128 { 0xf918e7d73771d5c4ULL, 0xdd40e701852f4d68ULL }, u128 { 0x7c4ac424e3836a4dULL, 0xcb7a0bcc58701175ULL }), u256(u128 { 0x5d41bcd96e47dfbdULL, 0x623a7c82c903789bULL }, u128 { 0x57c3723bfcfd7eeeULL, 0x8b1f21a0739fa6a8ULL }));
    EXPECT_EQ(u256(u128 { 0xa4394401788e848aULL, 0x8a907db529ba2943ULL }, u128 { 0x4f3c13b9058d17d3ULL, 0xf17f01b5c1898104ULL }) % u256(u128 { 0x214097598f92cebeULL, 0x723b873f1f879305ULL }, u128 { 0x5f9352861d92ff91ULL, 0x527c65978f7d12ebULL }), u256(u128 { 0x61b8154e5968e70eULL, 0xa6196f36eaab0339ULL }, u128 { 0x90156eacca6718b0ULL, 0x4c863686a28f5b2dULL }));
    EXPECT_EQ(u256(u128 { 0x324e46a2bd4d9c0dULL, 0xfb8980a6353814a8ULL }, u128 { 0x3605ef999901dc37ULL, 0xcc2493941c934b83ULL }) % u256(u128 { 0x45e1b8552ccd49b1ULL, 0xe61bd62768189e42ULL }, u128 { 0x859e83ed2f92c211ULL, 0xc7713b3893031cbdULL }), u256(u128 { 0xec6c8e4d9080525cULL, 0x156daa7ecd1f7665ULL }, u128 { 0xb0676bac696f1a26ULL, 0x04b3585b89902ec5ULL }));
    EXPECT_EQ(u256(u128 { 0x9a3b5f7c879d14f4ULL, 0xc437119868072180ULL }, u128 { 0xea395ae2238ada4eULL, 0x1aa5cc44c4c9deb5ULL }) % u256(u128 { 0x9535e4674b364058ULL, 0xbbf3d10e995c610dULL }, u128 { 0x8fac6f8ae200290aULL, 0x7832f747c56ae6dfULL }), u256(u128 { 0x9a3b5f7c879d14f4ULL, 0xc437119868072180ULL }, u128 { 0xea395ae2238ada4eULL, 0x1aa5cc44c4c9deb5ULL }));
    EXPECT_EQ(u256(u128 { 0xf2a2d399b73fd0c2ULL, 0x02b7155ee15525ffULL }, u128 { 0xcaaa7daf39923db6ULL, 0x8ccb6244075bb5bbULL }) % u256(u128 { 0xfc002da6ab396d95ULL, 0xd7d0ebd6242b7119ULL }, u128 { 0x7f2ec32021ce7d32ULL, 0x63cef84255b91414ULL }), u256(u128 { 0xf6a2a5f30c06632dULL, 0x2ae62988bd29b4e5ULL }, u128 { 0x4b7bba8f17c3c083ULL, 0x28fc6a01b1a2a1a7ULL }));
    EXPECT_EQ(u256(u128 { 0xfef71dab99335163ULL, 0xd1f1bc5f37570d67ULL }, u128 { 0x34bd2c7372eb8c4cULL, 0x15c0d3f1cc1613beULL }) % u256(u128 { 0x3978824c651c6cceULL, 0x5631f4d483e9f3ffULL }, u128 { 0xfd7c47d688e0d50fULL, 0xb3a9f99c7234d772ULL }), u256(u128 { 0xfef71dab99335163ULL, 0xd1f1bc5f37570d67ULL }, u128 { 0x34bd2c7372eb8c4cULL, 0x15c0d3f1cc1613beULL }));
    EXPECT_EQ(u256(u128 { 0x19d69d0229db064eULL, 0x612eea6e8d79807bULL }, u128 { 0xe755c10d2b9e25adULL, 0x6a84d397b8e7da54ULL }) % u256(u128 { 0x9db6a18d292bc65fULL, 0xbdc7ccbcdb4f046cULL }, u128 { 0xd5be95d179cc1aa4ULL, 0x77c81421a604eb66ULL }), u256(u128 { 0x19d69d0229db064eULL, 0x612eea6e8d79807bULL }, u128 { 0xe755c10d2b9e25adULL, 0x6a84d397b8e7da54ULL }));
    EXPECT_EQ(u256(u128 { 0xcd6a8ed6185d098fULL, 0xcf17b08e6e3836e5ULL }, u128 { 0x52e187a75426d99dULL, 0x562e1c437b33a29dULL }) % u256(u128 { 0x0c3dd1aa87a4bd96ULL, 0xac333d8636735a23ULL }, u128 { 0x1a30abda1015e674ULL, 0xe968125d96bdc2e9ULL }), u256(u128 { 0xcd6a8ed6185d098fULL, 0xcf17b08e6e3836e5ULL }, u128 { 0x52e187a75426d99dULL, 0x562e1c437b33a29dULL }));
    EXPECT_EQ(u256(u128 { 0x60151f3f11782d51ULL, 0xeecbc23fa60bd168ULL }, u128 { 0x825b67c89bce81f2ULL, 0x082fe85ba1a09583ULL }) % u256(u128 { 0x438123a283f8133aULL, 0x7b5936b727339a8eULL }, u128 { 0x36f2bc572018588cULL, 0xbdebe2b4033d3209ULL }), u256(u128 { 0x60151f3f11782d51ULL, 0xeecbc23fa60bd168ULL }, u128 { 0x825b67c89bce81f2ULL, 0x082fe85ba1a09583ULL }));
    EXPECT_EQ(u256(u128 { 0x6a98f75458b6c9daULL, 0xbe935c50e782e82fULL }, u128 { 0xf8f7479d9ba56379ULL, 0xfd3cb6194bc5966fULL }) % u256(u128 { 0xc0fb2a97d7368d96ULL, 0x306534301d4eadbeULL }, u128 { 0x30b2c8ff81066af6ULL, 0xd23116ef8d5eacf5ULL }), u256(u128 { 0xa99dccbc81803c44ULL, 0x8e2e2820ca343a70ULL }, u128 { 0xc8447e9e1a9ef883ULL, 0x2b0b9f29be66e97aULL }));
    EXPECT_EQ(u256(u128 { 0xf90a6805c45be556ULL, 0x1d4a0c204a2dec7dULL }, u128 { 0x4a8c0d194584da59ULL, 0xcd1ab79a84dfccb6ULL }) % u256(u128 { 0xcedf80ed06c339b1ULL, 0x3a18231b09b21a3cULL }, u128 { 0xef2fedb7c3b237ddULL, 0x01d6223300a1f18aULL }), u256(u128 { 0x4621813fd5b5e197ULL, 0xecd2d36715f48c20ULL }, u128 { 0x94c3fa6b6b3ea16cULL, 0x0141e37d3ea81178ULL }));
    EXPECT_EQ(u256(u128 { 0x5cced259ff5b73fdULL, 0x223a2bc9d62d3714ULL }, u128 { 0xf1b7b34b45f3608fULL, 0xce2325cbc0e9734fULL }) % u256(u128 { 0xf5dc56158c242575ULL, 0xb3bf8578c1852fdcULL }, u128 { 0xd97725f998d1d289ULL, 0x053baa680c5abb16ULL }), u256(u128 { 0xe83db511a5d9bf2aULL, 0xc00cd6645ae2ec6aULL }, u128 { 0xd090ea44fdfc4d94ULL, 0x020c2ff1df16f2d4ULL }));
    EXPECT_EQ(u256(u128 { 0x6ce28a960af0ceb3ULL, 0x2da9808f962b0c43ULL }, u128 { 0x67cdac05a542bd66ULL, 0x5d3eb81aadf9479aULL }) % u256(u128 { 0x45f549795eab7c6cULL, 0x5643e85f6b4399eeULL }, u128 { 0x3b068fa03cb257dfULL, 0x3b42cfa16517b14cULL }), u256(u128 { 0x26ed411cac455247ULL, 0xd76598302ae77255ULL }, u128 { 0x2cc71c6568906586ULL, 0x21fbe87948e1964eULL }));
    EXPECT_EQ(u256(u128 { 0x236f8081c4dc0d2aULL, 0xa7da15c4c15e83f3ULL }, u128 { 0x32c0948d497b78f0ULL, 0xf75ddc710601d2d0ULL }) % u256(u128 { 0x326f376465b287beULL, 0x5e24a7c87a45f4ebULL }, u128 { 0x1fa25aecc5a5a1f2ULL, 0x3490287aca77c399ULL }), u256(u128 { 0x59b2a2f02e11ee32ULL, 0x2f4776a2d846b046ULL }, u128 { 0xb43728da32e4f127ULL, 0x251d3a85dc22c46bULL }));
    EXPECT_EQ(u256(u128 { 0xf5af1d760c381629ULL, 0x9f4d904501f9f6d6ULL }, u128 { 0xc23fe8d79d015270ULL, 0x3982c8897a86e837ULL }) % u256(u128 { 0xd3cc875eec2d5032ULL, 0x46e392089468f8cfULL }, u128 { 0x91c6762130826cedULL, 0x9e3b011ba58b4705ULL }), u256(u128 { 0xf5af1d760c381629ULL, 0x9f4d904501f9f6d6ULL }, u128 { 0xc23fe8d79d015270ULL, 0x3982c8897a86e837ULL }));
    EXPECT_EQ(u256(u128 { 0x6b60c428cac4f505ULL, 0xeac42ae8d7929fb7ULL }, u128 { 0x59a0ce8a7110df27ULL, 0xc0d5952f55096e15ULL }) % u256(u128 { 0x280419bd2d8fe3e8ULL, 0x13b50ec9c2bb7397ULL }, u128 { 0x8d8ef08f3ac8ce5eULL, 0x8912b53aa9279938ULL }), u256(u128 { 0x435caa6b9d35111dULL, 0xd70f1c1f14d72c20ULL }, u128 { 0xcc11ddfb364810c9ULL, 0x37c2dff4abe1d4dcULL }));
    EXPECT_EQ(u256(u128 { 0x8068bf135ceead51ULL, 0xadda5b57797a3a27ULL }, u128 { 0x4c4e3fe186af2698ULL, 0xdfbab959987cb289ULL }) % u256(u128 { 0x93c99cb4fa9f36c0ULL, 0xe107948b8bf301d8ULL }, u128 { 0xab4e7570e6e8e177ULL, 0xdb95d36ef24543daULL }), u256(u128 { 0xec9f225e624f7691ULL, 0xccd2c6cbed87384eULL }, u128 { 0xa0ffca709fc64520ULL, 0x0424e5eaa6376eaeULL }));
    EXPECT_EQ(u256(u128 { 0x036b4a64b2ab05bbULL, 0x6be175b3549f7440ULL }, u128 { 0x3c6839ecac5d4634ULL, 0x6a1939f6585dd1ddULL }) % u256(u128 { 0x329f61eaf9c14938ULL, 0x6653276323053388ULL }, u128 { 0x7e511a9611463f4dULL, 0x9898a93910722fd8ULL }), u256(u128 { 0x036b4a64b2ab05bbULL, 0x6be175b3549f7440ULL }, u128 { 0x3c6839ecac5d4634ULL, 0x6a1939f6585dd1ddULL }));
    EXPECT_EQ(u256(u128 { 0xe5d0db9190bb01c1ULL, 0x20510645c252e9b1ULL }, u128 { 0x3b673f98db9a3038ULL, 0xbda4406d733b1c6cULL }) % u256(u128 { 0x4d67af71063282f2ULL, 0x594aa60bb2360bbdULL }, u128 { 0x4c2759ff1b2ffbd1ULL, 0xe29a2e0962d9bdbfULL }), u256(u128 { 0xe5d0db9190bb01c1ULL, 0x20510645c252e9b1ULL }, u128 { 0x3b673f98db9a3038ULL, 0xbda4406d733b1c6cULL }));
    EXPECT_EQ(u256(u128 { 0x2f833c8cd20c43f9ULL, 0x405bd5f257ac19e1ULL }, u128 { 0xd9873917f32ca4adULL, 0x582dda480fecde28ULL }) % u256(u128 { 0xb56564a5a9dbc163ULL, 0x17b4076b2667c703ULL }, u128 { 0xdf0f26d9a66f513eULL, 0xb34c28d1e1a1953cULL }), u256(u128 { 0x2f833c8cd20c43f9ULL, 0x405bd5f257ac19e1ULL }, u128 { 0xd9873917f32ca4adULL, 0x582dda480fecde28ULL }));
    EXPECT_EQ(u256(u128 { 0x79ceb31188bc142bULL, 0xb2c083d1b0d1a172ULL }, u128 { 0x87a465799728fe9fULL, 0xe05c1c98eaa03994ULL }) % u256(u128 { 0x548b4f12f104f995ULL, 0x8d1d554e53ebc210ULL }, u128 { 0x4f5238bde10ce04aULL, 0x33da77cfa817ef7cULL }), u256(u128 { 0x27a176c5c4a82dd7ULL, 0x7e4b2e9861229931ULL }, u128 { 0x4a5b828212f57d75ULL, 0x10f23d5a4a407ba3ULL }));
    EXPECT_EQ(u256(u128 { 0xfbcfb8e88417af84ULL, 0xfd35ec5ad38f6f00ULL }, u128 { 0x12d5c3e4e108cc62ULL, 0x09370460a41c637fULL }) % u256(u128 { 0x71faedeee5e0bf52ULL, 0x3d17ff54be8d686fULL }, u128 { 0x02e3ab47712e3d11ULL, 0x64da86270055e5eaULL }), u256(u128 { 0xfbcfb8e88417af84ULL, 0xfd35ec5ad38f6f00ULL }, u128 { 0x12d5c3e4e108cc62ULL, 0x09370460a41c637fULL }));
    EXPECT_EQ(u256(u128 { 0x690f9145f7b1f8f8ULL, 0xe790aa66a2e08b63ULL }, u128 { 0x1d6ded50aa11aa3cULL, 0x601ec6f81fd1d57aULL }) % u256(u128 { 0x1256bdca9e0d6066ULL, 0xd19119919b026c0cULL }, u128 { 0x17a17b7df7689c40ULL, 0x97baf68d5f5622ddULL }), u256(u128 { 0x690f9145f7b1f8f8ULL, 0xe790aa66a2e08b63ULL }, u128 { 0x1d6ded50aa11aa3cULL, 0x601ec6f81fd1d57aULL }));
    EXPECT_EQ(u256(u128 { 0x7f13e232d82a24c6ULL, 0x23d41447dd7f5bc6ULL }, u128 { 0xd89a3ed8b30527caULL, 0xa98ef2cc01e83685ULL }) % u256(u128 { 0x8d4f5b1983fc1f0eULL, 0xf54102ece15fb0faULL }, u128 { 0x17b8aec68556a16dULL, 0x4e1e5bea70cb9398ULL }), u256(u128 { 0x64752bffd031e6aaULL, 0x39520e6e1abff9d1ULL }, u128 { 0xa928e14ba857e4eeULL, 0x0d523af720510f55ULL }));
    EXPECT_EQ(u256(u128 { 0x49750d7f39d61607ULL, 0x58bdef1c3e00d18eULL }, u128 { 0xa651479cd1fd1933ULL, 0xd1834bc3d654b633ULL }) % u256(u128 { 0x1bda34f5ec68ef3bULL, 0x12c65ce5363a7616ULL }, u128 { 0x5a79c4d85da0071aULL, 0xffa6b6284559d1aaULL }), u256(u128 { 0x49750d7f39d61607ULL, 0x58bdef1c3e00d18eULL }, u128 { 0xa651479cd1fd1933ULL, 0xd1834bc3d654b633ULL }));
}

TEST_CASE(endian_swap)
{
    constexpr u128 const a { 0x1234567890abcdefULL, 0xfedcba0987654321ULL };
    constexpr u128 const a_swapped { 0x2143658709badcfeull, 0xefcdab9078563412ull };

    static_assert(!AK::HostIsLittleEndian || bit_cast<u128>(BigEndian { a }) == a_swapped);
    static_assert(AK::HostIsLittleEndian || bit_cast<u128>(LittleEndian { a }) == a_swapped);

    static_assert(!AK::HostIsLittleEndian || bit_cast<u128>(LittleEndian { a }) == a);
    static_assert(AK::HostIsLittleEndian || bit_cast<u128>(BigEndian { a }) == a);
}
