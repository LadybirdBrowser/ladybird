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
    if (process == "WebWorker"sv)
        return "Worker"sv;
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

void CrashReportsUI::load_reports()
{
    struct Group {
        ByteString id;
        StringView process;
        Optional<UnixDateTime> last_crashed_at;
        Vector<ByteString> names;
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
    for (auto const& name : names) {
        ByteString signature { "Unreadable"sv };
        StringView process = "Unreadable report"sv;
        Optional<UnixDateTime> crashed_at;

        // A report that cannot be read still counts as waiting, so it gets a row of its own to delete it from.
        if (auto report = store.saved_report(name); report.is_error()) {
            warnln("Could not read crash report {}: {}", name, report.error());
        } else {
            auto diagnostics = CrashReportDiagnostics::parse(report.value().text);
            signature = diagnostics.signature();
            process = plain_process_name(diagnostics.header("Process"sv));
            crashed_at = diagnostics.crashed_at;
        }

        auto index = group_indices.ensure(signature, [&] {
            groups.append({
                .id = encode_hex(Crypto::Hash::SHA256::hash(signature.bytes()).bytes()),
                .process = process,
                .last_crashed_at = crashed_at,
                .names = {},
            });
            return groups.size() - 1;
        });
        groups[index].names.append(name);
    }

    JsonArray serialized_groups;
    serialized_groups.ensure_capacity(groups.size());
    for (auto const& group : groups) {
        JsonArray serialized_names;
        bool is_sending = false;
        bool could_not_send = false;
        for (auto const& name : group.names) {
            serialized_names.must_append(String::from_utf8_with_replacement_character(name));
            is_sending |= outbox.is_sending(name);
            could_not_send |= outbox.could_not_send(name);
        }

        JsonObject serialized_group;
        serialized_group.set("id"sv, String::from_utf8_with_replacement_character(group.id));
        serialized_group.set("process"sv, group.process);
        if (group.last_crashed_at.has_value())
            serialized_group.set("lastCrashedAt"sv, group.last_crashed_at->milliseconds_since_epoch());
        serialized_group.set("names"sv, move(serialized_names));
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
        if (name.has_value())
            reports.append({ name->to_byte_string(), description.has_value() ? *description : String {} });
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
