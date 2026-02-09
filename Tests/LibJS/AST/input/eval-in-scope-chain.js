function outer() {
    let x = 1;
    function middle() {
        eval("");
        function inner() {
            let y = 2;
            return x + y;
        }
        return inner();
    }
    return middle();
}
