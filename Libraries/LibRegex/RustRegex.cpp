/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRegex/RustRegex.h>
#include <LibUnicode/CharacterTypes.h>

// Forward declarations for C++ functions called from Rust.
extern "C" {
bool unicode_property_matches(uint32_t, unsigned char const*, size_t, unsigned char const*, size_t, int);
uint32_t unicode_simple_case_fold(uint32_t, int);
int unicode_code_point_matches_range_ignoring_case(uint32_t, uint32_t, uint32_t, int);
int unicode_property_matches_case_insensitive(uint32_t, unsigned char const*, size_t, unsigned char const*, size_t, int);
int unicode_property_all_case_equivalents_match(uint32_t, unsigned char const*, size_t, unsigned char const*, size_t, int);
unsigned int unicode_get_case_closure(uint32_t, uint32_t*, unsigned int);
int unicode_is_string_property(unsigned char const*, size_t);
int unicode_is_valid_ecma262_property(unsigned char const*, size_t, unsigned char const*, size_t, int);
uint32_t unicode_get_string_property_data(unsigned char const*, size_t, uint32_t*, uint32_t);
int unicode_resolve_property(unsigned char const*, size_t, unsigned char const*, size_t, int, unsigned char*, uint32_t*);
int unicode_resolved_property_matches(uint32_t, unsigned char, uint32_t);
int unicode_is_id_start(uint32_t);
int unicode_is_id_continue(uint32_t);
}

extern "C" bool unicode_property_matches(
    uint32_t code_point,
    unsigned char const* name_ptr, size_t name_len,
    unsigned char const* value_ptr, size_t value_len,
    int has_value)
{
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto value = has_value
        ? StringView { reinterpret_cast<char const*>(value_ptr), value_len }
        : StringView {};

    // If there's a value, the name is a category key like "Script", "General_Category", etc.
    if (has_value) {
        // Script or Script_Extensions
        if (name.is_one_of("sc"sv, "Script"sv, "scx"sv, "Script_Extensions"sv)) {
            auto script = Unicode::script_from_string(value);
            if (!script.has_value())
                return false;
            if (name.is_one_of("sc"sv, "Script"sv))
                return Unicode::code_point_has_script(code_point, *script);
            return Unicode::code_point_has_script_extension(code_point, *script);
        }
        // General_Category
        if (name.is_one_of("gc"sv, "General_Category"sv)) {
            auto category = Unicode::general_category_from_string(value);
            if (!category.has_value())
                return false;
            return Unicode::code_point_has_general_category(code_point, *category);
        }
        return false;
    }

    // No value: could be a property name, general category, or script.
    // Try as a binary property first.
    auto prop = Unicode::property_from_string(name);
    if (prop.has_value())
        return Unicode::code_point_has_property(code_point, *prop);

    // Try as a general category.
    auto category = Unicode::general_category_from_string(name);
    if (category.has_value())
        return Unicode::code_point_has_general_category(code_point, *category);

    // Try as a script.
    auto script = Unicode::script_from_string(name);
    if (script.has_value())
        return Unicode::code_point_has_script(code_point, *script);

    return false;
}

// Property kind constants (must match Rust ResolvedProperty::kind).
enum ResolvedPropertyKind : uint8_t {
    Script = 0,
    ScriptExtension = 1,
    GeneralCategory = 2,
    BinaryProperty = 3,
};

