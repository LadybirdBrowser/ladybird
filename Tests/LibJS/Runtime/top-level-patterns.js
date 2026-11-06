let [first, second = 2] = [1];
const { third, ...others } = { third: 3, fourth: 4 };
var [fifth] = [5];

test("patterns in declarations of the top level", () => {
    expect(first).toBe(1);
    expect(second).toBe(2);
    expect(third).toBe(3);
    expect(others).toEqual({ fourth: 4 });
    expect(fifth).toBe(5);
});
