/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCore/EventLoop.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontCatalog.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWebView/FontService.h>
#include <LibWebView/FontServiceHost.h>

namespace {

struct RenderSideFontService {
    NonnullRefPtr<WebView::FontService> font_service;
    NonnullOwnPtr<WebView::FontServiceHost> host;
    NonnullOwnPtr<Compositing::FontServiceClient> client;
};

RenderSideFontService connect_render_side_font_service()
{
    auto font_service = WebView::FontService::create({});
    auto host = WebView::FontServiceHost::create(*font_service);
    auto client = MUST(Compositing::FontServiceClient::create(MUST(host->connect())));
    return { move(font_service), move(host), move(client) };
}

void run_on_another_thread(Function<void()> function)
{
    auto thread = Threading::Thread::construct("RenderSideFontQuestion"sv, [function = move(function)] {
        function();
        return 0;
    });
    thread->start();
    (void)thread->join();
}

constexpr Array<u32, 4> code_points { 'A', 0x4e2d, 0x0416, 0x05d0 };

}

// The shape a renderer's render side will have: the document thread is inside a join, pumping
// nothing, while the pass it waits for needs a code point no family in its cascade covers. The
// answer has to come from a connection the document thread does not own, or this never returns.
TEST_CASE(a_question_from_another_thread_is_answered_while_the_main_thread_is_blocked)
{
    auto service = connect_render_side_font_service();

    // The document thread's loop exists and is not running, exactly as it will be during a join.
    Core::EventLoop event_loop;

    Array<u64, code_points.size()> face_ids {};
    run_on_another_thread([&] {
        for (size_t index = 0; index < code_points.size(); ++index)
            face_ids[index] = service.client->match_font_for_code_point(code_points[index], 400, Gfx::FontWidth::Normal, 0, false).face_id;
    });

    // A machine with no font at all for basic Latin cannot run this suite.
    EXPECT_NE(face_ids[0], 0u);

    // The connection answered for the rest rather than handing back nothing: the service on the
    // other end agrees about which code points it can cover.
    for (size_t index = 0; index < code_points.size(); ++index) {
        auto brokered = service.font_service->match_font_for_code_point(code_points[index], 400, Gfx::FontWidth::Normal, 0, false);
        EXPECT_EQ(brokered.face_id, face_ids[index]);
    }
}

