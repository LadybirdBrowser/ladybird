/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibWebView/StorageAreaRegistry.h>
#include <LibWebView/StorageJar.h>

using Web::StorageAPI::StorageAreaId;
using WebView::StorageEndpointType;

namespace {

struct Event {
    enum class Kind : u8 {
        Changed,
        Acknowledged,
        Refused,
        Reset,
    };

    Kind kind;
    StorageAreaId area_id;
    Optional<Utf16String> key;
    Optional<Utf16String> old_value;
    Optional<Utf16String> new_value;
    String url;
    Vector<WebView::StorageItem> items;
};

class FakeListener final : public WebView::StorageAreaListener {
public:
    Vector<Event> events;

    virtual void storage_area_changed(StorageAreaId area_id, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url) override
    {
        events.append({ Event::Kind::Changed, area_id, key, old_value, new_value, url, {} });
    }
    virtual void storage_area_write_acknowledged(StorageAreaId area_id) override
    {
        events.append({ Event::Kind::Acknowledged, area_id, {}, {}, {}, {}, {} });
    }
    virtual void storage_area_write_refused(StorageAreaId area_id, Vector<WebView::StorageItem> items) override
    {
        events.append({ Event::Kind::Refused, area_id, {}, {}, {}, {}, move(items) });
    }
    virtual void storage_area_reset(StorageAreaId area_id, Vector<WebView::StorageItem> items) override
    {
        events.append({ Event::Kind::Reset, area_id, {}, {}, {}, {}, move(items) });
    }
};

auto const KEY = "https://example.com"_string;
auto const URL = "https://example.com/page.html"_string;

}

TEST_CASE(connect_returns_items_and_shares_the_area_between_listeners)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    jar->set_item(StorageEndpointType::LocalStorage, KEY, "a"_utf16, "1"_utf16);

    FakeListener first;
    FakeListener second;
    auto first_connection = registry.connect(first, *jar, StorageEndpointType::LocalStorage, KEY).value();
    auto second_connection = registry.connect(second, *jar, StorageEndpointType::LocalStorage, KEY).value();

    // Every connection gets an id of its own, even to one area, so a message meant for a connection that has since
    // gone can never reach the one that took its place.
    EXPECT(first_connection.id != second_connection.id);
    EXPECT_EQ(first_connection.items.size(), 1u);
    EXPECT_EQ(first_connection.items[0].key, "a"_utf16);
    EXPECT_EQ(first_connection.items[0].value, "1"_utf16);
    EXPECT_EQ(first_connection.usage, 2u);
    EXPECT(registry.was_issued_to(first, first_connection.id));
    EXPECT(registry.was_issued_to(second, second_connection.id));
    EXPECT(!registry.was_issued_to(second, first_connection.id));

    // A different bottle of the same jar is a different area.
    auto session_connection = registry.connect(first, *jar, StorageEndpointType::SessionStorage, KEY).value();
    EXPECT(session_connection.id != first_connection.id);
    EXPECT(session_connection.items.is_empty());
}

TEST_CASE(write_fans_out_to_others_and_acks_the_writer)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    FakeListener elsewhere;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    auto b_id = registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    (void)registry.connect(elsewhere, *jar, StorageEndpointType::LocalStorage, "https://other.example"_string).value();

    registry.set_item(a, id, "k"_utf16, "v"_utf16, URL);

    EXPECT_EQ(a.events.size(), 1u);
    EXPECT_EQ(a.events[0].kind, Event::Kind::Acknowledged);
    EXPECT_EQ(a.events[0].area_id, id);

    EXPECT_EQ(b.events.size(), 1u);
    EXPECT_EQ(b.events[0].kind, Event::Kind::Changed);
    EXPECT_EQ(b.events[0].area_id, b_id);
    EXPECT_EQ(b.events[0].key, Optional<Utf16String> { "k"_utf16 });
    EXPECT_EQ(b.events[0].old_value, Optional<Utf16String> {});
    EXPECT_EQ(b.events[0].new_value, Optional<Utf16String> { "v"_utf16 });
    EXPECT_EQ(b.events[0].url, URL);

    EXPECT(elsewhere.events.is_empty());
    EXPECT_EQ(jar->get_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16), Optional<Utf16String> { "v"_utf16 });

    // A write of the value already there changes nothing, but the writer still hears it was handled.
    registry.set_item(a, id, "k"_utf16, "v"_utf16, URL);
    EXPECT_EQ(a.events.size(), 2u);
    EXPECT_EQ(a.events[1].kind, Event::Kind::Acknowledged);
    EXPECT_EQ(b.events.size(), 1u);

    registry.remove_item(a, id, "k"_utf16, URL);
    EXPECT_EQ(b.events.size(), 2u);
    EXPECT_EQ(b.events[1].kind, Event::Kind::Changed);
    EXPECT_EQ(b.events[1].old_value, Optional<Utf16String> { "v"_utf16 });
    EXPECT_EQ(b.events[1].new_value, Optional<Utf16String> {});

    registry.set_item(b, b_id, "k"_utf16, "w"_utf16, URL);
    registry.clear(a, id, URL);
    EXPECT_EQ(b.events.size(), 4u);
    EXPECT_EQ(b.events[3].kind, Event::Kind::Changed);
    EXPECT_EQ(b.events[3].key, Optional<Utf16String> {});
    EXPECT_EQ(a.events.size(), 5u);
    EXPECT_EQ(a.events[4].kind, Event::Kind::Acknowledged);
}

