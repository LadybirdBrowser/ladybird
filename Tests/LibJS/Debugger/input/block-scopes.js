function shadowed() {
    let value = 1;
    {
        let value = 2;
        debugger;
    }
    debugger;
    return value;
}

shadowed();