// Two render-side threads asking at once each get their own answer.
TEST_CASE(questions_from_several_threads_at_once_are_answered_one_at_a_time)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto service = connect_render_side_font_service();
    Core::EventLoop event_loop;

    IGNORE_USE_IN_ESCAPING_LAMBDA Array<Array<u64, code_points.size()>, 2> face_ids {};
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < face_ids.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("RenderSideFontQuestion"sv, [&, thread_index] {
            for (size_t index = 0; index < code_points.size(); ++index)
                face_ids[thread_index][index] = service.client->match_font_for_code_point(code_points[index], 400, Gfx::FontWidth::Normal, 0, false).face_id;
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    EXPECT_NE(face_ids[0][0], 0u);
    EXPECT_EQ(face_ids[0], face_ids[1]);
}

// The host keeps the font service alive until its thread is gone, however long the UI process's own
// reference to the service lasts.
TEST_CASE(the_host_keeps_the_font_service_alive)
{
    RefPtr<WebView::FontService> font_service = WebView::FontService::create({});
    auto host = WebView::FontServiceHost::create(*font_service);
    auto client = MUST(Compositing::FontServiceClient::create(MUST(host->connect())));
    auto expected = font_service->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id;
    font_service = nullptr;

    Core::EventLoop event_loop;
    u64 face_id = 0;
    run_on_another_thread([&] {
        face_id = client->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id;
    });
    EXPECT_EQ(face_id, expected);
}

// One host thread answers the connections of every process, and a process that goes away takes
// only its own connection with it.
TEST_CASE(one_host_answers_several_connections_and_outlives_a_closed_one)
{
    auto font_service = WebView::FontService::create({});
    auto host = WebView::FontServiceHost::create(*font_service);
    OwnPtr<Compositing::FontServiceClient> first = MUST(Compositing::FontServiceClient::create(MUST(host->connect())));
    auto second = MUST(Compositing::FontServiceClient::create(MUST(host->connect())));
    auto expected = font_service->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id;
    EXPECT_NE(expected, 0u);

    EXPECT_EQ(first->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id, expected);
    EXPECT_EQ(second->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id, expected);

    first = nullptr;
    EXPECT_EQ(second->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id, expected);

    auto third = MUST(Compositing::FontServiceClient::create(MUST(host->connect())));
    EXPECT_EQ(third->match_font_for_code_point('A', 400, Gfx::FontWidth::Normal, 0, false).face_id, expected);
}

// The process's font provider sends a code point miss from any thread but its own over the render
// side's connection, so the miss is answered while the document thread pumps nothing.
TEST_CASE(a_code_point_miss_from_another_thread_goes_out_on_the_render_side_connection)
{
    auto service = connect_render_side_font_service();

    // The provider's own callbacks stand for the document thread's connection, which no other
    // thread may use.
    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.match_font_for_code_point = [](u32, u16, u16, u8, bool) -> Gfx::BrokeredFont { VERIFY_NOT_REACHED(); };
    auto provider = MUST(Gfx::SharedFontProvider::create_empty(1, move(callbacks)));

    Gfx::SharedFontProviderCallbacks render_side_callbacks;
    render_side_callbacks.match_font_for_code_point = [&](u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) {
        return service.client->match_font_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
    };
    provider->set_callbacks_for_other_threads(move(render_side_callbacks));

    Core::EventLoop event_loop;
    RefPtr<Gfx::Font> font;
    run_on_another_thread([&] {
        font = provider->get_font_for_code_point('A', 16, 400, Gfx::FontWidth::Normal, 0, false);
    });
    EXPECT(font);
}

// A catalog face carries a face id and no font data, so the first use of any system family has to
// ask the font service to open the file. From any thread but the document's, that question too
// goes out on the render side's connection.
TEST_CASE(a_cold_family_lookup_from_another_thread_goes_out_on_the_render_side_connection)
{
    auto service = connect_render_side_font_service();

    // The provider's own callbacks stand for the document thread's connection, which no other
    // thread may use. They are left empty, so only the render side's connection can open a face.
    auto catalog = MUST(service.font_service->clone_catalog());
    auto provider = MUST(Gfx::SharedFontProvider::create_from_catalog_file_or_empty(move(catalog.file), catalog.size, catalog.generation, {}));

    Gfx::SharedFontProviderCallbacks render_side_callbacks;
    render_side_callbacks.open_font = [&](u64 generation, u64 face_id) {
        return service.client->open_font(generation, face_id);
    };
    provider->set_callbacks_for_other_threads(move(render_side_callbacks));

    // Any installed family will do; take the first one the catalog lists.
    auto catalog_copy = MUST(service.font_service->clone_catalog());
    auto mapping = MUST(Core::MappedFile::map_from_fd_range_and_close(catalog_copy.file.take_fd(), "font catalog"sv, 0, catalog_copy.size));
    auto parsed_catalog = MUST(Gfx::FontCatalog::parse(mapping->bytes(), catalog_copy.generation));
    VERIFY(parsed_catalog->face_count() > 0);
    auto family = MUST(FlyString::from_utf8(parsed_catalog->face_at(0)->family));

    Core::EventLoop event_loop;
    size_t typefaces_seen = 0;
    run_on_another_thread([&] {
        provider->for_each_typeface_with_family_name(family, [&](Gfx::Typeface const&) {
            ++typefaces_seen;
        });
    });

    // Every file the lookup needed was opened without the main thread pumping anything.
    EXPECT(typefaces_seen > 0u);
}
