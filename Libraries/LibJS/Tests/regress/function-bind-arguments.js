test("Some function.bind cases", () => {
    function collectArguments() {
        let a = [];
        for (let i = 0; i < arguments.length; ++i) a.push(arguments[i]);
        return a;
    }

    let b = collectArguments.bind(null);
    expect(b()).toEqual([]);
    expect(b(3, 4)).toEqual([3, 4]);
    expect(b(3, 4, 5, 6)).toEqual([3, 4, 5, 6]);

    let b12 = collectArguments.bind(null, 1, 2);
    expect(b12()).toEqual([1, 2]);
    expect(b12(3, 4)).toEqual([1, 2, 3, 4]);
    expect(b12(3, 4, 5, 6)).toEqual([1, 2, 3, 4, 5, 6]);
});
