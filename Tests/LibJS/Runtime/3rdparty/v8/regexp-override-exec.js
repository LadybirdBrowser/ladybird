// Copyright 2018 the V8 project authors. All rights reserved.
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
            try {
                fn = new Function(fn);
            } catch (e) {
                return;
            }
        } catch (e) {
            return;
        }
    }
    if (typeof fn === "string") {
        try {
            try {
                fn = new Function(fn);
            } catch (e) {
                return;
            }
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

test("regexp-override-exec", () => {
    var s = "baa";

    assertEquals(1, s.search(/a/));
    assertEquals(["aa"], s.match(/a./));
    assertEquals(["b", "", ""], s.split(/a/));

    let o = { index: 3, 0: "x" };

    RegExp.prototype.exec = () => {
        return o;
    };
    assertEquals(3, s.search(/a/));
    assertEquals(o, s.match(/a./));
    assertEquals("baar", s.replace(/a./, "r"));

    RegExp.prototype.exec = () => {
        return null;
    };
    assertEquals(["baa"], s.split(/a/));
});
