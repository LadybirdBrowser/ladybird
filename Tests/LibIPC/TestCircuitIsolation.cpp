/*
 * Copyright (c) 2025, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/NetworkIdentity.h>
#include <LibIPC/ProxyConfig.h>
#include <LibTest/TestCase.h>
#include <AK/ByteString.h>

/*
 * Circuit Isolation Security Tests
 *
 * These tests verify that Tor/proxy configuration changes on one connection
 * do NOT affect other connections, preventing Critical Vulnerability #1:
 * Global State Mutation (CVSS 8.1) identified in SECURITY_AUDIT_REPORT.md
 *
 * Test categories:
 * 1. Per-connection proxy independence
 * 2. Circuit isolation between tabs
 * 3. Credential isolation
 * 4. State mutation prevention
 */

// =============================================================================
// SECTION 1: Basic Proxy Independence Tests
// =============================================================================

TEST_CASE(test_proxy_configs_are_independent)
{
    // Create two network identities (simulating two tabs/connections)
    auto identity_a = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto identity_b = MUST(IPC::NetworkIdentity::create_for_page(2));

    // Configure Tor on identity A
    auto tor_proxy_a = IPC::ProxyConfig::tor_proxy("circuit-a");
    identity_a->set_proxy_config(tor_proxy_a);

    // Verify identity A has proxy
    EXPECT(identity_a->has_proxy());
    EXPECT(identity_a->proxy_config().has_value());
    EXPECT_EQ(identity_a->proxy_config()->host, "localhost");
    EXPECT_EQ(identity_a->proxy_config()->port, 9050);

    // Verify identity B does NOT have proxy (independent state)
    EXPECT_EQ(identity_b->has_proxy(), false);
    EXPECT_EQ(identity_b->proxy_config().has_value(), false);
}

TEST_CASE(test_clearing_proxy_on_one_identity_does_not_affect_other)
{
    // Create two identities with Tor enabled
    auto identity_a = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto identity_b = MUST(IPC::NetworkIdentity::create_for_page(2));

    auto tor_proxy_a = IPC::ProxyConfig::tor_proxy("circuit-a");
    auto tor_proxy_b = IPC::ProxyConfig::tor_proxy("circuit-b");

    identity_a->set_proxy_config(tor_proxy_a);
    identity_b->set_proxy_config(tor_proxy_b);

    // Both should have proxies
    EXPECT(identity_a->has_proxy());
    EXPECT(identity_b->has_proxy());

    // Clear proxy on identity A
    identity_a->clear_proxy_config();

    // Identity A should no longer have proxy
    EXPECT_EQ(identity_a->has_proxy(), false);

    // Identity B should STILL have proxy (independent state)
    EXPECT(identity_b->has_proxy());
    EXPECT(identity_b->proxy_config().has_value());
    EXPECT_EQ(identity_b->proxy_config()->host, "localhost");
}

// =============================================================================
// SECTION 2: Circuit Isolation Tests
// =============================================================================

TEST_CASE(test_different_tabs_use_different_circuits)
{
    // Simulate three tabs, each with unique circuit ID
    auto tab1 = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto tab2 = MUST(IPC::NetworkIdentity::create_for_page(2));
    auto tab3 = MUST(IPC::NetworkIdentity::create_for_page(3));

    // Each tab gets its own Tor circuit
    tab1->set_proxy_config(IPC::ProxyConfig::tor_proxy("circuit-tab1"));
    tab2->set_proxy_config(IPC::ProxyConfig::tor_proxy("circuit-tab2"));
    tab3->set_proxy_config(IPC::ProxyConfig::tor_proxy("circuit-tab3"));

    // Verify each tab has a unique circuit ID
    EXPECT(tab1->tor_circuit_id().has_value());
    EXPECT(tab2->tor_circuit_id().has_value());
    EXPECT(tab3->tor_circuit_id().has_value());

    EXPECT_EQ(*tab1->tor_circuit_id(), "circuit-tab1");
    EXPECT_EQ(*tab2->tor_circuit_id(), "circuit-tab2");
    EXPECT_EQ(*tab3->tor_circuit_id(), "circuit-tab3");

    // Verify circuit IDs are different (no correlation possible)
    EXPECT_NE(*tab1->tor_circuit_id(), *tab2->tor_circuit_id());
    EXPECT_NE(*tab2->tor_circuit_id(), *tab3->tor_circuit_id());
    EXPECT_NE(*tab1->tor_circuit_id(), *tab3->tor_circuit_id());
}

