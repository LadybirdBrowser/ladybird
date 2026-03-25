// Copyright 2011 the V8 project authors. All rights reserved.
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

test("harmony/regexp-property-char-class", () => {
    assertThrows("/[\\p]/u");
    assertThrows("/[\\p{garbage}]/u");
    assertThrows("/[\\p{}]/u");
    assertThrows("/[\\p{]/u");
    assertThrows("/[\\p}]/u");
    assertThrows("/^[\\p{Lu}-\\p{Ll}]+$/u");

    assertTrue(/^[\p{Lu}\p{Ll}]+$/u.test("ABCabc"));
    assertTrue(/^[\p{Lu}-]+$/u.test("ABC-"));
    assertFalse(/^[\P{Lu}\p{Ll}]+$/u.test("ABCabc"));
    assertTrue(/^[\P{Lu}\p{Ll}]+$/u.test("abc"));
    assertTrue(/^[\P{Lu}]+$/u.test("abc123"));
    assertFalse(/^[\P{Lu}]+$/u.test("XYZ"));
    assertTrue(/[\p{Math}]/u.test("+"));
    assertTrue(/[\P{Bidi_M}]/u.test(" "));
    assertTrue(/[\p{Hex}]/u.test("A"));

    assertTrue(/^[^\P{Lu}]+$/u.test("XYZ"));
    assertFalse(/^[^\p{Lu}\p{Ll}]+$/u.test("abc"));
    assertFalse(/^[^\p{Lu}\p{Ll}]+$/u.test("ABC"));
    assertTrue(/^[^\p{Lu}\p{Ll}]+$/u.test("123"));
    assertTrue(/^[^\p{Lu}\P{Ll}]+$/u.test("abc"));
});
