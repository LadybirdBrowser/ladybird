test("let scoping", () => {
    let i = 1;
    {
        let i = 3;
        expect(i).toBe(3);
    }
    expect(i).toBe(1);

    {
        const i = 2;
        expect(i).toBe(2);
    }
    expect(i).toBe(1);
});
