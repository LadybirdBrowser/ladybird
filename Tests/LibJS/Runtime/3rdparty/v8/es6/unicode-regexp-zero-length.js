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

test("es6/unicode-regexp-zero-length", () => {
    var L = "\ud800";
    var T = "\udc00";
    var x = "x";

    var r = /()/g; // Global, but not unicode.
    // Zero-length matches do not advance lastIndex.
    assertEquals(["", ""], r.exec(L + T + L + T));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 1;
    assertEquals(["", ""], r.exec(L + T + L + T));
    assertEquals(1, r.lastIndex);

    var u = /()/gu; // Global and unicode.
    // Zero-length matches do not advance lastIndex.
    assertEquals(["", ""], u.exec(L + T + L + T));
    assertEquals(0, u.lastIndex);
    u.lastIndex = 1;
    assertEquals(["", ""], u.exec(L + T + L + T));
    assertEquals(0, u.lastIndex);

    // However, with repeating matches, lastIndex does not matter.
    // We do advance from match to match.
    r.lastIndex = 2;
    assertEquals(x + L + x + T + x + L + x + T + x, (L + T + L + T).replace(r, "x"));

    // With unicode flag, we advance code point by code point.
    u.lastIndex = 3;
    assertEquals(x + L + T + x + L + T + x, (L + T + L + T).replace(u, "x"));

    // Test that exhausting the global match cache is fine.
    assertEquals((x + L + T).repeat(1000) + x, (L + T).repeat(1000).replace(u, "x"));

    // Same thing for RegExp.prototype.match.
    r.lastIndex = 1;
    assertEquals(["", "", "", "", ""], (L + T + L + T).match(r));
    r.lastIndex = 2;
    assertEquals(["", "", "", "", ""], (L + T + L + T).match(r));

    u.lastIndex = 1;
    assertEquals(["", "", ""], (L + T + L + T).match(u));
    u.lastIndex = 2;
    assertEquals(["", "", ""], (L + T + L + T).match(u));

    var expected = [];
    for (var i = 0; i <= 1000; i++) expected.push("");
    assertEquals(expected, (L + T).repeat(1000).match(u));

    // Also test RegExp.prototype.@@split.
    assertEquals(["\u{12345}"], "\u{12345}".split(/(?:)/u));
});
