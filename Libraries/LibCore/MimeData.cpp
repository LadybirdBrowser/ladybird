/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibURL/Parser.h>

namespace Core {

Vector<URL::URL> MimeData::urls() const
{
    auto it = m_data.find("text/uri-list"sv);
    if (it == m_data.end())
        return {};
    Vector<URL::URL> urls;
    for (auto& line : StringView(it->value).split_view('\n')) {
        if (auto maybe_url = URL::Parser::basic_parse(line); maybe_url.has_value())
            urls.append(maybe_url.release_value());
    }
    return urls;
}

ErrorOr<void> MimeData::set_urls(Vector<URL::URL> const& urls)
{
    StringBuilder builder;
    for (auto& url : urls) {
        TRY(builder.try_append(url.to_byte_string()));
        TRY(builder.try_append('\n'));
    }
    set_data("text/uri-list"_string, TRY(builder.to_byte_buffer()));

    return {};
}

ByteString MimeData::text() const
{
    return ByteString::copy(m_data.get("text/plain"sv).value_or({}));
}

void MimeData::set_text(ByteString const& text)
{
    set_data("text/plain"_string, text.to_byte_buffer());
}

// FIXME: Share this, TextEditor and HackStudio language detection somehow.
static Array constexpr s_plaintext_suffixes = {
    // Extensions
    ".c"sv,
    ".cpp"sv,
    ".gml"sv,
    ".h"sv,
    ".hpp"sv,
    ".ini"sv,
    ".ipc"sv,
    ".txt"sv,

    // Base names
    ".history"sv,
    ".shellrc"sv
    "CMakeLists.txt"sv,
};

// See https://www.iana.org/assignments/media-types/<mime-type> for a list of registered MIME types.
// For example, https://www.iana.org/assignments/media-types/application/gzip
static Array const s_registered_mime_type = {
    MimeType { .name = "application/gzip"sv, .common_extensions = { ".gz"sv, ".gzip"sv }, .description = "GZIP compressed data"sv, .magic_bytes = Vector<u8> { 0x1F, 0x8B } },
    MimeType { .name = "application/javascript"sv, .common_extensions = { ".js"sv, ".mjs"sv }, .description = "JavaScript source"sv },
    MimeType { .name = "application/json"sv, .common_extensions = { ".json"sv }, .description = "JSON data"sv },
    MimeType { .name = "application/pdf"sv, .common_extensions = { ".pdf"sv }, .description = "PDF document"sv, .magic_bytes = Vector<u8> { 0x25, 'P', 'D', 'F', 0x2D } },
    MimeType { .name = "application/rtf"sv, .common_extensions = { ".rtf"sv }, .description = "Rich text file"sv, .magic_bytes = Vector<u8> { 0x7B, 0x5C, 0x72, 0x74, 0x66, 0x31 } },
    MimeType { .name = "application/tar"sv, .common_extensions = { ".tar"sv }, .description = "Tape archive"sv, .magic_bytes = Vector<u8> { 0x75, 0x73, 0x74, 0x61, 0x72 }, .offset = 0x101 },
    MimeType { .name = "application/vnd.iccprofile"sv, .common_extensions = { ".icc"sv }, .description = "ICC color profile"sv, .magic_bytes = Vector<u8> { 'a', 'c', 's', 'p' }, .offset = 36 },
    MimeType { .name = "application/vnd.sqlite3"sv, .common_extensions = { ".sqlite"sv }, .description = "SQLite database"sv, .magic_bytes = Vector<u8> { 'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', 0x00 } },
    MimeType { .name = "application/wasm"sv, .common_extensions = { ".wasm"sv }, .description = "WebAssembly bytecode"sv, .magic_bytes = Vector<u8> { 0x00, 'a', 's', 'm' } },
    MimeType { .name = "application/x-7z-compressed"sv, .common_extensions = { "7z"sv }, .description = "7-Zip archive"sv, .magic_bytes = Vector<u8> { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C } },
    MimeType { .name = "application/x-blender"sv, .common_extensions = { ".blend"sv, ".blended"sv }, .description = "Blender project file"sv, .magic_bytes = Vector<u8> { 'B', 'L', 'E', 'N', 'D', 'E', 'R' } },
    MimeType { .name = "application/x-bzip2"sv, .common_extensions = { ".bz2"sv }, .description = "BZIP2 compressed data"sv, .magic_bytes = Vector<u8> { 'B', 'Z', 'h' } },
    MimeType { .name = "application/x-sheets+json"sv, .common_extensions = { ".sheets"sv }, .description = "Serenity Spreadsheet document"sv },
    MimeType { .name = "application/xhtml+xml"sv, .common_extensions = { ".xhtml"sv, ".xht"sv }, .description = "XHTML document"sv },
    MimeType { .name = "application/zip"sv, .common_extensions = { ".zip"sv }, .description = "ZIP archive"sv, .magic_bytes = Vector<u8> { 0x50, 0x4B } },

    MimeType { .name = "audio/flac"sv, .common_extensions = { ".flac"sv }, .description = "FLAC audio"sv, .magic_bytes = Vector<u8> { 'f', 'L', 'a', 'C' } },
    MimeType { .name = "audio/midi"sv, .common_extensions = { ".mid"sv }, .description = "MIDI notes"sv, .magic_bytes = Vector<u8> { 0x4D, 0x54, 0x68, 0x64 } },
    MimeType { .name = "audio/mpeg"sv, .common_extensions = { ".mp3"sv }, .description = "MP3 audio"sv, .magic_bytes = Vector<u8> { 0xFF, 0xFB } },
    MimeType { .name = "audio/qoa"sv, .common_extensions = { ".qoa"sv }, .description = "Quite OK Audio"sv, .magic_bytes = Vector<u8> { 'q', 'o', 'a', 'f' } },
    MimeType { .name = "audio/wav"sv, .common_extensions = { ".wav"sv }, .description = "WAVE audio"sv, .magic_bytes = Vector<u8> { 'W', 'A', 'V', 'E' }, .offset = 8 },

    MimeType { .name = "extra/elf"sv, .common_extensions = { ".elf"sv }, .description = "ELF"sv, .magic_bytes = Vector<u8> { 0x7F, 'E', 'L', 'F' } },
    MimeType { .name = "extra/ext"sv, .description = "EXT filesystem"sv, .magic_bytes = Vector<u8> { 0x53, 0xEF }, .offset = 0x438 },
    MimeType { .name = "extra/iso-9660"sv, .common_extensions = { ".iso"sv }, .description = "ISO 9660 CD/DVD image"sv, .magic_bytes = Vector<u8> { 0x43, 0x44, 0x30, 0x30, 0x31 }, .offset = 0x8001 },
    MimeType { .name = "extra/iso-9660"sv, .common_extensions = { ".iso"sv }, .description = "ISO 9660 CD/DVD image"sv, .magic_bytes = Vector<u8> { 0x43, 0x44, 0x30, 0x30, 0x31 }, .offset = 0x8801 },
    MimeType { .name = "extra/iso-9660"sv, .common_extensions = { ".iso"sv }, .description = "ISO 9660 CD/DVD image"sv, .magic_bytes = Vector<u8> { 0x43, 0x44, 0x30, 0x30, 0x31 }, .offset = 0x9001 },
    MimeType { .name = "extra/isz"sv, .common_extensions = { ".isz"sv }, .description = "Compressed ISO image"sv, .magic_bytes = Vector<u8> { 'I', 's', 'Z', '!' } },
    MimeType { .name = "extra/lua-bytecode"sv, .description = "Lua bytecode"sv, .magic_bytes = Vector<u8> { 0x1B, 'L', 'u', 'a' } },
    MimeType { .name = "extra/nes-rom"sv, .common_extensions = { ".nes"sv }, .description = "Nintendo Entertainment System ROM"sv, .magic_bytes = Vector<u8> { 'N', 'E', 'S', 0x1A } },
    MimeType { .name = "extra/qcow"sv, .common_extensions = { ".qcow"sv, ".qcow2"sv, ".qcow3"sv }, .description = "QCOW file"sv, .magic_bytes = Vector<u8> { 'Q', 'F', 'I' } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0x01 } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0x5E } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0x9C } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0xDA } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0x20 } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0x7D } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0xBB } },
    MimeType { .name = "extra/raw-zlib"sv, .description = "Raw zlib stream"sv, .magic_bytes = Vector<u8> { 0x78, 0xF9 } },
    MimeType { .name = "extra/win-31x-compressed"sv, .description = "Windows 3.1X compressed file"sv, .magic_bytes = Vector<u8> { 'K', 'W', 'A', 'J' } },
    MimeType { .name = "extra/win-95-compressed"sv, .description = "Windows 95 compressed file"sv, .magic_bytes = Vector<u8> { 'S', 'Z', 'D', 'D' } },

    MimeType { .name = "font/otf"sv, .common_extensions = { "otf"sv }, .description = "OpenType font"sv, .magic_bytes = Vector<u8> { 'O', 'T', 'T', 'F' } },
    MimeType { .name = "font/ttf"sv, .common_extensions = { "ttf"sv }, .description = "TrueType font"sv, .magic_bytes = Vector<u8> { 0x00, 0x01, 0x00, 0x00, 0x00 } },
    MimeType { .name = "font/woff"sv, .common_extensions = { "woff"sv }, .description = "WOFF font"sv, .magic_bytes = Vector<u8> { 'W', 'O', 'F', 'F' } },
    MimeType { .name = "font/woff2"sv, .common_extensions = { "woff2"sv }, .description = "WOFF2 font"sv, .magic_bytes = Vector<u8> { 'W', 'O', 'F', '2' } },

    MimeType { .name = "image/bmp"sv, .common_extensions = { ".bmp"sv }, .description = "BMP image data"sv, .magic_bytes = Vector<u8> { 'B', 'M' } },
    MimeType { .name = "image/gif"sv, .common_extensions = { ".gif"sv }, .description = "GIF image data"sv, .magic_bytes = Vector<u8> { 'G', 'I', 'F', '8', '7', 'a' } },
    MimeType { .name = "image/gif"sv, .common_extensions = { ".gif"sv }, .description = "GIF image data"sv, .magic_bytes = Vector<u8> { 'G', 'I', 'F', '8', '9', 'a' } },
    MimeType { .name = "image/j2c"sv, .common_extensions = { ".j2c"sv, ".j2k"sv }, .description = "JPEG2000 image data codestream"sv, .magic_bytes = Vector<u8> { 0xFF, 0x4F, 0xFF, 0x51 } },
    MimeType { .name = "image/jp2"sv, .common_extensions = { ".jp2"sv, ".jpf"sv, ".jpx"sv }, .description = "JPEG2000 image data"sv, .magic_bytes = Vector<u8> { 0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A } },
    MimeType { .name = "image/jpeg"sv, .common_extensions = { ".jpg"sv, ".jpeg"sv }, .description = "JPEG image data"sv, .magic_bytes = Vector<u8> { 0xFF, 0xD8, 0xFF } },
    MimeType { .name = "image/jxl"sv, .common_extensions = { ".jxl"sv }, .description = "JPEG XL image data"sv, .magic_bytes = Vector<u8> { 0xFF, 0x0A } },
    MimeType { .name = "image/png"sv, .common_extensions = { ".png"sv }, .description = "PNG image data"sv, .magic_bytes = Vector<u8> { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A } },
    MimeType { .name = "image/svg+xml"sv, .common_extensions = { ".svg"sv }, .description = "Scalable Vector Graphics image"sv },
    MimeType { .name = "image/tiff"sv, .common_extensions = { ".tiff"sv }, .description = "TIFF image data"sv, .magic_bytes = Vector<u8> { 'I', 'I', '*', 0x00 } },
    MimeType { .name = "image/tiff"sv, .common_extensions = { ".tiff"sv }, .description = "TIFF image data"sv, .magic_bytes = Vector<u8> { 'M', 'M', 0x00, '*' } },
    MimeType { .name = "image/tinyvg"sv, .common_extensions = { ".tvg"sv }, .description = "TinyVG vector graphics"sv, .magic_bytes = Vector<u8> { 0x72, 0x56 } },
    MimeType { .name = "image/webp"sv, .common_extensions = { ".webp"sv }, .description = "WebP image data"sv, .magic_bytes = Vector<u8> { 'W', 'E', 'B', 'P' }, .offset = 8 },
    MimeType { .name = "image/x-icon"sv, .common_extensions = { ".ico"sv }, .description = "ICO image data"sv },
    MimeType { .name = "image/x-ilbm"sv, .common_extensions = { ".iff"sv, ".lbm"sv }, .description = "Interleaved bitmap image data"sv, .magic_bytes = Vector<u8> { 0x46, 0x4F, 0x52, 0x4F } },
    MimeType { .name = "image/x-jbig2"sv, .common_extensions = { ".jbig2"sv, ".jb2"sv }, .description = "JBIG2 image data"sv, .magic_bytes = Vector<u8> { 0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A } },
    MimeType { .name = "image/x-portable-arbitrarymap"sv, .common_extensions = { ".pam"sv }, .description = "PAM image data"sv, .magic_bytes = Vector<u8> { 0x50, 0x37, 0x0A } },
    MimeType { .name = "image/x-portable-bitmap"sv, .common_extensions = { ".pbm"sv }, .description = "PBM image data"sv, .magic_bytes = Vector<u8> { 0x50, 0x31, 0x0A } },
    MimeType { .name = "image/x-portable-graymap"sv, .common_extensions = { ".pgm"sv }, .description = "PGM image data"sv, .magic_bytes = Vector<u8> { 0x50, 0x32, 0x0A } },
    MimeType { .name = "image/x-portable-pixmap"sv, .common_extensions = { ".ppm"sv }, .description = "PPM image data"sv, .magic_bytes = Vector<u8> { 0x50, 0x33, 0x0A } },
    MimeType { .name = "image/x-targa"sv, .common_extensions = { ".tga"sv }, .description = "Targa image data"sv },

    MimeType { .name = "text/css"sv, .common_extensions = { ".css"sv }, .description = "Cascading Style Sheet"sv },
    MimeType { .name = "text/csv"sv, .common_extensions = { ".csv"sv }, .description = "CSV text"sv },
    MimeType { .name = "text/html"sv, .common_extensions = { ".html"sv, ".htm"sv, ".xht"sv, "/"sv }, .description = "HTML document"sv }, // FIXME: The "/" seems dubious
    MimeType { .name = "text/xml"sv, .common_extensions = { ".xml"sv }, .description = "XML document"sv },
    MimeType { .name = "text/markdown"sv, .common_extensions = { ".md"sv }, .description = "Markdown document"sv },
    MimeType { .name = "text/plain"sv, .common_extensions = Vector(s_plaintext_suffixes.span()), .description = "plain text"sv },
    MimeType { .name = "text/x-shellscript"sv, .common_extensions = { ".sh"sv }, .description = "POSIX shell script text executable"sv, .magic_bytes = Vector<u8> { '#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n' } },

    MimeType { .name = "video/matroska"sv, .common_extensions = { ".mkv"sv }, .description = "Matroska container"sv, .magic_bytes = Vector<u8> { 0x1A, 0x45, 0xDF, 0xA3 } },
    MimeType { .name = "video/webm"sv, .common_extensions = { ".webm"sv }, .description = "WebM video"sv },
};

