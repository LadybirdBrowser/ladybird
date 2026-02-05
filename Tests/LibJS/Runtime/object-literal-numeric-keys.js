/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

test("object literal with numeric string key '0'", () => {
    function a() {
        return { 0: "value1", a: "value2" };
    }
    const obj1 = a();
    const obj2 = a();

    expect(obj1["0"]).toBe("value1");
    expect(obj1.a).toBe("value2");
    expect(obj2["0"]).toBe("value1");
    expect(obj2.a).toBe("value2");
});

test("object literal with various numeric string keys", () => {
    function b() {
        return { 1: "one", 2: "two", x: "x" };
    }
    const obj1 = b();
    const obj2 = b();

    expect(obj1["1"]).toBe("one");
    expect(obj1["2"]).toBe("two");
    expect(obj1.x).toBe("x");
    expect(obj2["1"]).toBe("one");
    expect(obj2["2"]).toBe("two");
    expect(obj2.x).toBe("x");
});

test("object literal with numeric key at boundary", () => {
    function c() {
        return { 4294967294: "max-1", a: "val" };
    }
    const obj1 = c();
    const obj2 = c();

    expect(obj1["4294967294"]).toBe("max-1");
    expect(obj1.a).toBe("val");
    expect(obj2["4294967294"]).toBe("max-1");
    expect(obj2.a).toBe("val");
});

test("object literal with leading zeros (not numeric index)", () => {
    // Leading zeros like "00" are NOT numeric indices, so these can use fast path
    function d() {
        return { "00": "zero-zero", "01": "zero-one" };
    }
    const obj1 = d();
    const obj2 = d();

    expect(obj1["00"]).toBe("zero-zero");
    expect(obj1["01"]).toBe("zero-one");
    expect(obj2["00"]).toBe("zero-zero");
    expect(obj2["01"]).toBe("zero-one");
});

test("object literal without numeric keys uses fast path", () => {
    // This should use the fast path since there are no numeric keys
    function e() {
        return { a: 1, b: 2, c: 3 };
    }
    const obj1 = e();
    const obj2 = e();

    expect(obj1.a).toBe(1);
    expect(obj1.b).toBe(2);
    expect(obj1.c).toBe(3);
    expect(obj2.a).toBe(1);
    expect(obj2.b).toBe(2);
    expect(obj2.c).toBe(3);
});
