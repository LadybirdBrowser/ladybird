/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/PEM.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibCrypto/PK/PK.h>
#include <LibCrypto/PK/RSA.h>
#include <LibTest/TestCase.h>
#include <cstring>

static ByteBuffer operator""_b(char const* string, size_t length)
{
    return ByteBuffer::copy(string, length).release_value();
}

TEST_CASE(test_RSA_raw_encrypt)
{
    ByteBuffer data { "hellohellohellohellohellohellohellohellohellohellohellohello123-"_b };
    u8 result[] { 0x6f, 0x7b, 0xe2, 0xd3, 0x95, 0xf8, 0x8d, 0x87, 0x6d, 0x10, 0x5e, 0xc3, 0xcd, 0xf7, 0xbb, 0xa6, 0x62, 0x8e, 0x45, 0xa0, 0xf1, 0xe5, 0x0f, 0xdf, 0x69, 0xcb, 0xb6, 0xd5, 0x42, 0x06, 0x7d, 0x72, 0xa9, 0x5e, 0xae, 0xbf, 0xbf, 0x0f, 0xe0, 0xeb, 0x31, 0x31, 0xca, 0x8a, 0x81, 0x1e, 0xb9, 0xec, 0x6d, 0xcc, 0xb8, 0xa4, 0xac, 0xa3, 0x31, 0x05, 0xa9, 0xac, 0xc9, 0xd3, 0xe6, 0x2a, 0x18, 0xfe };
    Crypto::PK::RSA rsa(
        "8126832723025844890518845777858816391166654950553329127845898924164623511718747856014227624997335860970996746552094406240834082304784428582653994490504519"_bigint,
        "4234603516465654167360850580101327813936403862038934287300450163438938741499875303761385527882335478349599685406941909381269804396099893549838642251053393"_bigint,
        "65537"_bigint);
    u8 buffer[rsa.output_size()];
    auto buf = Bytes { buffer, sizeof(buffer) };
    rsa.encrypt(data, buf);
    EXPECT(memcmp(result, buf.data(), buf.size()) == 0);
}

// RSA PKCS #1 1.5
TEST_CASE(test_RSA_PKCS_1_encrypt)
{
    ByteBuffer data { "hellohellohellohellohellohellohellohellohello123-"_b };
    Crypto::PK::RSA_PKCS1_EME rsa(
        "8126832723025844890518845777858816391166654950553329127845898924164623511718747856014227624997335860970996746552094406240834082304784428582653994490504519"_bigint,
        "4234603516465654167360850580101327813936403862038934287300450163438938741499875303761385527882335478349599685406941909381269804396099893549838642251053393"_bigint,
        "65537"_bigint);
    u8 buffer[rsa.output_size()];
    auto buf = Bytes { buffer, sizeof(buffer) };
    rsa.encrypt(data, buf);
    rsa.decrypt(buf, buf);

    EXPECT(memcmp(buf.data(), "hellohellohellohellohellohellohellohellohello123-", 49) == 0);
}

// RSA | ASN1 PKCS1 DER / PEM encoded Key import
TEST_CASE(test_RSA_ASN1_PKCS1_DER_PEM_parse)
{
    auto privkey = R"(-----BEGIN RSA PRIVATE KEY-----
MIIBOgIBAAJBAJsrIYHxs1YL9tpfodaWs1lJoMdF4kgFisUFSj6nvBhJUlmBh607AlgTaX0E
DGPYycXYGZ2n6rqmms5lpDXBpUcCAwEAAQJAUNpPkmtEHDENxsoQBUXvXDYeXdePSiIBJhpU
joNOYoR5R9z5oX2cpcyykQ58FC2vKKg+x8N6xczG7qO95tw5UQIhAN354CP/FA+uTeJ6KJ+i
zCBCl58CjNCzO0s5HTc56el5AiEAsvPKXo5/9gS/S4UzDRP6abq7GreixTfjR8LXidk3FL8C
IQCTjYI861Y+hjMnlORkGSdvWlTHUj6gjEOh4TlWeJzQoQIgAxMZOQKtxCZUuxFwzRq4xLRG
nrDlBQpuxz7bwSyQO7UCIHrYMnDohgNbwtA5ZpW3H1cKKQQvueWm6sxW9P5sUrZ3
-----END RSA PRIVATE KEY-----)"sv;

    Crypto::PK::RSA rsa(privkey);
    if (rsa.public_key().public_exponent() != 65537) {
        FAIL("Invalid public exponent");
    }
    if (rsa.private_key().private_exponent() != "4234603516465654167360850580101327813936403862038934287300450163438938741499875303761385527882335478349599685406941909381269804396099893549838642251053393"_bigint) {
        FAIL("Invalid private exponent");
    }
}

