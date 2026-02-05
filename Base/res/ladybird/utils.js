export function getByteFormatter(optionsFunction) {
    const BYTE_UNITS = ["byte", "kilobyte", "megabyte", "gigabyte", "terabyte"];
    const BYTE_FORMATTERS = {
        byte: undefined,
        kilobyte: undefined,
        megabyte: undefined,
        gigabyte: undefined,
        terabyte: undefined,
        petabyte: undefined,
    };
    return {
        formatBytes: bytes => {
            let index = 0;
            while (bytes >= 1024 && index < BYTE_UNITS.length - 1) {
                bytes /= 1024;
                ++index;
            }

            const unit = BYTE_UNITS[index];

            if (!BYTE_FORMATTERS[unit]) {
                let options = { style: "unit", unit: unit };
                if (optionsFunction) {
                    options = { ...options, ...optionsFunction(unit) };
                }
                BYTE_FORMATTERS[unit] = new Intl.NumberFormat([], options);
            }

            return BYTE_FORMATTERS[unit].format(bytes);
        },
    };
}
