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

test.xfail("es6/regexp-tostring", () => {
    var log = [];

    var fake = {
        get source() {
            log.push("p");
            return {
                toString: function () {
                    log.push("ps");
                    return "pattern";
                },
            };
        },
        get flags() {
            log.push("f");
            return {
                toString: function () {
                    log.push("fs");
                    return "flags";
                },
            };
        },
    };

    function testThrows(x) {
        try {
            RegExp.prototype.toString.call(x);
        } catch (e) {
            assertTrue(/incompatible receiver/.test(e.message));
            return;
        }
        assertUnreachable();
    }

    testThrows(1);
    testThrows(null);
    Number.prototype.source = "a";
    Number.prototype.flags = "b";
    testThrows(1);

    assertEquals("/pattern/flags", RegExp.prototype.toString.call(fake));
    assertEquals(["p", "ps", "f", "fs"], log);

    // Monkey-patching is also possible on RegExp instances

    let weird = /foo/;
    Object.defineProperty(weird, "flags", { value: "bar" });
    Object.defineProperty(weird, "source", { value: "baz" });
    assertEquals("/baz/bar", weird.toString());

    assertEquals("/(?:)/", RegExp.prototype.toString());
    assertEquals("(?:)", RegExp.prototype.source);
    assertEquals("", RegExp.prototype.flags);
});