TEST_CASE(refused_write_sends_the_snapshot_to_the_writer_only)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    auto b_id = registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value().id;

    auto big = Utf16String::repeated('x', Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA - 8);
    registry.set_item(b, b_id, "big"_utf16, big, URL);
    a.events.clear();
    b.events.clear();

    registry.set_item(a, id, "more"_utf16, "xxxxxxxx"_utf16, URL);

    EXPECT_EQ(a.events.size(), 1u);
    EXPECT_EQ(a.events[0].kind, Event::Kind::Refused);
    EXPECT_EQ(a.events[0].area_id, id);
    EXPECT_EQ(a.events[0].items.size(), 1u);
    EXPECT_EQ(a.events[0].items[0].key, "big"_utf16);
    EXPECT(b.events.is_empty());
}

TEST_CASE(browser_side_changes_reach_every_listener_with_the_storage_key_as_url)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    auto b_id = registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value().id;

    jar->set_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16, "v"_utf16);
    EXPECT_EQ(a.events.size(), 1u);
    EXPECT_EQ(b.events.size(), 1u);
    EXPECT_EQ(a.events[0].kind, Event::Kind::Changed);
    EXPECT_EQ(a.events[0].area_id, id);
    EXPECT_EQ(b.events[0].area_id, b_id);
    EXPECT_EQ(a.events[0].url, KEY);

    jar->clear_storage_key(StorageEndpointType::LocalStorage, KEY);
    EXPECT_EQ(a.events.size(), 2u);
    EXPECT_EQ(a.events[1].kind, Event::Kind::Changed);
    EXPECT_EQ(a.events[1].key, Optional<Utf16String> {});
    EXPECT_EQ(b.events.size(), 2u);
}

TEST_CASE(reset_after_removing_items_accessed_since_reaches_every_listener)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    jar->set_item(StorageEndpointType::LocalStorage, KEY, "kept"_utf16, "1"_utf16);
    FakeListener a;
    FakeListener b;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    (void)registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value();
    a.events.clear();
    b.events.clear();

    jar->remove_items_accessed_since(UnixDateTime::earliest());

    EXPECT_EQ(a.events.size(), 1u);
    EXPECT_EQ(a.events[0].kind, Event::Kind::Reset);
    EXPECT_EQ(a.events[0].area_id, id);
    EXPECT(a.events[0].items.is_empty());
    EXPECT_EQ(b.events.size(), 1u);
    EXPECT_EQ(b.events[0].kind, Event::Kind::Reset);
}

TEST_CASE(a_removed_listener_hears_nothing_more)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    (void)registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value();

    registry.remove_listener(b);
    registry.set_item(a, id, "k"_utf16, "v"_utf16, URL);

    EXPECT(b.events.is_empty());
    EXPECT_EQ(a.events.size(), 1u);
    EXPECT(!registry.was_issued_to(b, id));
}

TEST_CASE(a_disconnected_listener_hears_nothing_more_but_keeps_its_claim)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    auto b_id = registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value().id;

    registry.disconnect(b, b_id);
    registry.set_item(a, id, "k"_utf16, "v"_utf16, URL);

    EXPECT(b.events.is_empty());
    EXPECT(registry.was_issued_to(b, b_id));

    // A write from a listener that has disconnected is dropped, not applied.
    registry.set_item(b, b_id, "k"_utf16, "stale"_utf16, URL);
    EXPECT_EQ(jar->get_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16), Optional<Utf16String> { "v"_utf16 });
    EXPECT(b.events.is_empty());
}

