test("invariants", () => {
    expect(String.prototype.matchAll).toHaveLength(1);
});

test("error cases", () => {
    [null, undefined].forEach(value => {
        expect(() => {
            value.matchAll("");
        }).toThrow(TypeError);
    });

    expect(() => {
        "hello friends".matchAll(/hello/);
    }).toThrow(TypeError);
});

test("basic functionality", () => {
    expect("hello friends".matchAll(/hello/g)).not.toBeNull();
    expect("hello friends".matchAll(/enemies/g)).not.toBeNull();

    {
        var iterator = "".matchAll(/a/g);

        var next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
    {
        var iterator = "a".matchAll(/a/g);

        var next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["a"]);
        expect(next.value.index).toBe(0);

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
    {
        var iterator = "aa".matchAll(/a/g);

        var next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["a"]);
        expect(next.value.index).toBe(0);

        next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["a"]);
        expect(next.value.index).toBe(1);

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
    {
        var iterator = "aba".matchAll(/a/g);

        var next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["a"]);
        expect(next.value.index).toBe(0);

        next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["a"]);
        expect(next.value.index).toBe(2);

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
});

test("UTF-16", () => {
    {
        var iterator = "😀".matchAll("foo");

        var next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
    {
        var iterator = "😀".matchAll("\ud83d");

        var next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["\ud83d"]);
        expect(next.value.index).toBe(0);

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
    {
        var iterator = "😀😀".matchAll("\ud83d");

        var next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["\ud83d"]);
        expect(next.value.index).toBe(0);

        next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["\ud83d"]);
        expect(next.value.index).toBe(2);

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
    {
        var iterator = "😀😀".matchAll("\ude00");

        var next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["\ude00"]);
        expect(next.value.index).toBe(1);

        next = iterator.next();
        expect(next.done).toBeFalse();
        expect(next.value).toEqual(["\ude00"]);
        expect(next.value.index).toBe(3);

        next = iterator.next();
        expect(next.done).toBeTrue();
        expect(next.value).toBeUndefined();
    }
});
