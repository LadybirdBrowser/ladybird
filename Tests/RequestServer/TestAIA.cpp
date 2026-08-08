/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Base64.h>
#include <AK/ByteBuffer.h>
#include <LibTest/TestCase.h>
#include <RequestServer/AIA.h>

#include <openssl/x509.h>

// Generated certificate fixtures (EC/prime256v1, fixed validity 2020-2050):
// - LEAF: subject CN "leaf.aia.test"; AIA extension with one http caIssuers URL, one https caIssuers URL, one http OCSP URL.
// - CA:   self-signed "AIA Test Root CA"; no AIA extension.
// - MANY: subject CN "many.aia.test"; AIA extension carrying six http caIssuers URLs.
// - PKCS7_TWO_CERTS: a PKCS#7 "certs-only" bundle containing [CA, leaf].
static constexpr auto leaf_der_base64 = "MIIBnTCCAUSgAwIBAgIBAjAKBggqhkjOPQQDAjAbMRkwFwYDVQQDDBBBSUEgVGVzdCBSb290IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIwNTAwMTAxMDAwMDAwWjAYMRYwFAYDVQQDDA1sZWFmLmFpYS50ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYsSP5b9NEG+zYqFyllVmL2kHe/eNXc0XUsRDFdTEBEFxn7I2cT7f8h8yjcrf63UgeEToVHWW5ySz1tzdbzCk9aN6MHgwdgYIKwYBBQUHAQEEajBoMCIGCCsGAQUFBzAChhZodHRwOi8vYWlhLnRlc3QvY2EuY3J0MCMGCCsGAQUFBzAChhdodHRwczovL2FpYS50ZXN0L2NhLmNydDAdBggrBgEFBQcwAYYRaHR0cDovL29jc3AudGVzdC8wCgYIKoZIzj0EAwIDRwAwRAIgR3a5MGrx7BBesYV8arbwfeBIprTELDzniCerSZuyHjgCIHn480IuXwqGJBJcBwqwvFcyRLEnfVI1HqmkCDElMA3C"sv;
static constexpr auto ca_der_base64 = "MIIBODCB4KADAgECAgEBMAoGCCqGSM49BAMCMBsxGTAXBgNVBAMMEEFJQSBUZXN0IFJvb3QgQ0EwIBcNMjAwMTAxMDAwMDAwWhgPMjA1MDAxMDEwMDAwMDBaMBsxGTAXBgNVBAMMEEFJQSBUZXN0IFJvb3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQvFaFCRjw2v74zuhfRNy6CQSj16eQo3DvL5UjoJo6Ic+jlA/En7yPQWBTdksQFW/3mxWqeoSShdsEzEVEkC84toxMwETAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49BAMCA0cAMEQCIFcoD2A6nwl2nz8sgjQUl1h/vEmP2ou36HfWDjHNrUDBAiBuf54FrlwV+CbAPcABgP5ktbYmt7Ho0zaKWSuDSI/EeA=="sv;
static constexpr auto many_der_base64 = "MIICGTCCAb+gAwIBAgIBAjAKBggqhkjOPQQDAjAbMRkwFwYDVQQDDBBBSUEgVGVzdCBSb290IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIwNTAwMTAxMDAwMDAwWjAYMRYwFAYDVQQDDA1tYW55LmFpYS50ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAExI4Mkk2uxbNU9fEXyoU9VSo172CiHbWDDbRkOhesAmAJHuBTleAkmTLAEWugT6bILtQTKCq5GldbYwMrHMJ2PaOB9DCB8TCB7gYIKwYBBQUHAQEEgeEwgd4wIwYIKwYBBQUHMAKGF2h0dHA6Ly9haWEudGVzdC9jYTAuY3J0MCMGCCsGAQUFBzAChhdodHRwOi8vYWlhLnRlc3QvY2ExLmNydDAjBggrBgEFBQcwAoYXaHR0cDovL2FpYS50ZXN0L2NhMi5jcnQwIwYIKwYBBQUHMAKGF2h0dHA6Ly9haWEudGVzdC9jYTMuY3J0MCMGCCsGAQUFBzAChhdodHRwOi8vYWlhLnRlc3QvY2E0LmNydDAjBggrBgEFBQcwAoYXaHR0cDovL2FpYS50ZXN0L2NhNS5jcnQwCgYIKoZIzj0EAwIDSAAwRQIgGwvZA7qgH9wcYmBZGRbWDbdsKQ7vbzyt27lQkobpVOgCIQCZZz5VBu6Ycv7e7i44STvL7tpiu2CeTqq5WQk6ZSvH+g=="sv;
static constexpr auto pkcs7_two_certs_base64 = "MIIDCAYJKoZIhvcNAQcCoIIC+TCCAvUCAQExADALBgkqhkiG9w0BBwGgggLdMIIBODCB4KADAgECAgEBMAoGCCqGSM49BAMCMBsxGTAXBgNVBAMMEEFJQSBUZXN0IFJvb3QgQ0EwIBcNMjAwMTAxMDAwMDAwWhgPMjA1MDAxMDEwMDAwMDBaMBsxGTAXBgNVBAMMEEFJQSBUZXN0IFJvb3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQvFaFCRjw2v74zuhfRNy6CQSj16eQo3DvL5UjoJo6Ic+jlA/En7yPQWBTdksQFW/3mxWqeoSShdsEzEVEkC84toxMwETAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49BAMCA0cAMEQCIFcoD2A6nwl2nz8sgjQUl1h/vEmP2ou36HfWDjHNrUDBAiBuf54FrlwV+CbAPcABgP5ktbYmt7Ho0zaKWSuDSI/EeDCCAZ0wggFEoAMCAQICAQIwCgYIKoZIzj0EAwIwGzEZMBcGA1UEAwwQQUlBIFRlc3QgUm9vdCBDQTAgFw0yMDAxMDEwMDAwMDBaGA8yMDUwMDEwMTAwMDAwMFowGDEWMBQGA1UEAwwNbGVhZi5haWEudGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABGLEj+W/TRBvs2KhcpZVZi9pB3v3jV3NF1LEQxXUxARBcZ+yNnE+3/IfMo3K3+t1IHhE6FR1lucks9bc3W8wpPWjejB4MHYGCCsGAQUFBwEBBGowaDAiBggrBgEFBQcwAoYWaHR0cDovL2FpYS50ZXN0L2NhLmNydDAjBggrBgEFBQcwAoYXaHR0cHM6Ly9haWEudGVzdC9jYS5jcnQwHQYIKwYBBQUHMAGGEWh0dHA6Ly9vY3NwLnRlc3QvMAoGCCqGSM49BAMCA0cAMEQCIEd2uTBq8ewQXrGFfGq28H3gSKa0xCw854gnq0mbsh44AiB5+PNCLl8KhiQSXAcKsLxXMkSxJ31SNR6ppAgxJTANwjEA"sv;
static constexpr auto ca_pem = R"PEM(-----BEGIN CERTIFICATE-----
MIIBODCB4KADAgECAgEBMAoGCCqGSM49BAMCMBsxGTAXBgNVBAMMEEFJQSBUZXN0
IFJvb3QgQ0EwIBcNMjAwMTAxMDAwMDAwWhgPMjA1MDAxMDEwMDAwMDBaMBsxGTAX
BgNVBAMMEEFJQSBUZXN0IFJvb3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNC
AAQvFaFCRjw2v74zuhfRNy6CQSj16eQo3DvL5UjoJo6Ic+jlA/En7yPQWBTdksQF
W/3mxWqeoSShdsEzEVEkC84toxMwETAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49
BAMCA0cAMEQCIFcoD2A6nwl2nz8sgjQUl1h/vEmP2ou36HfWDjHNrUDBAiBuf54F
rlwV+CbAPcABgP5ktbYmt7Ho0zaKWSuDSI/EeA==
-----END CERTIFICATE-----)PEM"sv;

