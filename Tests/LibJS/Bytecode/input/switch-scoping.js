function switchWithBlockDecl(x) {
    let result;
    switch (x) {
        case 1:
            let a = "one";
            result = () => a;
            break;
        case 2:
            let b = "two";
            result = () => b;
            break;
    }
    return result();
}
console.log(switchWithBlockDecl(1));
console.log(switchWithBlockDecl(2));
