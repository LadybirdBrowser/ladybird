function forLetClosure() {
    let fns = [];
    for (let i = 0; i < 3; i++) {
        fns.push(() => i);
    }
    return fns.map(f => f());
}
console.log(forLetClosure());