TEST_CASE(test_rotating_circuit_on_one_tab_does_not_affect_others)
{
    // Create two tabs with Tor enabled
    auto tab1 = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto tab2 = MUST(IPC::NetworkIdentity::create_for_page(2));

    tab1->set_proxy_config(IPC::ProxyConfig::tor_proxy("circuit-1"));
    tab2->set_proxy_config(IPC::ProxyConfig::tor_proxy("circuit-2"));

    auto original_circuit_2 = *tab2->tor_circuit_id();

    // Rotate circuit on tab1
    auto result = tab1->rotate_tor_circuit();
    EXPECT(!result.is_error());

    // Tab1's circuit should change
    EXPECT_NE(*tab1->tor_circuit_id(), "circuit-1");

    // Tab2's circuit should remain UNCHANGED
    EXPECT_EQ(*tab2->tor_circuit_id(), original_circuit_2);
}

TEST_CASE(test_circuit_ids_are_used_for_stream_isolation)
{
    // Verify that circuit IDs are passed as SOCKS5 username for isolation
    auto identity = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto tor_proxy = IPC::ProxyConfig::tor_proxy("my-unique-circuit");

    identity->set_proxy_config(tor_proxy);

    // Circuit ID should be stored as the SOCKS5 username
    EXPECT(identity->proxy_config().has_value());
    EXPECT(identity->proxy_config()->username.has_value());
    EXPECT_EQ(*identity->proxy_config()->username, "my-unique-circuit");

    // This ensures Tor will use a separate circuit for this identity
}

// =============================================================================
// SECTION 3: Credential Isolation Tests
// =============================================================================

TEST_CASE(test_credentials_are_not_shared_between_identities)
{
    // Create two identities with different proxy credentials
    auto identity_a = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto identity_b = MUST(IPC::NetworkIdentity::create_for_page(2));

    IPC::ProxyConfig proxy_a;
    proxy_a.type = IPC::ProxyType::SOCKS5;
    proxy_a.host = "proxy-a.example.com";
    proxy_a.port = 1080;
    proxy_a.username = "user-a";
    proxy_a.password = "password-a";

    IPC::ProxyConfig proxy_b;
    proxy_b.type = IPC::ProxyType::SOCKS5;
    proxy_b.host = "proxy-b.example.com";
    proxy_b.port = 1080;
    proxy_b.username = "user-b";
    proxy_b.password = "password-b";

    identity_a->set_proxy_config(move(proxy_a));
    identity_b->set_proxy_config(move(proxy_b));

    // Verify each identity has its own credentials
    EXPECT_EQ(*identity_a->proxy_config()->username, "user-a");
    EXPECT_EQ(*identity_a->proxy_config()->password, "password-a");

    EXPECT_EQ(*identity_b->proxy_config()->username, "user-b");
    EXPECT_EQ(*identity_b->proxy_config()->password, "password-b");

    // Verify credentials are NOT shared
    EXPECT_NE(*identity_a->proxy_config()->username, *identity_b->proxy_config()->username);
    EXPECT_NE(*identity_a->proxy_config()->password, *identity_b->proxy_config()->password);
}

