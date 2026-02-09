function withBlock() {
    let obj = { x: 42 };
    with (obj) {
        console.log(x);
    }
}
withBlock();