// RSA | ASN1 PKCS8 DER / PEM encoded Key import
TEST_CASE(test_RSA_ASN1_PKCS8_DER_PEM_parse)
{
    auto privkey = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC7ZBYaG9+CcJP7
WVFJRI/uw3hljc7WpzeYs8MN82/g9CG1gnEF3P3ZSBdWVr8gnbh05EsSGHKghIce
CB7DNrM5Ab0ru04CuODdPx56xCj+4MmzTc/aq79ntmOt131NGHgq9yVwfJqnSpyl
OoVw7j/Wg4ciwPDQaeLmD1BsE/W9UsF1km7DWasBpW5br82DpudKgJq2Ixf52+rY
TCkMgyWcetx4MfXll4y5ZVtJXCnHJfkCS64EaCqXmClP4ovOuHH4khJ3rW9j4yuL
e5ck3PSXOrtOTR43HZkCXzseCkbW7qKSmk/9ZreImOzOgu8vvw7ewLAQR9qYVS6X
PXY8IilDAgMBAAECggEBAIV3ld5mt90Z/exqA2Fh+fofMyNxyz5Lv2d9sZHAL5FT
kKbND18TtaIKnMSb6Gl8rKJk76slyo7Vlb8oHXEBBsm1mV0KfVenAlHS4QyjpmdT
B5Yz97VR2nQuDfUFpHNC2GQRv5LMzQIWPFfaxKxYpRNOfvOb5Gks4bTmd2tjFAYR
MCbHgPw1liKA9dYKk4NB0301EY05e4Zz8RjqYHkkmOPD7DnjFbHqcFUjVKK5E3vD
WjxNXUbiSudCCN7WLEOyeHZNd+l6kSAVxZuCAp0G3Da5ndXgIStcy4hYi/fL3XQQ
bNpxjfhsjlD3tdHNr3NNYDAqxcxpsyO1NCpCIW3ZVrECgYEA7l6gTZ3e9AiSNlMd
2O2vNnbQ6UZfsEfu2y7HmpCuNJkFkAnM/1h72Krejnn31rRuR6uCFn4YgQUN9Eq0
E1PJCtTay2ucZw5rqtkewT9QzXvVD9eiGM+MF89UzSCC+dOW0/odkD+xP2evnPvG
PbXztnuERC1pi0YWLj1YcsfsEX0CgYEAyUA2UtYjnvCcteIy+rURT0aoZ9tDMrG+
Es42EURVv1sduVdUst5R+bXx1aDzpCkcdni3TyxeosvTGAZngI3O8ghh1GV7NPZR
nkiPXjMnhL0Zf+X9gCA6TFANfPuWhMSGijYsCd46diKGDReGYUnmcN9XopeG1h6i
3JiOuVPAIb8CgYBmIcUtfGb6yHFdNV+kgrJ/84ivaqe1MBz3bKO5ZiQ+BRKNFKXx
AkiOHSgeg8PdCpH1w1aJrJ1zKmdANIHThiKtsWXNot3wig03tq+mvSox4Mz5bLrX
RpYP3ZXIDhYQVMhbKt9f3upi8FoeOQJHjp5Nob6aN5rxQaZfSYmMJHzRQQKBgQCO
ALwUGTtLNBYvlKtKEadkG8RKfAFfbOFkXZLy/hfPDRjdJY0DJTIMk+BPT+F6rPOD
eMxHllQ0ZMPPiP1RTT5/s4BsISsdhMy0dhiLbGbvF4s9nugPly3rmPTbgp6DkjQo
o+7RC7iOkO+rnzTXwxBSBpXMiUTAIx/hrdfPVxQT+wKBgCh7N3OLIOH6EWcW1fif
UoENh8rkt/kzm89G1JLwBhuBIBPXUEZt2dS/xSUempqVqFGONpP87gvqxkMTtgCA
73KXn/cxHWM2kmXyHA3kQlOYw6WHjpldQAxLE+TRHXO2JUtZ09Mu4rVXX7lmwbTm
l3vmuDEF3/Bo1C1HTg0xRV/l
-----END PRIVATE KEY-----)"sv;

    Crypto::PK::RSA rsa(privkey);
    if (rsa.public_key().public_exponent() != 65537) {
        FAIL("Invalid public exponent");
    }

    if (rsa.private_key().private_exponent() != "16848664331299797559656678180469464902267415922431923391961407795209879741791261105581093539484181644099608161661780611501562625272630894063592208758992911105496755004417051031019663332258403844985328863382168329621318366311519850803972480500782200178279692319955495383119697563295214236936264406600739633470565823022975212999060908747002623721589308539473108154612454595201561671949550531384574873324370774408913092560971930541734744950937900805812300970883306404011323308000168926094053141613790857814489531436452649384151085451448183385611208320292948291211969430321231180227006521681776197974694030147965578466993"_bigint) {
        FAIL("Invalid private exponent");
    }
}

