function level0(a) {
    let x = 1;
    function level1(b) {
        let y = 2;
        function level2(c) {
            return a + x + b + y + c;
        }
        return level2(3);
    }
    return level1(2);
}

function closure_over_var() {
    var counter = 0;
    function increment() {
        counter = counter + 1;
        return counter;
    }
    return increment;
}
