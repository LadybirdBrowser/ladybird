test("return from try followed by finally with function call inside", () => {
    let value = (() => {
        try {
            return 1;
        } finally {
            (() => {})();
        }
    })();

    expect(value).toBe(1);
});

test("return from outer finally with nested unwind contexts", () => {
    let value = (() => {
        try {
            try {
                return 1;
            } finally {
            }
        } finally {
            return 2;
        }
    })();

    expect(value).toBe(2);
});

test("restore exception after generator yield in finally", () => {
    let generator = (function* () {
        try {
            throw new Error("foo");
        } finally {
            yield 42;
        }
    })();

    expect(generator.next().value).toBe(42);
    expect(() => generator.next()).toThrowWithMessage(Error, "foo");
    expect(generator.next().done).toBe(true);
});

test("yield, then return from finally", () => {
    let test = [];
    let generator = (function* () {
        try {
            yield 1;
            test.push(1);
        } finally {
            test.push(2);
            return 2;
        }
        expect.fail("unreachable");
    })();

    let result = generator.next();
    expect(result.value).toBe(1);
    expect(result.done).toBe(false);
    result = generator.next();
    expect(result.value).toBe(2);
    expect(result.done).toBe(true);
    expect(test).toEqual([1, 2]);
});

test("return from async through finally", () => {
    let test = 0;
    let result = (async function () {
        try {
            return { y: 5 };
        } finally {
            test = 42;
        }
        expect.fail("unreachable");
    })();

    expect(result).toBeInstanceOf(Promise);
    expect(test).toBe(42);
    result.then(value => {
        expect(value).toEqual({ y: 5 });
    });
});
