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

test("es6/unicode-regexp-last-index", () => {
    var r = /./gu;
    assertEquals(["\ud800\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    r.lastIndex = 1;
    assertEquals(["\ud800\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    assertEquals(["\ud801\udc01"], r.exec("\ud800\udc00\ud801\udc01"));
    r.lastIndex = 3;
    assertEquals(["\ud801\udc01"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(4, r.lastIndex);
    r.lastIndex = 4;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 5;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);

    r.lastIndex = 3;
    assertEquals(["\ud802"], r.exec("\ud800\udc00\ud801\ud802"));
    r.lastIndex = 4;
    assertNull(r.exec("\ud800\udc00\ud801\ud802"));

    r = /./g;
    assertEquals(["\ud800"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(1, r.lastIndex);
    assertEquals(["\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    assertEquals(["\ud801"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(3, r.lastIndex);
    assertEquals(["\udc01"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(4, r.lastIndex);
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 1;
    assertEquals(["\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);

    // ------------------------

    r = /^./gu;
    assertEquals(["\ud800\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    r.lastIndex = 1;
    assertEquals(["\ud800\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 3;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 4;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 5;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);

    r = /^./g;
    assertEquals(["\ud800"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(1, r.lastIndex);
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);
    r.lastIndex = 3;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(0, r.lastIndex);

    //------------------------

    r = /(?:(^.)|.)/gu;
    assertEquals(["\ud800\udc00", "\ud800\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    r.lastIndex = 1;
    assertEquals(["\ud800\udc00", "\ud800\udc00"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    assertEquals(["\ud801\udc01", undefined], r.exec("\ud800\udc00\ud801\udc01"));
    r.lastIndex = 3;
    assertEquals(["\ud801\udc01", undefined], r.exec("\ud800\udc00\ud801\udc01"));
    r.lastIndex = 4;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));
    r.lastIndex = 5;
    assertNull(r.exec("\ud800\udc00\ud801\udc01"));

    r.lastIndex = 3;
    assertEquals(["\ud802", undefined], r.exec("\ud800\udc00\ud801\ud802"));
    r.lastIndex = 4;
    assertNull(r.exec("\ud800\udc00\ud801\ud802"));

    r = /(?:(^.)|.)/g;
    assertEquals(["\ud800", "\ud800"], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(1, r.lastIndex);
    assertEquals(["\udc00", undefined], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(2, r.lastIndex);
    r.lastIndex = 3;
    assertEquals(["\udc01", undefined], r.exec("\ud800\udc00\ud801\udc01"));
    assertEquals(4, r.lastIndex);
});
