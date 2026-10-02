#include "FftDouble.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <utility>

namespace ampsim
{

FftDouble::FftDouble (int order) : n (1 << order)
{
    twiddles.resize ((size_t) n / 2);
    for (int k = 0; k < n / 2; ++k)
        twiddles[(size_t) k] = std::polar (1.0, -2.0 * std::numbers::pi * k / n);

    bitReversed.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < order; ++b)
            r |= ((i >> b) & 1) << (order - 1 - b);
        bitReversed[(size_t) i] = r;
    }
}

void FftDouble::inverse (std::vector<std::complex<double>>& data) const
{
    perform (data, true);
    const auto scale = 1.0 / n;
    for (auto& v : data)
        v *= scale;
}

void FftDouble::perform (std::vector<std::complex<double>>& data, bool conjugateTwiddles) const
{
    assert ((int) data.size() == n);

    for (int i = 0; i < n; ++i)
        if (i < bitReversed[(size_t) i])
            std::swap (data[(size_t) i], data[(size_t) bitReversed[(size_t) i]]);

    // Pass with butterflies spanning `length` samples; the twiddle for butterfly k is W^(k N/length).
    for (int length = 2; length <= n; length <<= 1)
    {
        const auto half = length / 2, stride = n / length;

        for (int start = 0; start < n; start += length)
        {
            for (int k = 0; k < half; ++k)
            {
                auto w = twiddles[(size_t) (k * stride)];
                if (conjugateTwiddles)
                    w = std::conj (w);

                const auto even = data[(size_t) (start + k)];
                const auto odd = data[(size_t) (start + k + half)] * w;
                data[(size_t) (start + k)] = even + odd;
                data[(size_t) (start + k + half)] = even - odd;
            }
        }
    }
}

} // namespace ampsim
