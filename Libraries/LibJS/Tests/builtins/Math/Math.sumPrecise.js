test("basic functionality", () => {
    // https://github.com/tc39/test262/tree/main/test/built-ins/Math/sumPrecise

    expect(Math.sumPrecise([NaN])).toBe(NaN);
    expect(Math.sumPrecise([Infinity, -Infinity])).toBe(NaN);
    expect(Math.sumPrecise([-Infinity, Infinity])).toBe(NaN);
    expect(Math.sumPrecise([Infinity])).toBe(Infinity);
    expect(Math.sumPrecise([Infinity, Infinity])).toBe(Infinity);
    expect(Math.sumPrecise([-Infinity])).toBe(-Infinity);
    expect(Math.sumPrecise([-Infinity, -Infinity])).toBe(-Infinity);
    expect(Math.sumPrecise([])).toBe(-0);
    expect(Math.sumPrecise([-0])).toBe(-0);
    expect(Math.sumPrecise([-0, -0])).toBe(-0);
    expect(Math.sumPrecise([-0, 0])).toBe(0);
    expect(Math.sumPrecise([1, 2])).toBe(3);
    expect(Math.sumPrecise([1, 2, 3])).toBe(6);
    expect(Math.sumPrecise([1e308])).toBe(1e308);
    expect(Math.sumPrecise([1e308, -1e308])).toBe(0);
    expect(Math.sumPrecise([0.1])).toBe(0.1);
    expect(Math.sumPrecise([0.1, 0.1])).toBe(0.2);
    expect(Math.sumPrecise([0.1, -0.1])).toBe(0);
    expect(Math.sumPrecise([1e308, 1e308, 0.1, 0.1, 1e30, 0.1, -1e30, -1e308, -1e308])).toBe(
        0.30000000000000004
    );
    expect(Math.sumPrecise([1e30, 0.1, -1e30])).toBe(0.1);
    expect(
        Math.sumPrecise([8.98846567431158e307, 8.988465674311579e307, -1.7976931348623157e308])
    ).toBe(9.9792015476736e291);
    expect(
        Math.sumPrecise([-5.630637621603525e255, 9.565271205476345e307, 2.9937604643020797e292])
    ).toBe(9.565271205476347e307);
    expect(
        Math.sumPrecise([
            6.739986666787661e66, 2, -1.2689709186578243e-116, 1.7046015739467354e308,
            -9.979201547673601e291, 6.160926733208294e307, -3.179557053031852e234,
            -7.027282978772846e307, -0.7500000000000001,
        ])
    ).toBe(1.61796594939028e308);
    expect(
        Math.sumPrecise([
            0.31150493246968836, -8.988465674311582e307, 1.8315037361673755e-270,
            -15.999999999999996, 2.9999999999999996, 7.345200721499384e164, -2.033582473639399,
            -8.98846567431158e307, -3.5737295155405993e292, 4.13894772383715e-124,
            -3.6111186457260667e-35, 2.387234887098013e180, 7.645295562778372e-298,
            3.395189016861822e-103, -2.6331611115768973e-149,
        ])
    ).toBe(-Infinity);
    expect(
        Math.sumPrecise([
            -1.1442589134409902e308, 9.593842098384855e138, 4.494232837155791e307,
            -1.3482698511467367e308, 4.494232837155792e307,
        ])
    ).toBe(-1.5936821971565685e308);
    expect(
        Math.sumPrecise([
            -1.1442589134409902e308, 4.494232837155791e307, -1.3482698511467367e308,
            4.494232837155792e307,
        ])
    ).toBe(-1.5936821971565687e308);
    expect(
        Math.sumPrecise([
            9.593842098384855e138, -6.948356297254111e307, -1.3482698511467367e308,
            4.494232837155792e307,
        ])
    ).toBe(-1.5936821971565685e308);
    expect(
        Math.sumPrecise([-2.534858246857893e115, 8.988465674311579e307, 8.98846567431158e307])
    ).toBe(1.7976931348623157e308);
    expect(
        Math.sumPrecise([1.3588124894186193e308, 1.4803986201152006e223, 6.741349255733684e307])
    ).toBe(Infinity);
    expect(
        Math.sumPrecise([6.741349255733684e307, 1.7976931348623155e308, -7.388327292663961e41])
    ).toBe(Infinity);
    expect(
        Math.sumPrecise([-1.9807040628566093e28, 1.7976931348623157e308, 9.9792015476736e291])
    ).toBe(1.7976931348623157e308);
    expect(
        Math.sumPrecise([
            -1.0214557991173964e61, 1.7976931348623157e308, 8.98846567431158e307,
            -8.988465674311579e307,
        ])
    ).toBe(1.7976931348623157e308);
    expect(
        Math.sumPrecise([
            1.7976931348623157e308, 7.999999999999999, -1.908963895403937e-230,
            1.6445950082320264e292, 2.0734856707605806e205,
        ])
    ).toBe(Infinity);
    expect(
        Math.sumPrecise([6.197409167220438e-223, -9.979201547673601e291, -1.7976931348623157e308])
    ).toBe(-Infinity);
    expect(
        Math.sumPrecise([
            4.49423283715579e307, 8.944251746776101e307, -0.0002441406250000001,
            1.1752060710043817e308, 4.940846717201632e292, -1.6836699406454528e308,
        ])
    ).toBe(8.353845887521184e307);
    expect(
        Math.sumPrecise([
            8.988465674311579e307, 7.999999999999998, 7.029158107234023e-308,
            -2.2303483759420562e-172, -1.7976931348623157e308, -8.98846567431158e307,
        ])
    ).toBe(-1.7976931348623157e308);
    expect(Math.sumPrecise([8.98846567431158e307, 8.98846567431158e307])).toBe(Infinity);
});
