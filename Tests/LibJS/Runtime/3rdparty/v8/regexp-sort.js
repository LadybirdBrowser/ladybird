// Copyright 2015 the V8 project authors. All rights reserved.
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

test("regexp-sort", () => {
    function Test(lower, upper) {
        var lx = lower + "x";
        var ux = upper + "x";
        var lp = lower + "|";
        var uxp = upper + "x|";
        assertEquals(lx, new RegExp(uxp + lp + lower + "cat", "i").exec(lx) + "");
        assertEquals(ux, new RegExp(uxp + lp + lower + "cat", "i").exec(ux) + "");
        assertEquals(lower, new RegExp(lp + uxp + lower + "cat", "i").exec(lx) + "");
        assertEquals(upper, new RegExp(lp + uxp + lower + "cat", "i").exec(ux) + "");
    }

    function TestFail(lower, upper) {
        var lx = lower + "x";
        var ux = upper + "x";
        var lp = lower + "|";
        var uxp = upper + "x|";
        assertEquals(lower, new RegExp(uxp + lp + lower + "cat", "i").exec(lx) + "");
        assertEquals(ux, new RegExp(uxp + lp + lower + "cat", "i").exec(ux) + "");
        assertEquals(lower, new RegExp(lp + uxp + lower + "cat", "i").exec(lx) + "");
        assertEquals(ux, new RegExp(lp + uxp + lower + "cat", "i").exec(ux) + "");
    }

    Test("a", "A");
    Test("0", "0");
    TestFail("a", "b");
    // Small and capital o-umlaut
    Test(String.fromCharCode(0xf6), String.fromCharCode(0xd6));
    // Small and capital kha.
    Test(String.fromCharCode(0x445), String.fromCharCode(0x425));
    // Small and capital y-umlaut.
    Test(String.fromCharCode(0xff), String.fromCharCode(0x178));
    // Small and large Greek mu.
    Test(String.fromCharCode(0x3bc), String.fromCharCode(0x39c));
    // Micron and large Greek mu.
    Test(String.fromCharCode(0xb5), String.fromCharCode(0x39c));
    // Micron and small Greek mu.
    Test(String.fromCharCode(0xb5), String.fromCharCode(0x3bc));
    // German double s and capital S. These are not equivalent since one is double.
    TestFail(String.fromCharCode(0xdf), "S");
    // Small i and Turkish capital dotted I. These are not equivalent due to
    // 21.2.2.8.2 section 3g.  One is below 128 and the other is above 127.
    TestFail("i", String.fromCharCode(0x130));
    // Small dotless i and I. These are not equivalent either.
    TestFail(String.fromCharCode(0x131), "I");
});
