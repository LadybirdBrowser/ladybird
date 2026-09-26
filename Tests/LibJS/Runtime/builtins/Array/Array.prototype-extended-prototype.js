describe("arrays keep their behavior when Array.prototype has extra properties", () => {
    Array.prototype.extension = function () {
        return this.length;
    };

    test("push and pop", () => {
        var array = [1, 2];
        expect(array.push(3, 4)).toBe(4);
        expect(array).toEqual([1, 2, 3, 4]);
        expect(array.pop()).toBe(4);
        expect(array.extension()).toBe(3);
    });

    test("concat spreads arrays", () => {
        expect([1].concat([2, 3], 4)).toEqual([1, 2, 3, 4]);
    });

    test("slice and splice", () => {
        var array = [1, 2, 3, 4];
        expect(array.slice(1, 3)).toEqual([2, 3]);
        expect(array.splice(1, 2)).toEqual([2, 3]);
        expect(array).toEqual([1, 4]);
    });

    test("indexed writes past the end of a holey array", () => {
        var array = [1, , 3];
        array[5] = 6;
        expect(array.length).toBe(6);
        expect(array[5]).toBe(6);
        expect(1 in array).toBeFalse();
    });

    test("an indexed setter on Array.prototype is still honored", () => {
        var setter_calls = [];
        Object.defineProperty(Array.prototype, 1, {
            set(value) {
                setter_calls.push(value);
            },
            configurable: true,
        });
        try {
            var array = [1];
            expect(array.push(2)).toBe(2);
            expect(setter_calls).toEqual([2]);
            expect(array.hasOwnProperty(1)).toBeFalse();
        } finally {
            delete Array.prototype[1];
        }
    });

    delete Array.prototype.extension;
});
