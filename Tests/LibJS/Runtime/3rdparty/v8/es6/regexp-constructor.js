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

test("es6/regexp-constructor", () => {
    "use strict";

    function should_not_be_called() {
        throw new Error("should not be called");
    }

    (function () {
        var r = new RegExp("biep");
        assertTrue(r === RegExp(r));
        assertFalse(r === new RegExp(r));
        r[Symbol.match] = false;
        Object.defineProperty(r, "source", { get: should_not_be_called });
        Object.defineProperty(r, "flags", { get: should_not_be_called });
        assertFalse(r === RegExp(r));
    })();

    (function () {
        let allow = false;
        class A extends RegExp {
            get source() {
                if (!allow) throw new Error("should not be called");
                return super.source;
            }
            get flags() {
                if (!allow) throw new Error("should not be called");
                return super.flags;
            }
        }

        var r = new A("biep");
        var r2 = RegExp(r);

        assertFalse(r === r2);
        allow = true;
        assertEquals(r, r2);
        allow = false;
        assertTrue(A.prototype === r.__proto__);
        assertTrue(RegExp.prototype === r2.__proto__);

        var r3 = RegExp(r);
        assertFalse(r3 === r);
        allow = true;
        assertEquals(r3, r);
        allow = false;

        var r4 = new A(r2);
        assertFalse(r4 === r2);
        allow = true;
        assertEquals(r4, r2);
        allow = false;
        assertTrue(A.prototype === r4.__proto__);

        r[Symbol.match] = false;
        var r5 = new A(r);
        assertFalse(r5 === r);
        allow = true;
        assertEquals(r5, r);
        allow = false;
        assertTrue(A.prototype === r5.__proto__);
    })();

    (function () {
        var log = [];
        var match = {
            get source() {
                log.push("source");
                return "biep";
            },
            get flags() {
                log.push("flags");
                return "i";
            },
        };
        Object.defineProperty(match, Symbol.match, {
            get() {
                log.push("match");
                return true;
            },
        });
        var r = RegExp(match);
        assertEquals(["match", "source", "flags"], log);
        assertFalse(r === match);
        assertEquals(/biep/i, r);
    })();

    (function () {
        var log = [];
        var match = {
            get source() {
                log.push("source");
                return "biep";
            },
            get flags() {
                log.push("flags");
                return "i";
            },
        };
        Object.defineProperty(match, Symbol.match, {
            get() {
                log.push("match");
                return true;
            },
        });
        match.constructor = RegExp;
        var r = RegExp(match);
        assertEquals(["match"], log);
        assertTrue(r === match);
    })();

    (function () {
        var r = RegExp("biep", "i");
        r[Symbol.match] = false;
        var r2 = RegExp(r, "g");
        assertFalse(r === r2);
        assertEquals(/biep/i, r);
        assertEquals(/biep/g, r2);
    })();

    (function () {
        class A extends RegExp {
            get ["constructor"]() {
                log.push("constructor");
                return RegExp;
            }
        }
        var r = new A("biep");
        var log = [];
        var r2 = RegExp(r);
        assertEquals(["constructor"], log);
        assertTrue(r === r2);
    })();
});
