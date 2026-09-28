function outer() {
    let a = 1;
    function evaluates() {
        eval("");
        return () => a;
    }
    function withObject(object) {
        with (object) {
            return () => a;
        }
    }
    return [evaluates()(), withObject({})()];
}

outer();
