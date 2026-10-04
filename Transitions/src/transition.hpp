#pragma once

#include "generated_parameters.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sunimo {

inline double clamp(double value, double low = 0.0, double high = 1.0)
{
    return std::clamp(std::isfinite(value) ? value : 0.0, low, high);
}
inline double ease(double value)
{
    const double t = clamp(value);
    return t * t * t * (t * (t * 6 - 15) + 10);
}
inline double window(double value, double low, double high)
{
    return ease((value - low) / std::max(1e-9, high - low));
}
inline double bell(double value)
{
    const double sine = std::sin(3.14159265358979323846 * clamp(value));
    return sine * sine;
}

struct TransitionConfig {
    int style = 0;
    int direction = 0;
    double strength = parameters::defaultStrength;
    double softness = parameters::defaultSoftness;
    double cx = parameters::defaultCenter;
    double cy = parameters::defaultCenter;
    int quality = 0;
};

class TransitionRenderer
{
public:
    void render(int width, int height, const std::uint8_t *first, const std::uint8_t *second,
                std::uint8_t *output, double progress, const TransitionConfig &config);

private:
    std::vector<std::uint8_t> copyA;
    std::vector<std::uint8_t> copyB;
    std::vector<std::uint8_t> blurA;
    std::vector<std::uint8_t> blurB;
    std::vector<std::uint8_t> work;
    std::vector<int> mapCache;
    std::vector<int> maskCache;
};

} // namespace sunimo
