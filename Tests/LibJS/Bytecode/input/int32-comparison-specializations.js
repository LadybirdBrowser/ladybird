function comparison_results(value) {
    return [
        value < 2,
        value <= 2,
        value > 2,
        value >= 2,
    ];
}

function comparison_jumps(value) {
    let result = 0;
    if (value < 2)
        result += 1;
    if (value <= 2)
        result += 2;
    if (value > 2)
        result += 4;
    if (value >= 2)
        result += 8;
    return result;
}

comparison_results(1);
comparison_jumps(1);
