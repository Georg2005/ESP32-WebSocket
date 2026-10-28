#pragma once
#include <math.h>
struct Limits {
  float tMin = 18, tMax = 30, hMin = 30, hMax = 70;
};
inline bool validLimits(const Limits& x) {
  return isfinite(x.tMin) && isfinite(x.tMax) &&
    isfinite(x.hMin) && isfinite(x.hMax) &&
    x.tMin >= -40 && x.tMax <= 80 && x.tMin < x.tMax &&
    x.hMin >= 0 && x.hMax <= 100 && x.hMin < x.hMax;
}
// Бітова маска дозволяє повідомити відразу про декілька відхилень.
inline unsigned alarmMask(float t, float h, const Limits& x) {
  return (t < x.tMin ? 1u : 0u) | (t > x.tMax ? 2u : 0u) |
         (h < x.hMin ? 4u : 0u) | (h > x.hMax ? 8u : 0u);
}
