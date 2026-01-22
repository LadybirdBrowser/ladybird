test("basic functionality", () => {
    let i = 0;
    let three;
    let five;

    for (; i < 9; ) {
        switch (i) {
            case 3:
                three = i;
                break;
            case 5:
                five = i;
                break;
        }
        ++i;
    }

    expect(three).toBe(3);
    expect(five).toBe(5);
});
