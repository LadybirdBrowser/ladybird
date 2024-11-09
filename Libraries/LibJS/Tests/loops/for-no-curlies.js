test("basic functionality", () => {
    let number = 0;
    for (let i = 0; i < 3; ++i) for (let j = 0; j < 3; ++j) number++;
    expect(number).toBe(9);
});
