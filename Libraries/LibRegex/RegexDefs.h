/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 * Copyright (c) 2020-2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

enum __Regex_Error {
    __Regex_NoError,
    __Regex_InvalidPattern,              // Invalid regular expression.
    __Regex_InvalidCollationElement,     // Invalid collating element referenced.
    __Regex_InvalidCharacterClass,       // Invalid character class type referenced.
    __Regex_InvalidTrailingEscape,       // Trailing \ in pattern.
    __Regex_InvalidNumber,               // Number in \digit invalid or in error.
    __Regex_MismatchingBracket,          // [ ] imbalance.
    __Regex_MismatchingParen,            // ( ) imbalance.
    __Regex_MismatchingBrace,            // { } imbalance.
    __Regex_InvalidBraceContent,         // Content of {} invalid: not a number, number too large, more than two numbers, first larger than second.
    __Regex_InvalidBracketContent,       // Content of [] invalid.
    __Regex_InvalidRange,                // Invalid endpoint in range expression.
    __Regex_InvalidRepetitionMarker,     // ?, * or + not preceded by valid regular expression.
    __Regex_ReachedMaxRecursion,         // MaximumRecursion has been reached.
    __Regex_EmptySubExpression,          // Sub expression has empty content.
    __Regex_InvalidCaptureGroup,         // Content of capture group is invalid.
    __Regex_InvalidNameForCaptureGroup,  // Name of capture group is invalid.
    __Regex_InvalidNameForProperty,      // Name of property is invalid.
    __Regex_DuplicateNamedCapture,       // Duplicate named capture group
    __Regex_InvalidCharacterClassEscape, // Invalid escaped entity in character class.
};

enum __RegexAllFlags {
    __Regex_Global = 1,                                          // All matches (don't return after first match)
    __Regex_Insensitive = __Regex_Global << 1,                   // Case insensitive match (ignores case of [a-zA-Z])
    __Regex_Ungreedy = __Regex_Global << 2,                      // The match becomes lazy by default. Now a ? following a quantifier makes it greedy
    __Regex_Unicode = __Regex_Global << 3,                       // Enable all unicode features and interpret all unicode escape sequences as such
    __Regex_Extended = __Regex_Global << 4,                      // Ignore whitespaces. Spaces and text after a # in the pattern are ignored
    __Regex_Extra = __Regex_Global << 5,                         // Disallow meaningless escapes. A \ followed by a letter with no special meaning is faulted
    __Regex_MatchNotBeginOfLine = __Regex_Global << 6,           // Pattern is not forced to ^ -> search in whole string!
    __Regex_MatchNotEndOfLine = __Regex_Global << 7,             // Don't Force the dollar sign, $, to always match end of the string, instead of end of the line. This option is ignored if the Multiline-flag is set
    __Regex_SkipSubExprResults = __Regex_Global << 8,            // Do not return sub expressions in the result
    __Regex_StringCopyMatches = __Regex_Global << 9,             // Do explicitly copy results into new allocated string instead of StringView to original string.
    __Regex_SingleLine = __Regex_Global << 10,                   // Dot matches newline characters
    __Regex_Sticky = __Regex_Global << 11,                       // Force the pattern to only match consecutive matches from where the previous match ended.
    __Regex_Multiline = __Regex_Global << 12,                    // Handle newline characters. Match each line, one by one.
    __Regex_SkipTrimEmptyMatches = __Regex_Global << 13,         // Do not remove empty capture group results.
    __Regex_SingleMatch = __Regex_Global << 14,                  // Stop after acquiring a single match.
    __Regex_UnicodeSets = __Regex_Global << 15,                  // ECMA262 Parser specific: Allow set operations in char classes.
    __Regex_Internal_Stateful = __Regex_Global << 16,            // Internal flag; enables stateful matches.
    __Regex_Internal_BrowserExtended = __Regex_Global << 17,     // Internal flag; enable browser-specific ECMA262 extensions.
    __Regex_Internal_ConsiderNewline = __Regex_Global << 18,     // Internal flag; allow matchers to consider newlines as line separators.
    __Regex_Internal_ECMA262DotSemantics = __Regex_Global << 19, // Internal flag; use ECMA262 semantics for dot ('.') - disallow CR/LF/LS/PS instead of just CR.
    __Regex_Last = __Regex_Internal_ECMA262DotSemantics,
};
