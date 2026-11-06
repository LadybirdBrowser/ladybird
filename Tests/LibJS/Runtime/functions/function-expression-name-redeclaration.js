test("parameter with the name of the function expression", () => {
    const function_ = function name(name) {
        var captured = [1];
        function read() {
            return captured.length;
        }
        return [name, read()];
    };
    expect(function_(7)).toEqual([7, 1]);
});

test("var with the name of the function expression", () => {
    const function_ = function name() {
        var name;
        name = 1;
        function read() {
            return name;
        }
        return read();
    };
    expect(function_()).toBe(1);
});

test("function declaration with the name of the function expression", () => {
    const function_ = function name() {
        var other = 2;
        function name() {
            return other;
        }
        return name();
    };
    expect(function_()).toBe(2);
});
