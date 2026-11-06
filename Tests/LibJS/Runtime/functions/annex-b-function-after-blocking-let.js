test("a let later in an enclosing block keeps a function from hoisting", () => {
    function run() {
        var outer;
        {
            function x() {
                return 1;
            }
            outer = x;
        }
        {
            {
                function x() {
                    return 2;
                }
            }
            let x;
        }
        {
            let x;
            {
                function x() {
                    return 3;
                }
            }
        }
        return x === outer;
    }
    expect(run()).toBeTrue();
});

test("a let later in the function keeps a function from hoisting", () => {
    function run() {
        {
            function x() {
                return 1;
            }
        }
        let x = 2;
        return x;
    }
    expect(run()).toBe(2);
});
