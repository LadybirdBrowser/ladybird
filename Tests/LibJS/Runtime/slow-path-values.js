test("slow paths return values without overwriting destinations on exceptions", () => {
    const number = { valueOf: () => 7 };
    expect(number + 3).toBe(10);
    expect(number * 3).toBe(21);
    expect(number < 8).toBeTrue();
    expect(number | 8).toBe(15);

    const thrown = {};
    let destination = 42;
    try {
        destination =
            {
                valueOf() {
                    throw thrown;
                },
            } + 1;
    } catch (error) {
        expect(error).toBe(thrown);
    }
    expect(destination).toBe(42);
});

test("postfix slow paths return both the old and updated values", () => {
    let value = { valueOf: () => 12345678901234567890n };
    expect(value++).toBe(12345678901234567890n);
    expect(value).toBe(12345678901234567891n);
    expect(value--).toBe(12345678901234567891n);
    expect(value).toBe(12345678901234567890n);
    value = value++;
    expect(value).toBe(12345678901234567890n);
});

test("cache-only helpers distinguish returned values from cache misses", () => {
    function read(object) {
        return object.value;
    }
    let getterCalls = 0;
    const cases = [
        [{ a: 1, value: false }, false],
        [{ b: 1, value: 0 }, 0],
        [{ c: 1, value: "" }, ""],
        [{ d: 1, value: undefined }, undefined],
        [{ e: 1, value: null }, null],
        [{ missing: 1 }, undefined],
        [
            {
                get value() {
                    ++getterCalls;
                    return "getter";
                },
            },
            "getter",
        ],
    ];
    for (let iteration = 0; iteration < 10; ++iteration)
        for (const [object, expected] of cases) expect(read(object)).toBe(expected);
    expect(getterCalls).toBe(10);
});

test("variable-length slow-path inputs survive nested calls and collection", () => {
    const call = new Function("callee", "value", `return callee(${Array(600).fill("value").join(",")})`);
    const value = {};
    function callee() {
        gc();
        expect(arguments.length).toBe(600);
        for (const argument of arguments) expect(argument).toBe(value);
        return value;
    }
    const bound = callee.bind(null);
    for (let iteration = 0; iteration < 3; ++iteration) expect(call(bound, value)).toBe(value);

    const throwing = function () {
        throw value;
    }.bind(null);
    expect(() => call(throwing, value)).toThrow(value);
    expect(call(bound, value)).toBe(value);
});

test("iterator slow paths preserve done state when extracting a value throws", () => {
    const thrown = {};
    let closed = false;
    const iterable = {
        [Symbol.iterator]() {
            return {
                next() {
                    return {
                        done: false,
                        get value() {
                            throw thrown;
                        },
                    };
                },
                return() {
                    closed = true;
                    return {};
                },
            };
        },
    };
    expect(() => {
        const [value] = iterable;
    }).toThrow(thrown);
    expect(closed).toBeFalse();
});

test("exhausted iterator outputs remain safe across collection", () => {
    function* values() {
        yield 42;
    }
    const iterator = values();
    for (let iteration = 0; iteration < 3; ++iteration) {
        const [first, second = "default"] = iterator;
        gc();
        expect(first).toBe(iteration === 0 ? 42 : undefined);
        expect(second).toBe("default");
    }
});
