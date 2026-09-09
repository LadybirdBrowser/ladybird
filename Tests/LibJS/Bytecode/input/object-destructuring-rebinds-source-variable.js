function run(source) {
    var { first, second: source, third } = source;
    return [first, source, third];
}
console.log(run({ first: 1, second: 2, third: 3 }));