/// Resolve a Unicode property name/value pair to a (kind, id) pair.
/// Returns 1 on success, 0 if the property is not recognized.
extern "C" int unicode_resolve_property(
    unsigned char const* name_ptr, size_t name_len,
    unsigned char const* value_ptr, size_t value_len,
    int has_value,
    unsigned char* out_kind, uint32_t* out_id)
{
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto value = has_value
        ? StringView { reinterpret_cast<char const*>(value_ptr), value_len }
        : StringView {};

    if (has_value) {
        if (name.is_one_of("sc"sv, "Script"sv, "scx"sv, "Script_Extensions"sv)) {
            auto script = Unicode::script_from_string(value);
            if (!script.has_value())
                return 0;
            *out_kind = name.is_one_of("scx"sv, "Script_Extensions"sv)
                ? ResolvedPropertyKind::ScriptExtension
                : ResolvedPropertyKind::Script;
            *out_id = script->value();
            return 1;
        }
        if (name.is_one_of("gc"sv, "General_Category"sv)) {
            auto category = Unicode::general_category_from_string(value);
            if (!category.has_value())
                return 0;
            *out_kind = ResolvedPropertyKind::GeneralCategory;
            *out_id = category->value();
            return 1;
        }
        return 0;
    }

    // No value: try binary property, general category, script.
    auto prop = Unicode::property_from_string(name);
    if (prop.has_value()) {
        *out_kind = ResolvedPropertyKind::BinaryProperty;
        *out_id = prop->value();
        return 1;
    }
    auto category = Unicode::general_category_from_string(name);
    if (category.has_value()) {
        *out_kind = ResolvedPropertyKind::GeneralCategory;
        *out_id = category->value();
        return 1;
    }
    auto script = Unicode::script_from_string(name);
    if (script.has_value()) {
        *out_kind = ResolvedPropertyKind::Script;
        *out_id = script->value();
        return 1;
    }
    return 0;
}

/// Check if a code point matches a resolved property. Direct ICU trie lookup, no string parsing.
extern "C" int unicode_resolved_property_matches(uint32_t code_point, unsigned char kind, uint32_t id)
{
    switch (kind) {
    case ResolvedPropertyKind::Script:
        return Unicode::code_point_has_script(code_point, Unicode::Script { id }) ? 1 : 0;
    case ResolvedPropertyKind::ScriptExtension:
        return Unicode::code_point_has_script_extension(code_point, Unicode::Script { id }) ? 1 : 0;
    case ResolvedPropertyKind::GeneralCategory:
        return Unicode::code_point_has_general_category(code_point, Unicode::GeneralCategory { id }) ? 1 : 0;
    case ResolvedPropertyKind::BinaryProperty:
        return Unicode::code_point_has_property(code_point, Unicode::Property { id }) ? 1 : 0;
    default:
        return 0;
    }
}

extern "C" uint32_t unicode_simple_case_fold(uint32_t code_point, int unicode_mode)
{
    return Unicode::canonicalize(code_point, unicode_mode != 0);
}

extern "C" int unicode_code_point_matches_range_ignoring_case(uint32_t code_point, uint32_t from, uint32_t to, int unicode_mode)
{
    return Unicode::code_point_matches_range_ignoring_case(code_point, from, to, unicode_mode != 0) ? 1 : 0;
}