static X509* parse_der(StringView base64)
{
    auto der = MUST(decode_base64(base64));
    auto const* data = der.data();
    return d2i_X509(nullptr, &data, static_cast<long>(der.size()));
}

static ByteString subject_common_name(X509* certificate)
{
    char buffer[256] = {};
    X509_NAME_get_text_by_NID(X509_get_subject_name(certificate), NID_commonName, buffer, sizeof(buffer));
    return ByteString { buffer };
}

TEST_CASE(ca_issuers_urls_returns_only_http_ca_issuers)
{
    auto* leaf = parse_der(leaf_der_base64);
    EXPECT_NE(leaf, nullptr);

    // Of the three access descriptions, only the http caIssuers URL is returned: The https caIssuers URL is skipped
    // (fetching over TLS would itself need verification), and the OCSP URL isn't a caIssuers entry.
    auto urls = RequestServer::ca_issuers_urls(leaf);
    EXPECT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://aia.test/ca.crt"sv);

    X509_free(leaf);
}

TEST_CASE(ca_issuers_urls_is_capped)
{
    auto* many = parse_der(many_der_base64);
    EXPECT_NE(many, nullptr);

    // The certificate carries six http caIssuers URLs; extraction is capped at five.
    EXPECT_EQ(RequestServer::ca_issuers_urls(many).size(), 5u);

    X509_free(many);
}

TEST_CASE(ca_issuers_urls_empty_without_extension)
{
    auto* ca = parse_der(ca_der_base64);
    EXPECT_NE(ca, nullptr);

    EXPECT(RequestServer::ca_issuers_urls(ca).is_empty());

    X509_free(ca);
}

TEST_CASE(parse_certificates_reads_der)
{
    auto der = MUST(decode_base64(leaf_der_base64));
    auto certificates = RequestServer::parse_certificates(der.bytes());
    EXPECT_EQ(certificates.size(), 1u);
    EXPECT_EQ(subject_common_name(certificates[0]), "leaf.aia.test"sv);
    for (auto* certificate : certificates)
        X509_free(certificate);
}

TEST_CASE(parse_certificates_reads_pkcs7_certs_only_bundle)
{
    // A PKCS#7 "certs-only" response can carry more than one certificate; all are returned.
    auto der = MUST(decode_base64(pkcs7_two_certs_base64));
    auto certificates = RequestServer::parse_certificates(der.bytes());
    EXPECT_EQ(certificates.size(), 2u);
    for (auto* certificate : certificates)
        X509_free(certificate);
}

TEST_CASE(parse_certificates_reads_pem)
{
    auto certificates = RequestServer::parse_certificates(ca_pem.bytes());
    EXPECT_EQ(certificates.size(), 1u);
    EXPECT_EQ(subject_common_name(certificates[0]), "AIA Test Root CA"sv);
    for (auto* certificate : certificates)
        X509_free(certificate);
}

TEST_CASE(parse_certificates_rejects_garbage)
{
    EXPECT(RequestServer::parse_certificates("this is not a certificate"sv.bytes()).is_empty());
}

TEST_CASE(parse_certificates_handles_degenerate_pkcs7_without_crashing)
{
    // A PKCS#7 SignedData whose (OPTIONAL) content is absent decodes with a NULL content pointer; parsing must not
    // dereference it. Body: SEQUENCE { OBJECT IDENTIFIER signedData } with no content.
    static constexpr Array<u8, 13> degenerate_pkcs7 { 0x30, 0x0B, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02 };
    EXPECT(RequestServer::parse_certificates(degenerate_pkcs7).is_empty());
}
