/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumberFormat.h>
#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <LibCore/DateTime.h>
#include <LibCore/Directory.h>
#include <LibCore/Resource.h>
#include <LibCore/System.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/UserAgent.h>

namespace Web {

void set_chrome_process_command_line(StringView command_line)
{
    s_chrome_process_command_line = MUST(String::from_utf8(command_line));
}

void set_chrome_process_executable_path(StringView executable_path)
{
    s_chrome_process_executable_path = MUST(String::from_utf8(executable_path));
}

ErrorOr<String> load_error_page(URL::URL const& url, StringView error_message)
{
    // Generate HTML error page from error template file
    // FIXME: Use an actual templating engine (our own one when it's built, preferably with a way to check these usages at compile time)
    auto template_file = TRY(Core::Resource::load_from_uri("resource://ladybird/templates/error.html"sv));
    StringBuilder builder;
    SourceGenerator generator { builder, '%', '%' };
    generator.set("failed_url", escape_html_entities(url.to_byte_string()));
    generator.set("error_message", escape_html_entities(error_message));
    generator.append(template_file->data());
    return TRY(String::from_utf8(generator.as_string_view()));
}

ErrorOr<String> load_file_directory_page(URL::URL const& url)
{
    // Generate HTML contents entries table
    auto lexical_path = LexicalPath(URL::percent_decode(url.serialize_path()));
    Core::DirIterator dt(lexical_path.string(), Core::DirIterator::Flags::SkipParentAndBaseDir);
    Vector<ByteString> names;
    while (dt.has_next())
        names.append(dt.next_path());
    quick_sort(names);

    StringBuilder contents;
    contents.append("<table>"sv);
    for (auto& name : names) {
        auto path = lexical_path.append(name);
        auto maybe_st = Core::System::stat(path.string());
        if (!maybe_st.is_error()) {
            auto st = maybe_st.release_value();
            auto is_directory = S_ISDIR(st.st_mode);

            contents.append("<tr>"sv);
            contents.appendff("<td><span class=\"{}\"></span></td>", is_directory ? "folder" : "file");
            contents.appendff("<td><a href=\"file://{}\">{}</a></td><td>&nbsp;</td>"sv, path, name);
            contents.appendff("<td>{:10}</td><td>&nbsp;</td>", is_directory ? "-"_string : human_readable_size(st.st_size));
            contents.appendff("<td>{}</td>"sv, Core::DateTime::from_timestamp(st.st_mtime).to_byte_string());
            contents.append("</tr>\n"sv);
        }
    }
    contents.append("</table>"sv);

    // Generate HTML directory page from directory template file
    // FIXME: Use an actual templating engine (our own one when it's built, preferably with a way to check these usages at compile time)
    auto template_file = TRY(Core::Resource::load_from_uri("resource://ladybird/templates/directory.html"sv));
    StringBuilder builder;
    SourceGenerator generator { builder, '%', '%' };
    generator.set("path", escape_html_entities(lexical_path.string()));
    generator.set("parent_url", TRY(String::formatted("file://{}", escape_html_entities(lexical_path.parent().string()))));
    generator.set("contents", contents.to_byte_string());
    generator.append(template_file->data());
    return TRY(String::from_utf8(generator.as_string_view()));
}

ErrorOr<String> load_about_version_page()
{
    // Generate HTML about version page from template file
    // FIXME: Use an actual templating engine (our own one when it's built, preferably with a way to check these usages at compile time)
    auto template_file = TRY(Core::Resource::load_from_uri("resource://ladybird/templates/version.html"sv));
    StringBuilder builder;
    SourceGenerator generator { builder, '%', '%' };
    generator.set("browser_name", BROWSER_NAME);
    generator.set("browser_version", BROWSER_VERSION);
    generator.set("arch_name", CPU_STRING);
    generator.set("os_name", OS_STRING);
    generator.set("user_agent", default_user_agent);
    generator.set("command_line", s_chrome_process_command_line);
    generator.set("executable_path", s_chrome_process_executable_path);
    generator.append(template_file->data());
    return TRY(String::from_utf8(generator.as_string_view()));
}

}
