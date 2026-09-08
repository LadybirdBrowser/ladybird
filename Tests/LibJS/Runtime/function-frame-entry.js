test("function entry initializes reused locals and constants", () => {
    function callee(initialize) {
        if (!initialize) return local;
        let local = "initialized";
        return [local, 12345678901234567890n, 1.25];
    }

    for (let iteration = 0; iteration < 3; ++iteration) {
        expect(callee(true)).toEqual(["initialized", 12345678901234567890n, 1.25]);
        let threw = false;
        try {
            callee(false);
        } catch (error) {
            threw = true;
            expect(error).toBeInstanceOf(ReferenceError);
        }
        expect(threw).toBeTrue();
        expect(callee.call(null, true)).toEqual(["initialized", 12345678901234567890n, 1.25]);
        expect(() => callee.apply(null, [false])).toThrow(ReferenceError);
    }
});

test("function entry preserves the receiver and argument slots", () => {
    function callee(first, second = 42) {
        return [this, first, second, arguments.length];
    }
    const receiver = {};
    const argument = {};
    const bound = callee.bind(receiver, argument);
    for (let iteration = 0; iteration < 3; ++iteration) {
        expect(callee.call(receiver, argument)).toEqual([receiver, argument, 42, 1]);
        expect(Reflect.apply(callee, receiver, [argument, 7])).toEqual([receiver, argument, 7, 2]);
        expect(bound()).toEqual([receiver, argument, 42, 1]);
    }
});

test("generator resumption preserves initialized locals and constants", () => {
    function* callee(argument) {
        let local = argument;
        yield local;
        local += 1.25;
        yield local;
        gc();
        return [local, 12345678901234567890n];
    }
    const first = callee(10);
    const second = callee(20);
    expect(first.next()).toEqual({ value: 10, done: false });
    expect(second.next()).toEqual({ value: 20, done: false });
    expect(first.next()).toEqual({ value: 11.25, done: false });
    expect(second.next()).toEqual({ value: 21.25, done: false });
    expect(first.next()).toEqual({ value: [11.25, 12345678901234567890n], done: true });
    expect(second.next()).toEqual({ value: [21.25, 12345678901234567890n], done: true });
});
