function int32_binary_specializations(value) {
    return [
        2 + value,
        value + 2,
        value - 2,
        value * 2,
        value ** 2,
        value / 2,
        value ^ 2,
        value & 2,
        value | 2,
        value << 2,
        value >> 2,
        value >>> 2,
        value % 2,
        value + 2.5,
    ];
}

int32_binary_specializations(8);
