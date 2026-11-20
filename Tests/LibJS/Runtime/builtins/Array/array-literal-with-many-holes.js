test("holes in a long, sparse array literal stay holes", () => {
    const x = {};
    // NB: Long array literals are built element by element, and this one becomes sparse enough to turn into a
    //     dictionary while its holes go in.
    const array = eval("[x" + ",".repeat(401) + "x, , x]");
    expect(array.length).toBe(404);
    expect(Object.keys(array)).toEqual(["0", "401", "403"]);
    for (let i = 1; i < 401; ++i) {
        expect(i in array).toBeFalse();
        expect(Object.getOwnPropertyDescriptor(array, i)).toBeUndefined();
    }
    expect(402 in array).toBeFalse();
    expect(array[0]).toBe(x);
    expect(array[401]).toBe(x);
    expect(array[403]).toBe(x);

    const concatenated = Array(2).concat(array);
    expect(concatenated.length).toBe(406);
    expect(Object.keys(concatenated)).toEqual(["2", "403", "405"]);
});