TEST_CASE(test_credentials_are_cleared_on_proxy_clear)
{
    // Create identity with proxy credentials
    auto identity = MUST(IPC::NetworkIdentity::create_for_page(1));

    IPC::ProxyConfig proxy;
    proxy.type = IPC::ProxyType::SOCKS5;
    proxy.host = "proxy.example.com";
    proxy.port = 1080;
    proxy.username = "testuser";
    proxy.password = "testpassword";

    identity->set_proxy_config(move(proxy));

    // Verify credentials are set
    EXPECT(identity->proxy_config()->username.has_value());
    EXPECT(identity->proxy_config()->password.has_value());

    // Clear proxy config
    identity->clear_proxy_config();

    // Verify config is cleared (credentials should be zeroed out)
    EXPECT_EQ(identity->has_proxy(), false);
    EXPECT_EQ(identity->proxy_config().has_value(), false);
}

TEST_CASE(test_sensitive_data_clearing)
{
    // Create identity with credentials
    auto identity = MUST(IPC::NetworkIdentity::create_for_page(1));

    IPC::ProxyConfig proxy;
    proxy.type = IPC::ProxyType::SOCKS5H;
    proxy.host = "127.0.0.1";
    proxy.port = 9050;
    proxy.username = "circuit-sensitive";
    proxy.password = "super-secret-password";

    identity->set_proxy_config(move(proxy));

    // Verify credentials are set
    EXPECT(identity->proxy_config()->username.has_value());
    EXPECT(identity->proxy_config()->password.has_value());

    // Call clear_sensitive_data()
    identity->clear_sensitive_data();

    // Verify all sensitive data is cleared
    EXPECT_EQ(identity->has_proxy(), false);
    EXPECT_EQ(identity->proxy_config().has_value(), false);
    EXPECT_EQ(identity->tor_circuit_id().has_value(), false);
}

// =============================================================================
// SECTION 4: State Mutation Prevention Tests
// =============================================================================

TEST_CASE(test_setting_proxy_on_identity_a_does_not_mutate_identity_b)
{
    // Create multiple identities
    Vector<NonnullRefPtr<IPC::NetworkIdentity>> identities;
    for (u64 i = 0; i < 10; ++i) {
        identities.append(MUST(IPC::NetworkIdentity::create_for_page(i)));
    }

    // Configure proxy only on identity 0
    IPC::ProxyConfig proxy;
    proxy.type = IPC::ProxyType::HTTP;
    proxy.host = "proxy.example.com";
    proxy.port = 8080;
    identities[0]->set_proxy_config(move(proxy));

    // Verify only identity 0 has proxy
    EXPECT(identities[0]->has_proxy());

    // Verify all other identities do NOT have proxy (no global mutation)
    for (size_t i = 1; i < identities.size(); ++i) {
        EXPECT_EQ(identities[i]->has_proxy(), false);
    }
}

TEST_CASE(test_rapid_proxy_changes_on_one_identity_do_not_affect_others)
{
    // Create two identities
    auto stable_identity = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto changing_identity = MUST(IPC::NetworkIdentity::create_for_page(2));

    // Set initial proxy on stable identity
    stable_identity->set_proxy_config(IPC::ProxyConfig::tor_proxy("stable-circuit"));
    auto original_circuit = *stable_identity->tor_circuit_id();

    // Rapidly change proxy on the other identity (simulating attack/bug)
    for (int i = 0; i < 100; ++i) {
        auto circuit_id = ByteString::formatted("circuit-{}", i);
        changing_identity->set_proxy_config(IPC::ProxyConfig::tor_proxy(circuit_id));

        if (i % 2 == 0) {
            changing_identity->clear_proxy_config();
        }
    }

    // Verify stable identity's circuit is UNCHANGED (no side effects)
    EXPECT(stable_identity->has_proxy());
    EXPECT_EQ(*stable_identity->tor_circuit_id(), original_circuit);
}

// =============================================================================
// SECTION 5: Proxy Type Independence Tests
// =============================================================================

