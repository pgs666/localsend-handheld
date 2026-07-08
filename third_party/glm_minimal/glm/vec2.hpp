#pragma once

namespace glm {

struct alignas(8) vec2 {
  float x;
  float y;

  constexpr vec2() : x(0.0f), y(0.0f) {}
  constexpr vec2(float scalar) : x(scalar), y(scalar) {}
  constexpr vec2(float x_value, float y_value) : x(x_value), y(y_value) {}
};

}  // namespace glm
