/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

test("closure-capturing callees do not drain promise jobs before the caller resumes", () => {
    let order = [];

    function inner() {
        let x = 1;
        order.push("inner");
        return () => x;
    }

    inner();
    order.length = 0;

    function outer() {
        inner();
        order.push("outer");
    }

    Promise.resolve().then(() => order.push("micro"));

    outer();
    expect(order).toEqual(["inner", "outer", "micro"]);
});

test("sloppy primitive receivers do not drain promise jobs before the caller resumes", () => {
    let order = [];

    Number.prototype.__asmInlineCallMicrotaskOrder = function () {
        this;
        order.push("inner");
    };

    (1).__asmInlineCallMicrotaskOrder();
    order.length = 0;

    function outer() {
        (1).__asmInlineCallMicrotaskOrder();
        order.push("outer");
    }

    Promise.resolve().then(() => order.push("micro"));

    outer();
    expect(order).toEqual(["inner", "outer", "micro"]);
});

test("sloppy undefined receivers still bind the global object in inline calls", () => {
    function inner() {
        return this;
    }

    inner();

    function outer() {
        return inner();
    }

    expect(outer()).toBe(globalThis);
});