StringView guess_mime_type_based_on_filename(StringView path)
{
    for (auto const& mime_type : s_registered_mime_type) {
        for (auto const possible_extension : mime_type.common_extensions) {
            if (path.ends_with(possible_extension))
                return mime_type.name;
        }
    }

    return "application/octet-stream"sv;
}

Optional<StringView> guess_mime_type_based_on_sniffed_bytes(ReadonlyBytes bytes)
{
    for (auto const& mime_type : s_registered_mime_type) {
        if (mime_type.magic_bytes.has_value()
            && bytes.size() >= mime_type.offset
            && bytes.slice(mime_type.offset).starts_with(*mime_type.magic_bytes)) {
            return mime_type.name;
        }
    }

    return {};
}

Optional<MimeType const&> get_mime_type_data(StringView mime_name)
{
    for (auto const& mime_type : s_registered_mime_type) {
        if (mime_name == mime_type.name)
            return mime_type;
    }

    return {};
}

Optional<StringView> guess_mime_type_based_on_sniffed_bytes(Core::File& file)
{
    // Read accounts for longest possible offset + signature we currently match against (extra/iso-9660)
    auto maybe_buffer = ByteBuffer::create_uninitialized(0x9006);
    if (maybe_buffer.is_error())
        return {};

    auto maybe_bytes = file.read_some(maybe_buffer.value());
    if (maybe_bytes.is_error())
        return {};

    return Core::guess_mime_type_based_on_sniffed_bytes(maybe_bytes.value());
}

}