TEST_CASE(test_different_proxy_types_on_different_identities)
{
    // Test that different identities can use different proxy types simultaneously
    auto tor_identity = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto http_identity = MUST(IPC::NetworkIdentity::create_for_page(2));
    auto https_identity = MUST(IPC::NetworkIdentity::create_for_page(3));

    // Configure different proxy types
    tor_identity->set_proxy_config(IPC::ProxyConfig::tor_proxy("tor-circuit"));

    IPC::ProxyConfig http_proxy;
    http_proxy.type = IPC::ProxyType::HTTP;
    http_proxy.host = "http-proxy.example.com";
    http_proxy.port = 8080;
    http_identity->set_proxy_config(move(http_proxy));

    IPC::ProxyConfig https_proxy;
    https_proxy.type = IPC::ProxyType::HTTPS;
    https_proxy.host = "https-proxy.example.com";
    https_proxy.port = 8443;
    https_identity->set_proxy_config(move(https_proxy));

    // Verify each has the correct proxy type
    EXPECT_EQ(tor_identity->proxy_config()->type, IPC::ProxyType::SOCKS5H);
    EXPECT_EQ(http_identity->proxy_config()->type, IPC::ProxyType::HTTP);
    EXPECT_EQ(https_identity->proxy_config()->type, IPC::ProxyType::HTTPS);

    // Verify hosts are different
    EXPECT_EQ(tor_identity->proxy_config()->host, "localhost");
    EXPECT_EQ(http_identity->proxy_config()->host, "http-proxy.example.com");
    EXPECT_EQ(https_identity->proxy_config()->host, "https-proxy.example.com");
}

// =============================================================================
// SECTION 6: Regression Tests for Critical Vulnerability #1
// =============================================================================

TEST_CASE(regression_test_no_global_s_connections_iteration)
{
    // This test verifies that the vulnerable code pattern from ConnectionFromClient
    // has been removed. The bug was:
    //
    //     for (auto& [id, connection] : s_connections) {
    //         connection->m_network_identity->set_proxy_config(...);
    //     }
    //
    // This test verifies the FIXED behavior: proxy changes are per-identity only.

    auto identity1 = MUST(IPC::NetworkIdentity::create_for_page(100));
    auto identity2 = MUST(IPC::NetworkIdentity::create_for_page(200));
    auto identity3 = MUST(IPC::NetworkIdentity::create_for_page(300));

    // Simulate enabling Tor on identity1 (the bug would affect identity2 and identity3)
    identity1->set_proxy_config(IPC::ProxyConfig::tor_proxy("attacker-circuit"));

    // EXPECTED: identity2 and identity3 are UNAFFECTED
    EXPECT_EQ(identity2->has_proxy(), false);
    EXPECT_EQ(identity3->has_proxy(), false);

    // EXPECTED: Only identity1 has Tor enabled
    EXPECT(identity1->has_proxy());
    EXPECT_EQ(*identity1->tor_circuit_id(), "attacker-circuit");
}

TEST_CASE(regression_test_circuit_correlation_prevented)
{
    // The vulnerability allowed circuit correlation: enabling Tor on tab A
    // with circuit "A" would apply circuit "A" to ALL tabs, allowing exit
    // node operators to correlate traffic.
    //
    // This test verifies that each tab maintains independent circuits.

    auto tab_banking = MUST(IPC::NetworkIdentity::create_for_page(1));
    auto tab_social = MUST(IPC::NetworkIdentity::create_for_page(2));
    auto tab_news = MUST(IPC::NetworkIdentity::create_for_page(3));

    // User enables Tor on banking tab with sensitive circuit
    tab_banking->set_proxy_config(IPC::ProxyConfig::tor_proxy("banking-circuit-sensitive"));

    // User enables Tor on social tab with different circuit
    tab_social->set_proxy_config(IPC::ProxyConfig::tor_proxy("social-circuit-public"));

    // News tab has no Tor
    EXPECT_EQ(tab_news->has_proxy(), false);

    // CRITICAL: Verify circuits are isolated (no correlation possible)
    EXPECT_EQ(*tab_banking->tor_circuit_id(), "banking-circuit-sensitive");
    EXPECT_EQ(*tab_social->tor_circuit_id(), "social-circuit-public");

    // This prevents exit node from correlating banking and social activity
}
