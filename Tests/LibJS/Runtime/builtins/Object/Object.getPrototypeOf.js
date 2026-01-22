test("basic functionality", () => {
    let o1 = new Object();
    let o2 = {};

    expect(Object.getPrototypeOf(o1)).toBe(Object.getPrototypeOf(o2));
    expect(Object.getPrototypeOf(Object.getPrototypeOf(o1))).toBeNull();

    Object.setPrototypeOf(o1, o2);
    expect(Object.getPrototypeOf(o1)).toBe(o2);
});
