/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibURL/Parser.h>
#include <LibWebView/CrashReportDiagnostics.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/WebUI/CrashReportsUI.h>

namespace WebView {

static StringView plain_process_name(StringView process)
{
    if (process == "Compositor"sv)
        return "Graphics process"sv;
    if (process == "WebContent"sv)
        return "Page"sv;
    if (process == "RequestServer"sv)
        return "Network process"sv;
    if (process == "ImageDecoder"sv)
        return "Image decoder"sv;
    if (process == "MediaServer"sv)
        return "Media process"sv;
    if (process == "AudioServer"sv)
        return "Audio process"sv;
    if (process == "VideoDecoderServer"sv)
        return "Video decoder"sv;
    if (process == "WebRTCClient"sv)
        return "WebRTC process"sv;
    return "Ladybird"sv;
}

static Optional<String> website_host(String const& website)
{
    auto url = URL::Parser::basic_parse(website);
    if (!url.has_value() || !url->host().has_value())
        return {};
    return MUST(String::from_utf8(url->serialized_host()));
}

void CrashReportsUI::register_interfaces()
{
    register_interface("loadReports"sv, [this](auto const&) {
        load_reports();
    });
    register_interface("sendReports"sv, [this](auto const& data) {
        send_reports(data);
    });
    register_interface("deleteReports"sv, [this](auto const& data) {
        delete_reports(data);
    });
    register_interface("showReport"sv, [this](auto const& data) {
        show_report(data);
    });
}

void CrashReportsUI::crash_reports_changed()
{
    if (has_client())
        load_reports();
}

static CrashReportsUI::ReportSummary read_report_summary(CrashReportStore const& store, ByteString const& name)
{
    // Only a stack can show that two reports are the same crash, so every other report gets a row of its own.
    CrashReportsUI::ReportSummary summary { name, "Unreadable report"_string, {} };

    // A report that cannot be read still counts as waiting, so it gets a row to delete it from.
    auto report = store.saved_report(name);
    if (report.is_error()) {
        warnln("Could not read crash report {}: {}", name, report.error());
        return summary;
    }

    auto diagnostics = CrashReportDiagnostics::parse(report.value().text);
    if (diagnostics.has_native_stack())
        summary.signature = diagnostics.signature();
    summary.process = MUST(String::from_utf8(plain_process_name(diagnostics.header("Process"sv))));
    summary.crashed_at = diagnostics.crashed_at;
    return summary;
}

void CrashReportsUI::load_reports()
{
    struct Group {
        ByteString id;
        String process;
        Optional<UnixDateTime> last_crashed_at;
        Vector<ByteString> names;
        Vector<String> websites;
        Vector<Optional<String>> website_of_each_report;
        Vector<String> hosts;
    };

    auto& store = CrashReportStore::the();
    auto& outbox = CrashReportOutbox::the();

    Vector<ByteString> names;
    if (auto pending = store.pending_report_names(); pending.is_error())
        warnln("Could not list crash reports: {}", pending.error());
    else
        names = pending.release_value();

    // The names come newest first, so a group's first report is its last crash.
    Vector<Group> groups;
    HashMap<ByteString, size_t> group_indices;
    HashMap<ByteString, ReportSummary> report_summaries;
    for (auto const& name : names) {
        // A saved report does not change, and this runs each time any report does.
        auto cached_summary = m_report_summaries.get(name);
        auto summary = cached_summary.has_value() ? cached_summary.value() : read_report_summary(store, name);
        report_summaries.set(name, summary);
        auto const& [signature, process, crashed_at] = summary;

        auto index = group_indices.ensure(signature, [&] {
            groups.append({
                .id = encode_hex(Crypto::Hash::SHA256::hash(signature.bytes()).bytes()),
                .process = process,
                .last_crashed_at = crashed_at,
                .names = {},
                .websites = {},
                .website_of_each_report = {},
                .hosts = {},
            });
            return groups.size() - 1;
        });
        auto& group = groups[index];
        group.names.append(name);

        auto website = store.website(name);
        group.website_of_each_report.append(website);
        if (website.has_value()) {
            if (auto host = website_host(*website); host.has_value() && !group.hosts.contains_slow(*host))
                group.hosts.append(host.release_value());
            if (!group.websites.contains_slow(*website))
                group.websites.append(website.release_value());
        }
    }

    m_report_summaries = move(report_summaries);

    JsonArray serialized_groups;
    serialized_groups.ensure_capacity(groups.size());
    for (auto const& group : groups) {
        JsonArray serialized_names;
        bool is_sending = false;
        bool could_not_send = false;
        String note;
        JsonArray included_websites;
        for (auto const& name : group.names) {
            serialized_names.must_append(String::from_utf8_with_replacement_character(name));
            is_sending |= outbox.is_sending(name);
            could_not_send |= outbox.could_not_send(name);
            if (auto attempt = outbox.failed_attempt(name); attempt.has_value()) {
                if (note.is_empty())
                    note = attempt->description;
                if (attempt->url.has_value())
                    included_websites.must_append(*attempt->url);
            }
        }

        JsonObject serialized_group;
        serialized_group.set("id"sv, String::from_utf8_with_replacement_character(group.id));
        // A row only names a site when that is the whole truth about the websites it would send.
        if (group.hosts.size() == 1)
            serialized_group.set("process"sv, MUST(String::formatted("{} on {}", group.process, group.hosts.first())));
        else if (group.hosts.size() > 1)
            serialized_group.set("process"sv, MUST(String::formatted("{} on {} websites", group.process, group.hosts.size())));
        else
            serialized_group.set("process"sv, group.process);
        if (group.last_crashed_at.has_value())
            serialized_group.set("lastCrashedAt"sv, group.last_crashed_at->milliseconds_since_epoch());
        serialized_group.set("names"sv, move(serialized_names));

        JsonArray serialized_websites;
        for (auto const& website : group.websites)
            serialized_websites.must_append(website);
        serialized_group.set("websites"sv, move(serialized_websites));

        // In the order of the names, so that the page can tell which reports a ticked website belongs to.
        JsonArray serialized_website_of_each_report;
        for (auto const& website : group.website_of_each_report)
            serialized_website_of_each_report.must_append(website.has_value() ? JsonValue { *website } : JsonValue {});
        serialized_group.set("websiteOfEachReport"sv, move(serialized_website_of_each_report));

        // A page opened after a failed attempt starts from the choices that were made for it.
        serialized_group.set("note"sv, note);
        serialized_group.set("includedWebsites"sv, move(included_websites));

        serialized_group.set("isSending"sv, is_sending);
        serialized_group.set("couldNotSend"sv, could_not_send);
        serialized_groups.must_append(move(serialized_group));
    }

    async_send_message("loadReports"sv, move(serialized_groups));
}

void CrashReportsUI::send_reports(JsonValue const& data)
{
    if (!data.is_array())
        return;

    Vector<CrashReportOutbox::Report> reports;
    data.as_array().for_each([&](JsonValue const& value) {
        if (!value.is_object())
            return;
        auto name = value.as_object().get_string("name"sv);
        auto description = value.as_object().get_string("description"sv);
        if (!name.has_value())
            return;

        // The website only ever leaves this computer because the user ticked its box for this report.
        auto report_name = name->to_byte_string();
        Optional<String> url;
        if (value.as_object().get_bool("includeWebsite"sv).value_or(false))
            url = CrashReportStore::the().website(report_name);
        reports.append({ move(report_name), description.has_value() ? *description : String {}, move(url) });
    });

    CrashReportOutbox::the().send(move(reports));
}

void CrashReportsUI::delete_reports(JsonValue const& data)
{
    if (!data.is_array())
        return;

    Vector<ByteString> names;
    data.as_array().for_each([&](JsonValue const& value) {
        if (value.is_string())
            names.append(value.as_string().to_byte_string());
    });

    CrashReportOutbox::the().remove(names);
}

void CrashReportsUI::show_report(JsonValue const& data)
{
    if (!data.is_string())
        return;

    auto name = data.as_string().to_byte_string();
    auto report = CrashReportStore::the().saved_report(name);
    if (report.is_error()) {
        warnln("Could not read crash report {}: {}", name, report.error());
        return;
    }

    JsonObject message;
    message.set("name"sv, String::from_utf8_with_replacement_character(name));
    message.set("text"sv, String::from_utf8_with_replacement_character(report.value().text));
    async_send_message("showReport"sv, move(message));
}

}
