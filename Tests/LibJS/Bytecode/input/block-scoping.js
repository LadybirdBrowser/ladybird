function closureOverBlockScope() {
    let fns = [];
    {
        let x = 1;
        fns.push(() => x);
    }
    {
        let y = 2;
        fns.push(() => y);
    }
    return fns[0]() + fns[1]();
}
console.log(closureOverBlockScope());

function breakThroughBlockScopes() {
    let result;
    outer: for (let i = 0; i < 3; i++) {
        {
            let x = i;
            let f = () => x;
            if (x > 1) {
                result = f;
                break outer;
            }
        }
    }
    return result();
}
console.log(breakThroughBlockScopes());