TEST_CASE(test_RSA_keygen_enc)
{
    auto keypem = R"(-----BEGIN PRIVATE KEY-----
MIIBVQIBADANBgkqhkiG9w0BAQEFAASCAT8wggE7AgEAAkEA5HMXMnY+RhEcYXsa
OyB/YkcrO1nxIeyDCMqwg5MDrSXO8vPXSEb9AZUNMF1jKiFWPoHxZ+foRxrLv4d9
sV/ETwIDAQABAkBpC37UJkjWQRHyxP83xuasExuO6/mT5sQN692kcppTJ9wHNWoD
9ZcREk4GGiklu4qx48/fYt8Cv6z6JuQ0ZQExAiEA9XRZVUnCJ2xOcCFCbyIF+d3F
9Kht5rR77F9KsRlgUbkCIQDuQ7YzLpQ8V8BJwKbDeXw1vQvcPEnyKnTOoALpF6bq
RwIhAIDSm8Ajgf7m3RQEoLVrCe/l8WtCqsuWliOsr6rbQq4hAiEAx8R16wvOtZlN
W4jvSU1+WwAaBZl21lfKf8OhLRXrmNkCIG9IRdcSiNR/Ut8QfD3N9Bb1HsUm+Bvz
c8yGzl89pYST
-----END PRIVATE KEY-----
)"sv;
    auto decoded = Crypto::decode_pem(keypem.bytes());
    EXPECT_EQ(decoded.type, Crypto::PEMType::PrivateKey);
    auto decoder = Crypto::ASN1::Decoder { decoded.data };
    auto priv_key_info = MUST(Crypto::Certificate::parse_private_key_info(decoder, {}));
    auto keypair = MUST(Crypto::PK::RSA::parse_rsa_key(priv_key_info.raw_key, true, {}));
    auto priv_der = MUST(priv_key_info.rsa.export_as_der());
    auto rsa_encryption_oid = Array<int, 7> { 1, 2, 840, 113549, 1, 1, 1 };
    auto wrapped_priv_der = MUST(Crypto::PK::wrap_in_private_key_info(priv_key_info.raw_key, rsa_encryption_oid, nullptr));
    auto priv_pem = MUST(Crypto::encode_pem(wrapped_priv_der, Crypto::PEMType::PrivateKey));
    auto rsa_from_pair = Crypto::PK::RSA(keypair.public_key, keypair.private_key);
    auto rsa_from_pem = Crypto::PK::RSA(priv_pem);

    EXPECT_EQ(keypem, StringView(priv_pem));

    u8 enc_buffer[rsa_from_pair.output_size()];
    u8 dec_buffer[rsa_from_pair.output_size()];

    auto enc = Bytes { enc_buffer, rsa_from_pair.output_size() };
    auto dec = Bytes { dec_buffer, rsa_from_pair.output_size() };

    dec.overwrite(0, "WellHelloFriends", 16);

    rsa_from_pair.encrypt(dec, enc);
    rsa_from_pem.decrypt(enc, dec);

    EXPECT_EQ(memcmp(dec.data(), "WellHelloFriends", 16), 0);
}

TEST_CASE(test_RSA_encrypt_decrypt)
{
    Crypto::PK::RSA rsa(
        "9527497237087650398000977129550904920919162360737979403539302312977329868395261515707123424679295515888026193056908173564681660256268221509339074678416049"_bigint,
        "39542231845947188736992321577701849924317746648774438832456325878966594812143638244746284968851807975097653255909707366086606867657273809465195392910913"_bigint,
        "65537"_bigint);

    u8 enc_buffer[rsa.output_size()];
    u8 dec_buffer[rsa.output_size()];

    auto enc = Bytes { enc_buffer, rsa.output_size() };
    auto dec = Bytes { dec_buffer, rsa.output_size() };

    enc.overwrite(0, "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends", 64);

    rsa.encrypt(enc, dec);
    rsa.decrypt(dec, enc);

    EXPECT(memcmp(enc.data(), "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends", 64) == 0);
}
