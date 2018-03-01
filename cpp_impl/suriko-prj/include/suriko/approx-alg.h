#pragma once
#include <cmath>
#include <type_traits> // std::common_type
namespace suriko
{
// https://docs.scipy.org/doc/numpy-1.13.0/reference/generated/numpy.isclose.html
template<typename F1, typename F2>
bool IsClose(F1 a, F2 b,
             typename std::common_type<F1,F2>::type rtol = 1.0e-5,
             typename std::common_type<F1,F2>::type atol = 1.0e-8)
{
    typedef typename std::common_type<F1,F2>::type F;
    return std::abs(a - b) <= (atol + rtol * std::abs(std::max<F>(a, b)));
}

template <typename F>
constexpr auto Sqr(F x) -> F { return x*x; }

template <typename F>
constexpr auto Sign(F x) -> int { return x >= 0 ? 1 : -1; }
}