TEST_CASE(writes_for_a_torn_down_area_are_dropped_and_the_claim_stays_valid)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    FakeListener a;
    StorageAreaId id { 0 };
    {
        auto jar = WebView::StorageJar::create();
        id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    }

    EXPECT(registry.was_issued_to(a, id));
    registry.set_item(a, id, "k"_utf16, "v"_utf16, URL);
    registry.remove_item(a, id, "k"_utf16, URL);
    registry.clear(a, id, URL);
    registry.disconnect(a, id);
    EXPECT(a.events.is_empty());
}

TEST_CASE(reconnecting_gets_a_fresh_id_and_a_message_for_the_old_one_is_dropped)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    auto first = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    auto b_id = registry.connect(b, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    registry.disconnect(a, first);

    // The area lives on for b, and a's next connection to it still gets an id nobody has used.
    auto second = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    EXPECT(second != first);
    EXPECT(second != b_id);

    // A write a sent under its old id, still in flight when it reconnected, is dropped — not applied to the area a
    // now mirrors under the new id, and not answered under either id.
    registry.set_item(a, first, "k"_utf16, "stale"_utf16, URL);
    EXPECT_EQ(jar->get_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16), Optional<Utf16String> {});
    EXPECT(a.events.is_empty());
    EXPECT(b.events.is_empty());

    registry.set_item(a, second, "k"_utf16, "fresh"_utf16, URL);
    EXPECT_EQ(a.events.size(), 1u);
    EXPECT_EQ(a.events[0].area_id, second);
    EXPECT_EQ(b.events.size(), 1u);
    EXPECT_EQ(b.events[0].area_id, b_id);
    registry.remove_listener(a);
    registry.remove_listener(b);
}

TEST_CASE(areas_are_per_jar)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto ordinary = WebView::StorageJar::create();
    auto private_jar = WebView::StorageJar::create();
    FakeListener a;
    FakeListener b;
    auto ordinary_id = registry.connect(a, *ordinary, StorageEndpointType::LocalStorage, KEY).value().id;
    auto private_id = registry.connect(b, *private_jar, StorageEndpointType::LocalStorage, KEY).value().id;
    EXPECT(ordinary_id != private_id);

    registry.set_item(a, ordinary_id, "k"_utf16, "v"_utf16, URL);
    EXPECT(b.events.is_empty());
    EXPECT_EQ(private_jar->get_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16), Optional<Utf16String> {});
}

// Only web storage is mirrored. A process asking for any other endpoint — one with no quota, say — gets nothing, and
// the jar gets no bottle for it.
TEST_CASE(connect_refuses_endpoints_that_are_not_web_storage)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();

    FakeListener a;
    EXPECT(!registry.connect(a, *jar, StorageEndpointType::Caches, KEY).has_value());
    EXPECT(!registry.connect(a, *jar, StorageEndpointType::IndexedDB, KEY).has_value());
    EXPECT(!registry.connect(a, *jar, StorageEndpointType::ServiceWorkerRegistrations, KEY).has_value());
    EXPECT(registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).has_value());
    EXPECT(registry.connect(a, *jar, StorageEndpointType::SessionStorage, KEY).has_value());
    EXPECT_EQ(jar->bottle_usage(StorageEndpointType::Caches, KEY), 0u);
}

TEST_CASE(connect_marks_the_bottle_accessed)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    jar->set_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16, "v"_utf16);
    MUST(Core::System::sleep_ms(2));
    auto since = UnixDateTime::now();
    MUST(Core::System::sleep_ms(2));

    FakeListener a;
    (void)registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value();
    jar->remove_items_accessed_since(since);
    EXPECT_EQ(jar->get_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16), Optional<Utf16String> {});
}

TEST_CASE(devtools_hears_every_change_whoever_made_it)
{
    auto& registry = WebView::StorageAreaRegistry::the();
    auto jar = WebView::StorageJar::create();
    Vector<WebView::StorageChange::Type> seen;
    registry.on_change_for_devtools = [&](WebView::StorageJar& changed_jar, WebView::StorageChange const& change) {
        EXPECT_EQ(&changed_jar, jar.ptr());
        seen.append(change.type);
    };

    FakeListener a;
    auto id = registry.connect(a, *jar, StorageEndpointType::LocalStorage, KEY).value().id;
    registry.set_item(a, id, "k"_utf16, "v"_utf16, URL);
    jar->remove_item(StorageEndpointType::LocalStorage, KEY, "k"_utf16);

    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], WebView::StorageChange::Type::Set);
    EXPECT_EQ(seen[1], WebView::StorageChange::Type::Removed);
    registry.on_change_for_devtools = nullptr;
}
