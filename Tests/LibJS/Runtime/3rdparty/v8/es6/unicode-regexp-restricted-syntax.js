// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// V8 assertion compatibility shim for Ladybird's test-js harness

function assertEquals(expected, actual, msg) {
    if (expected instanceof RegExp && actual instanceof RegExp) {
        expect(actual.source).toBe(expected.source);
        expect(actual.flags).toBe(expected.flags);
    } else if (Array.isArray(expected) && Array.isArray(actual)) {
        expect(actual).toEqual(expected);
    } else if (expected !== null && typeof expected === "object" && actual !== null && typeof actual === "object") {
        expect(actual).toEqual(expected);
    } else {
        expect(actual).toBe(expected);
    }
}

function assertTrue(val, msg) {
    expect(val).toBeTrue();
}

function assertFalse(val, msg) {
    expect(val).toBeFalse();
}

function assertNull(val, msg) {
    expect(val).toBeNull();
}

function assertNotNull(val, msg) {
    expect(val).not.toBeNull();
}

function assertThrows(fn, type_opt, msg_opt) {
    if (typeof fn === "string") {
        try {
            fn = new Function(fn);
        } catch (e) {
            return;
        }
    }
    expect(fn).toThrow();
}

function assertDoesNotThrow(fn, msg) {
    fn();
}

function assertInstanceof(val, type, msg) {
    expect(val instanceof type).toBeTrue();
}

function assertUnreachable(msg) {
    expect().fail("unreachable" + (msg ? ": " + msg : ""));
}

function assertEarlyError(code) {
    assertThrows(() => new Function(code));
}

function assertThrowsAtRuntime(code, type_opt) {
    const f = new Function(code);
    assertThrows(f, type_opt);
}

function assertArrayEquals(expected, actual) {
    expect(actual).toEqual(expected);
}

test("es6/unicode-regexp-restricted-syntax", () => {
    assertThrows("/\\1/u", SyntaxError);
    // test262/language/literals/regexp/u-invalid-char-range-a
    assertThrows("/[\\w-a]/u", SyntaxError);
    // test262/language/literals/regexp/u-invalid-char-range-b
    assertThrows("/[a-\\w]/u", SyntaxError);
    // test262/language/literals/regexp/u-invalid-char-esc
    assertThrows("/\\c/u", SyntaxError);
    assertThrows("/\\c0/u", SyntaxError);
    // test262/built-ins/RegExp/unicode_restricted_quantifiable_assertion
    assertThrows("/(?=.)*/u", SyntaxError);
    assertThrows("/(?=.){1,2}/u", SyntaxError);
    // test262/built-ins/RegExp/unicode_restricted_octal_escape
    assertThrows("/[\\1]/u", SyntaxError);
    assertThrows("/\\00/u", SyntaxError);
    assertThrows("/\\09/u", SyntaxError);
    // test262/built-ins/RegExp/unicode_restricted_identity_escape_alpha
    assertThrows("/[\\c]/u", SyntaxError);
    // test262/built-ins/RegExp/unicode_restricted_identity_escape_c
    assertThrows("/[\\c0]/u", SyntaxError);
    // test262/built-ins/RegExp/unicode_restricted_incomple_quantifier
    assertThrows("/a{/u", SyntaxError);
    assertThrows("/a{1,/u", SyntaxError);
    assertThrows("/{/u", SyntaxError);
    assertThrows("/}/u", SyntaxError);
    // test262/data/test/built-ins/RegExp/unicode_restricted_brackets
    assertThrows("/]/u", SyntaxError);
    // test262/built-ins/RegExp/unicode_identity_escape
    /\//u;

    // escaped \0 is allowed inside a character class.
    assertEquals(["\0"], /[\0]/u.exec("\0"));
    // unless it is followed by another digit.
    assertThrows("/[\\00]/u", SyntaxError);
    assertThrows("/[\\01]/u", SyntaxError);
    assertThrows("/[\\09]/u", SyntaxError);
    assertEquals(["\u{0}1\u{0}a\u{0}"], /[1\0a]+/u.exec("b\u{0}1\u{0}a\u{0}2"));
    // escaped \- is allowed inside a character class.
    assertEquals(["-"], /[a\-z]/u.exec("12-34"));
});
