/*
 * Copyright (c) 2021, Cesar Torres <shortanemoia@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Complex.h>

using namespace Test::Randomized;

namespace {

Complex<f64> gen_complex()
{
    auto r = Gen::number_f64();
    auto i = Gen::number_f64();
    return Complex<f64>(r, i);
}

Complex<f64> gen_complex(f64 min, f64 max)
{
    auto r = Gen::number_f64(min, max);
    auto i = Gen::number_f64(min, max);
    return Complex<f64>(r, i);
}

}

template<typename T>
void expect_approximate_complex(Complex<T> a, Complex<T> b)
{
    EXPECT_APPROXIMATE(a.real(), b.real());
    EXPECT_APPROXIMATE(a.imag(), b.imag());
}

TEST_CASE(Complex)
{
    auto a = Complex<float> { 1.f, 1.f };
    auto b = complex_real_unit<double> + Complex<double> { 0, 1 } * 1;
    EXPECT_APPROXIMATE(a.real(), b.real());
    EXPECT_APPROXIMATE(a.imag(), b.imag());

#ifdef AKCOMPLEX_CAN_USE_MATH_H
    EXPECT_APPROXIMATE((complex_imag_unit<float> - complex_imag_unit<float>).magnitude(), 0);
    EXPECT_APPROXIMATE((complex_imag_unit<float> + complex_real_unit<float>).magnitude(), sqrt(2));

    auto c = Complex<double> { 0., 1. };
    auto d = Complex<double>::from_polar(1., M_PI / 2.);
    EXPECT_APPROXIMATE(c.real(), d.real());
    EXPECT_APPROXIMATE(c.imag(), d.imag());

    c = Complex<double> { -1., 1. };
    d = Complex<double>::from_polar(sqrt(2.), 3. * M_PI / 4.);
    EXPECT_APPROXIMATE(c.real(), d.real());
    EXPECT_APPROXIMATE(c.imag(), d.imag());
    EXPECT_APPROXIMATE(d.phase(), 3. * M_PI / 4.);
    EXPECT_APPROXIMATE(c.magnitude(), d.magnitude());
    EXPECT_APPROXIMATE(c.magnitude(), sqrt(2.));
#endif
    EXPECT_EQ((complex_imag_unit<double> * complex_imag_unit<double>).real(), -1.);
    EXPECT_EQ((complex_imag_unit<double> / complex_imag_unit<double>).real(), 1.);

    EXPECT_EQ(Complex(1., 10.) == (Complex<double>(1., 0.) + Complex(0., 10.)), true);
    EXPECT_EQ(Complex(1., 10.) != (Complex<double>(1., 1.) + Complex(0., 10.)), true);
#ifdef AKCOMPLEX_CAN_USE_MATH_H
    EXPECT_EQ(approx_eq(Complex<int>(1), Complex<float>(1.0000004f)), true);
    EXPECT_APPROXIMATE(cexp(Complex<double>(0., 1.) * M_PI).real(), -1.);
#endif
}

TEST_CASE(real_operators_regression)
{
    {
        auto c = Complex(0., 0.);
        c += 1;
        EXPECT_EQ(c.real(), 1);
    }
    {
        auto c = Complex(0., 0.);
        c -= 1;
        EXPECT_EQ(c.real(), -1);
    }
    {
        auto c1 = Complex(1., 1.);
        auto c2 = 1 - c1;
        EXPECT_EQ(c2.real(), 0);
        EXPECT_EQ(c2.imag(), -1);
    }
    {
        auto c1 = Complex(1., 1.);
        auto c2 = 1 / c1;
        EXPECT_EQ(c2.real(), 0.5);
        EXPECT_EQ(c2.imag(), -0.5);
    }
}

TEST_CASE(constructor_0_is_origin)
{
    auto c = Complex<f64>();
    EXPECT_EQ(c.real(), 0L);
    EXPECT_EQ(c.imag(), 0L);
}

RANDOMIZED_TEST_CASE(constructor_1)
{
    GEN(r, Gen::number_f64());
    auto c = Complex<f64>(r);
    EXPECT_EQ(c.real(), r);
    EXPECT_EQ(c.imag(), 0L);
}

RANDOMIZED_TEST_CASE(constructor_2)
{
    GEN(r, Gen::number_f64());
    GEN(i, Gen::number_f64());
    auto c = Complex<f64>(r, i);
    EXPECT_EQ(c.real(), r);
    EXPECT_EQ(c.imag(), i);
}

RANDOMIZED_TEST_CASE(magnitude_squared)
{
    GEN(c, gen_complex());
    auto magnitude_squared = c.magnitude_squared();
    auto magnitude = c.magnitude();
    EXPECT_APPROXIMATE(magnitude_squared, magnitude * magnitude);
}

RANDOMIZED_TEST_CASE(from_polar_magnitude)
{
    // Magnitude only makes sense non-negative, but the library allows it to be negative.
    GEN(m, Gen::number_f64(-1000, 1000));
    GEN(p, Gen::number_f64(-1000, 1000));
    auto c = Complex<f64>::from_polar(m, p);
    EXPECT_APPROXIMATE(c.magnitude(), abs(m));
}

RANDOMIZED_TEST_CASE(from_polar_phase)
{
    // To have a meaningful phase, magnitude needs to be >0.
    GEN(m, Gen::number_f64(1, 1000));
    GEN(p, Gen::number_f64(-1000, 1000));

    auto c = Complex<f64>::from_polar(m, p);

    // Returned phase is in the (-pi,pi] interval.
    // We need to mod from our randomly generated [-1000,1000] interval]
    // down to [0,2pi) or (-2pi,0] depending on our sign.
    // Then we can adjust and get into the -pi..pi range by adding/subtracting
    // one last 2pi.
    auto wanted_p = fmod(p, 2 * M_PI);
    if (wanted_p > M_PI)
        wanted_p -= 2 * M_PI;
    else if (wanted_p < -M_PI)
        wanted_p += 2 * M_PI;

    EXPECT_APPROXIMATE(c.phase(), wanted_p);
}

RANDOMIZED_TEST_CASE(imag_untouched_c_plus_r)
{
    GEN(c1, gen_complex());
    GEN(r2, Gen::number_f64());
    auto c2 = c1 + r2;
    EXPECT_EQ(c2.imag(), c1.imag());
}

RANDOMIZED_TEST_CASE(imag_untouched_c_minus_r)
{
    GEN(c1, gen_complex());
    GEN(r2, Gen::number_f64());
    auto c2 = c1 - r2;
    EXPECT_EQ(c2.imag(), c1.imag());
}

RANDOMIZED_TEST_CASE(assignment_same_as_binop_plus)
{
    GEN(c1, gen_complex());
    GEN(c2, gen_complex());
    auto out1 = c1 + c2;
    auto out2 = c1;
    out2 += c2;
    EXPECT_EQ(out2, out1);
}

RANDOMIZED_TEST_CASE(assignment_same_as_binop_minus)
{
    GEN(c1, gen_complex());
    GEN(c2, gen_complex());
    auto out1 = c1 - c2;
    auto out2 = c1;
    out2 -= c2;
    EXPECT_EQ(out2, out1);
}

RANDOMIZED_TEST_CASE(assignment_same_as_binop_mult)
{
    GEN(c1, gen_complex(-1000, 1000));
    GEN(c2, gen_complex(-1000, 1000));
    auto out1 = c1 * c2;
    auto out2 = c1;
    out2 *= c2;
    EXPECT_EQ(out2, out1);
}

RANDOMIZED_TEST_CASE(assignment_same_as_binop_div)
{
    GEN(c1, gen_complex(-1000, 1000));
    GEN(c2, gen_complex(-1000, 1000));
    auto out1 = c1 / c2;
    auto out2 = c1;
    out2 /= c2;
    EXPECT_EQ(out2, out1);
}

RANDOMIZED_TEST_CASE(commutativity_c_c)
{
    GEN(c1, gen_complex());
    GEN(c2, gen_complex());
    expect_approximate_complex(c1 + c2, c2 + c1);
    expect_approximate_complex(c1 * c2, c2 * c1);
}

RANDOMIZED_TEST_CASE(commutativity_c_r)
{
    GEN(c, gen_complex());
    GEN(r, Gen::number_f64());
    expect_approximate_complex(r + c, c + r);
    expect_approximate_complex(r * c, c * r);
}

RANDOMIZED_TEST_CASE(unary_plus_noop)
{
    GEN(c, gen_complex());
    EXPECT_EQ(+c, c);
}

RANDOMIZED_TEST_CASE(unary_minus_inverse)
{
    GEN(c, gen_complex());
    expect_approximate_complex(-(-c), c);
}

RANDOMIZED_TEST_CASE(wrapping_real)
{
    GEN(c, gen_complex(-1000, 1000));
    GEN(r, Gen::number_f64(-1000, 1000));
    auto cr = Complex<f64>(r);

    expect_approximate_complex(r + c, cr + c);
    expect_approximate_complex(r - c, cr - c);
    expect_approximate_complex(r * c, cr * c);
    expect_approximate_complex(r / c, cr / c);

    expect_approximate_complex(c + r, c + cr);
    expect_approximate_complex(c - r, c - cr);
    expect_approximate_complex(c * r, c * cr);
    expect_approximate_complex(c / r, c / cr);
}