// Check if a code point matches a Unicode property, considering case closure.
// Returns 1 if the code point or any of its case equivalents has the property.
extern "C" int unicode_property_matches_case_insensitive(
    uint32_t code_point,
    unsigned char const* name_ptr, size_t name_len,
    unsigned char const* value_ptr, size_t value_len,
    int has_value)
{
    // First check the code point itself.
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto value = has_value
        ? StringView { reinterpret_cast<char const*>(value_ptr), value_len }
        : StringView {};

    auto check_property = [&](u32 cp) -> bool {
        if (has_value) {
            if (name.is_one_of("sc"sv, "Script"sv, "scx"sv, "Script_Extensions"sv)) {
                auto script = Unicode::script_from_string(value);
                if (!script.has_value())
                    return false;
                if (name.is_one_of("sc"sv, "Script"sv))
                    return Unicode::code_point_has_script(cp, *script);
                return Unicode::code_point_has_script_extension(cp, *script);
            }
            if (name.is_one_of("gc"sv, "General_Category"sv)) {
                auto category = Unicode::general_category_from_string(value);
                if (!category.has_value())
                    return false;
                return Unicode::code_point_has_general_category(cp, *category);
            }
            return false;
        }
        auto prop = Unicode::property_from_string(name);
        if (prop.has_value())
            return Unicode::code_point_has_property(cp, *prop);
        auto category = Unicode::general_category_from_string(name);
        if (category.has_value())
            return Unicode::code_point_has_general_category(cp, *category);
        auto script = Unicode::script_from_string(name);
        if (script.has_value())
            return Unicode::code_point_has_script(cp, *script);
        return false;
    };

    // Check using case closure — all case-equivalent code points.
    bool found = false;
    Unicode::for_each_case_folded_code_point(code_point, [&](u32 cp) {
        if (check_property(cp)) {
            found = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return found ? 1 : 0;
}

// Check if ALL case-equivalents of a code point have the property.
// Returns 1 only if every case-equivalent has the property.
// Used for u-flag negated case-insensitive: \P{X}/iu matches if NOT all have X.
extern "C" int unicode_property_all_case_equivalents_match(
    uint32_t code_point,
    unsigned char const* name_ptr, size_t name_len,
    unsigned char const* value_ptr, size_t value_len,
    int has_value)
{
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto value = has_value
        ? StringView { reinterpret_cast<char const*>(value_ptr), value_len }
        : StringView {};

    auto check_property = [&](u32 cp) -> bool {
        if (has_value) {
            if (name.is_one_of("sc"sv, "Script"sv, "scx"sv, "Script_Extensions"sv)) {
                auto script = Unicode::script_from_string(value);
                if (!script.has_value())
                    return false;
                if (name.is_one_of("sc"sv, "Script"sv))
                    return Unicode::code_point_has_script(cp, *script);
                return Unicode::code_point_has_script_extension(cp, *script);
            }
            if (name.is_one_of("gc"sv, "General_Category"sv)) {
                auto category = Unicode::general_category_from_string(value);
                if (!category.has_value())
                    return false;
                return Unicode::code_point_has_general_category(cp, *category);
            }
            return false;
        }
        auto prop = Unicode::property_from_string(name);
        if (prop.has_value())
            return Unicode::code_point_has_property(cp, *prop);
        auto category = Unicode::general_category_from_string(name);
        if (category.has_value())
            return Unicode::code_point_has_general_category(cp, *category);
        auto script = Unicode::script_from_string(name);
        if (script.has_value())
            return Unicode::code_point_has_script(cp, *script);
        return false;
    };

    bool all_match = true;
    Unicode::for_each_case_folded_code_point(code_point, [&](u32 cp) {
        if (!check_property(cp)) {
            all_match = false;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return all_match ? 1 : 0;
}

extern "C" unsigned int unicode_get_case_closure(
    uint32_t code_point,
    uint32_t* out_buffer,
    unsigned int buffer_capacity)
{
    unsigned int count = 0;
    Unicode::for_each_case_folded_code_point(code_point, [&](u32 cp) {
        if (count < buffer_capacity) {
            out_buffer[count] = cp;
            count++;
        }
        return IterationDecision::Continue;
    });
    return count;
}

extern "C" int unicode_is_string_property(
    unsigned char const* name_ptr, size_t name_len)
{
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto prop = Unicode::property_from_string(name);
    if (!prop.has_value())
        return 0;
    return Unicode::is_ecma262_string_property(*prop) ? 1 : 0;
}

extern "C" int unicode_is_valid_ecma262_property(
    unsigned char const* name_ptr, size_t name_len,
    unsigned char const* value_ptr, size_t value_len,
    int has_value)
{
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto value = has_value
        ? StringView { reinterpret_cast<char const*>(value_ptr), value_len }
        : StringView {};

    if (has_value) {
        // Key=Value form: Script, Script_Extensions, General_Category
        if (name.is_one_of("sc"sv, "Script"sv, "scx"sv, "Script_Extensions"sv))
            return Unicode::script_from_string(value).has_value() ? 1 : 0;
        if (name.is_one_of("gc"sv, "General_Category"sv))
            return Unicode::general_category_from_string(value).has_value() ? 1 : 0;
        return 0;
    }

    // Lone name: try as ECMA-262 binary property or General_Category value.
    // Note: Script names (e.g. "Hiragana") are NOT valid as lone names per
    // ECMA-262 -- they require Script= or sc= prefix.
    auto prop = Unicode::property_from_string(name);
    if (prop.has_value())
        return (Unicode::is_ecma262_property(*prop) || Unicode::is_ecma262_string_property(*prop)) ? 1 : 0;
    if (Unicode::general_category_from_string(name).has_value())
        return 1;
    return 0;
}

/// Get all multi-codepoint strings for a Unicode string property.
/// Writes packed data: [string_count, len1, cp1_0, cp1_1, ..., len2, cp2_0, ...]
/// Returns total number of u32 values written.
/// If out is null, returns the total size needed.
extern "C" uint32_t unicode_get_string_property_data(
    unsigned char const* name_ptr, size_t name_len,
    uint32_t* out, uint32_t capacity)
{
    auto name = StringView { reinterpret_cast<char const*>(name_ptr), name_len };
    auto prop = Unicode::property_from_string(name);
    if (!prop.has_value() || !Unicode::is_ecma262_string_property(*prop))
        return 0;

    auto strings = Unicode::get_property_strings(*prop);

    // Filter to multi-codepoint strings and compute total size needed.
    // Format: [count, len1, cp1..., len2, cp2..., ...]
    Vector<Vector<u32>> multi_cp_strings;
    for (auto const& str : strings) {
        Vector<u32> code_points;
        for (auto cp : str.code_points())
            code_points.append(cp);
        if (code_points.size() > 1)
            multi_cp_strings.append(move(code_points));
    }

    // Calculate total size: 1 (count) + sum(1 + len) for each string
    uint32_t total_size = 1;
    for (auto const& cps : multi_cp_strings)
        total_size += 1 + static_cast<uint32_t>(cps.size());

    if (!out || capacity < total_size)
        return total_size;

    uint32_t offset = 0;
    out[offset++] = static_cast<uint32_t>(multi_cp_strings.size());
    for (auto const& cps : multi_cp_strings) {
        out[offset++] = static_cast<uint32_t>(cps.size());
        for (auto cp : cps)
            out[offset++] = cp;
    }

    return total_size;
}

extern "C" int unicode_is_id_start(uint32_t code_point)
{
    return Unicode::code_point_has_identifier_start_property(code_point) ? 1 : 0;
}

extern "C" int unicode_is_id_continue(uint32_t code_point)
{
    return Unicode::code_point_has_identifier_continue_property(code_point) ? 1 : 0;
}

namespace regex {

ErrorOr<CompiledRustRegex, String> CompiledRustRegex::compile(StringView pattern, RustRegexFlags flags)
{
    unsigned char const* error_ptr = nullptr;
    size_t error_len = 0;

    auto* regex = rust_regex_compile(
        reinterpret_cast<unsigned char const*>(pattern.characters_without_null_termination()),
        pattern.length(),
        flags,
        &error_ptr,
        &error_len);
    if (!regex) {
        String error_message = "Invalid pattern"_string;
        if (error_ptr) {
            error_message = MUST(String::from_utf8({ reinterpret_cast<char const*>(error_ptr), error_len }));
            rust_regex_free_error(const_cast<unsigned char*>(error_ptr), error_len);
        }
        return error_message;
    }

    CompiledRustRegex result(regex);

    unsigned int group_count = 0;
    auto* groups = rust_regex_get_named_groups(regex, &group_count);
    if (groups) {
        result.m_named_groups.ensure_capacity(group_count);
        for (unsigned int i = 0; i < group_count; ++i) {
            auto name = String::from_utf8({ reinterpret_cast<char const*>(groups[i].name), groups[i].name_len });
            result.m_named_groups.append(RustNamedCaptureGroup { MUST(name), groups[i].index });
        }
        rust_regex_free_named_groups(groups, group_count);
    }

    return result;
}

CompiledRustRegex::~CompiledRustRegex()
{
    if (m_regex)
        rust_regex_free(m_regex);
}

CompiledRustRegex::CompiledRustRegex(CompiledRustRegex&& other)
    : m_regex(other.m_regex)
    , m_named_groups(move(other.m_named_groups))
    , m_capture_buffer(move(other.m_capture_buffer))
    , m_capture_count(other.m_capture_count)
    , m_capture_count_cached(other.m_capture_count_cached)
    , m_find_all_buffer(move(other.m_find_all_buffer))
{
    other.m_regex = nullptr;
    other.m_capture_count = 0;
    other.m_capture_count_cached = false;
}

CompiledRustRegex& CompiledRustRegex::operator=(CompiledRustRegex&& other)
{
    if (this != &other) {
        if (m_regex)
            rust_regex_free(m_regex);
        m_regex = other.m_regex;
        m_named_groups = move(other.m_named_groups);
        m_capture_buffer = move(other.m_capture_buffer);
        m_capture_count = other.m_capture_count;
        m_capture_count_cached = other.m_capture_count_cached;
        m_find_all_buffer = move(other.m_find_all_buffer);
        other.m_regex = nullptr;
        other.m_capture_count = 0;
        other.m_capture_count_cached = false;
    }
    return *this;
}

CompiledRustRegex::CompiledRustRegex(RustRegex* regex)
    : m_regex(regex)
{
}

int CompiledRustRegex::exec_internal(Utf16View input, size_t start_pos) const
{
    if (!m_capture_count_cached) {
        m_capture_count = rust_regex_capture_count(m_regex) + 1;
        m_capture_count_cached = true;
    }
    auto slots = m_capture_count * 2;
    m_capture_buffer.resize(slots);

    if (input.has_ascii_storage()) {
        auto ascii = input.ascii_span();
        return rust_regex_exec_into_ascii(
            m_regex,
            reinterpret_cast<uint8_t const*>(ascii.data()),
            ascii.size(),
            start_pos,
            m_capture_buffer.data(),
            slots);
    }

    auto utf16 = input.utf16_span();
    return rust_regex_exec_into(
        m_regex,
        reinterpret_cast<unsigned short const*>(utf16.data()),
        utf16.size(),
        start_pos,
        m_capture_buffer.data(),
        slots);
}

unsigned int CompiledRustRegex::total_groups() const
{
    if (!m_capture_count_cached) {
        m_capture_count = rust_regex_capture_count(m_regex) + 1;
        m_capture_count_cached = true;
    }
    return m_capture_count;
}

bool CompiledRustRegex::is_single_non_bmp_literal() const
{
    return rust_regex_is_single_non_bmp_literal(m_regex);
}

int CompiledRustRegex::test(Utf16View input, size_t start_pos) const
{
    if (input.has_ascii_storage()) {
        auto ascii = input.ascii_span();
        return rust_regex_test_ascii(
            m_regex,
            reinterpret_cast<uint8_t const*>(ascii.data()),
            ascii.size(),
            start_pos);
    }

    auto utf16 = input.utf16_span();
    return rust_regex_test(
        m_regex,
        reinterpret_cast<unsigned short const*>(utf16.data()),
        utf16.size(),
        start_pos);
}

int CompiledRustRegex::find_all(Utf16View input, size_t start_pos) const
{
    // Start with reasonable capacity; keep doubling until it fits.
    if (m_find_all_buffer.size() < 256)
        m_find_all_buffer.resize(256);

    for (;;) {
        int result;
        if (input.has_ascii_storage()) {
            auto ascii = input.ascii_span();
            result = rust_regex_find_all_ascii(
                m_regex,
                reinterpret_cast<uint8_t const*>(ascii.data()),
                ascii.size(),
                start_pos,
                m_find_all_buffer.data(),
                m_find_all_buffer.size());
        } else {
            auto utf16 = input.utf16_span();
            result = rust_regex_find_all(
                m_regex,
                reinterpret_cast<unsigned short const*>(utf16.data()),
                utf16.size(),
                start_pos,
                m_find_all_buffer.data(),
                m_find_all_buffer.size());
        }
        if (result != -1)
            return result;
        m_find_all_buffer.resize(m_find_all_buffer.size() * 2);
    }
}

unsigned int CompiledRustRegex::capture_count() const
{
    return rust_regex_capture_count(m_regex);
}

} // namespace regex
