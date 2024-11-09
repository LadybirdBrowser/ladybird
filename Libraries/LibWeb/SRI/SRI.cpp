/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Base64.h>
#include <AK/Vector.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWeb/SRI/SRI.h>

namespace Web::SRI {

constexpr Array supported_hash_functions {
    // These are sorted by strength, low to high.
    // NOTE: We are specifically told to refuse MD5 and SHA1.
    //       https://w3c.github.io/webappsec-subresource-integrity/#hash-functions
    "sha256"sv,
    "sha384"sv,
    "sha512"sv,
};

// https://w3c.github.io/webappsec-subresource-integrity/#getprioritizedhashfunction
static StringView get_prioritized_hash_function(StringView a, StringView b)
{
    if (a == b)
        return ""sv;

    auto a_priority = supported_hash_functions.first_index_of(a).value();
    auto b_priority = supported_hash_functions.first_index_of(b).value();
    if (a_priority > b_priority)
        return a;
    return b;
}

// https://w3c.github.io/webappsec-subresource-integrity/#apply-algorithm-to-response
ErrorOr<String> apply_algorithm_to_bytes(StringView algorithm, ByteBuffer const& bytes)
{
    // NOTE: The steps are duplicated here because each hash algorithm returns a different result type.

    if (algorithm == "sha256"sv) {
        // 1. Let result be the result of applying algorithm to bytes.
        auto result = Crypto::Hash::SHA256::hash(bytes);

        // 2. Return the result of base64 encoding result.
        return encode_base64(result.bytes());
    }

    if (algorithm == "sha384"sv) {
        // 1. Let result be the result of applying algorithm to bytes.
        auto result = Crypto::Hash::SHA384::hash(bytes);

        // 2. Return the result of base64 encoding result.
        return encode_base64(result.bytes());
    }

    if (algorithm == "sha512"sv) {
        // 1. Let result be the result of applying algorithm to bytes.
        auto result = Crypto::Hash::SHA512::hash(bytes);

        // 2. Return the result of base64 encoding result.
        return encode_base64(result.bytes());
    }

    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webappsec-subresource-integrity/#parse-metadata
ErrorOr<Vector<Metadata>> parse_metadata(StringView metadata)
{
    // 1. Let result be the empty set.
    Vector<Metadata> result;

    // 2. For each item returned by splitting metadata on spaces:
    TRY(metadata.for_each_split_view(' ', SplitBehavior::Nothing, [&](StringView item) -> ErrorOr<void> {
        // 1. Let hash-with-opt-token-list be the result of splitting item on U+003F (?).
        auto hash_with_opt_token_list = item.split_view('?');

        // 2. Let hash-expression be hash-with-opt-token-list[0].
        auto hash_expression = hash_with_opt_token_list[0];

        // 3. Let base64-value be the empty string.
        StringView base64_value;

        // 4. Let hash-expr-token-list be the result of splitting hash-expression on U+002D (-).
        auto hash_expr_token_list = hash_expression.split_view('-');

        // 5. Let algorithm be hash-expr-token-list[0].
        auto algorithm = hash_expr_token_list[0];

        // 6. If hash-expr-token-list[1] exists, set base64-value to hash-expr-token-list[1].
        if (hash_expr_token_list.size() >= 1)
            base64_value = hash_expr_token_list[1];

        // 7. If algorithm is not a hash function recognized by the user agent, continue.
        if (!supported_hash_functions.contains_slow(algorithm))
            return {};

        // 8. Let metadata be the ordered map «["alg" → algorithm, "val" → base64-value]».
        //    Note: Since no options are defined (see the §3.1 Integrity metadata), a corresponding entry is not set in metadata.
        //    If options are defined in a future version, hash-with-opt-token-list[1] can be utilized as options.
        auto metadata = Metadata {
            .algorithm = TRY(String::from_utf8(algorithm)),
            .base64_value = TRY(String::from_utf8(base64_value)),
            .options = {},
        };

        // 9. Append metadata to result.
        TRY(result.try_append(move(metadata)));

        return {};
    }));

    // 3. Return result.
    return result;
}

// https://w3c.github.io/webappsec-subresource-integrity/#get-the-strongest-metadata
ErrorOr<Vector<Metadata>> get_strongest_metadata_from_set(Vector<Metadata> const& set)
{
    // 1. Let result be the empty set and strongest be the empty string.
    Vector<Metadata> result;
    Optional<Metadata> strongest;

    // 2. For each item in set:
    for (auto const& item : set) {
        // 1. If result is the empty set, add item to result and set strongest to item, skip to the next item.
        if (result.is_empty()) {
            TRY(result.try_append(item));
            strongest = item;
            continue;
        }

        // 2. Let currentAlgorithm be the alg component of strongest.
        auto& current_algorithm = strongest->algorithm;

        // 3. Let newAlgorithm be the alg component of item.
        auto& new_algorithm = item.algorithm;

        // 4. If the result of getPrioritizedHashFunction(currentAlgorithm, newAlgorithm) is the empty string, add item to result.
        auto prioritized_hash_function = get_prioritized_hash_function(current_algorithm, new_algorithm);
        if (prioritized_hash_function.is_empty()) {
            TRY(result.try_append(item));
        }
        //    If the result is newAlgorithm, set strongest to item, set result to the empty set, and add item to result.
        else if (prioritized_hash_function == new_algorithm) {
            strongest = item;
            result.clear_with_capacity();
            TRY(result.try_append(item));
        }
    }

    // 3. Return result.
    return result;
}

// https://w3c.github.io/webappsec-subresource-integrity/#does-response-match-metadatalist
ErrorOr<bool> do_bytes_match_metadata_list(ByteBuffer const& bytes, StringView metadata_list)
{
    // 1. Let parsedMetadata be the result of parsing metadataList.
    auto parsed_metadata = TRY(parse_metadata(metadata_list));

    // 2. If parsedMetadata is empty set, return true.
    if (parsed_metadata.is_empty())
        return true;

    // 3. Let metadata be the result of getting the strongest metadata from parsedMetadata.
    auto metadata = TRY(get_strongest_metadata_from_set(parsed_metadata));

    // 4. For each item in metadata:
    for (auto const& item : metadata) {
        // 1. Let algorithm be the item["alg"].
        auto& algorithm = item.algorithm;

        // 2. Let expectedValue be the item["val"].
        auto& expected_value = item.base64_value;

        // 3. Let actualValue be the result of applying algorithm to bytes.
        auto actual_value = TRY(apply_algorithm_to_bytes(algorithm, bytes));

        // 4. If actualValue is a case-sensitive match for expectedValue, return true.
        if (actual_value == expected_value)
            return true;
    }

    // 5. Return false.
    return false;
}

}
