#include "workbench/shape_tween.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace luciole::workbench {
  namespace {
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

    float lerp(float a, float b, float t) { return a + (b - a) * t; }

    ShapePoint lerp(ShapePoint a, ShapePoint b, float t) { return {.x = lerp(a.x, b.x, t), .y = lerp(a.y, b.y, t)}; }

    float toLinear(std::uint8_t channel) {
      const float c = channel / 255.0F;
      return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
    }

    std::uint8_t toByte(float unit) {
      return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0F, 1.0F) * 255.0F));
    }

    std::uint8_t fromLinear(float linear) {
      const float c = linear <= 0.0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
      return toByte(c);
    }

    Rgba8 mixLinear(Rgba8 a, Rgba8 b, float t) {
      const auto channel = [t](std::uint8_t x, std::uint8_t y) {
        return fromLinear(lerp(toLinear(x), toLinear(y), t));
      };
      return {
          .r = channel(a.r, b.r),
          .g = channel(a.g, b.g),
          .b = channel(a.b, b.b),
          .a = toByte(lerp(a.a / 255.0F, b.a / 255.0F, t)),
      };
    }

    bool stroked(const Shape& shape) { return std::isfinite(shape.strokeWidth) && shape.strokeWidth > 0; }

    float soft(const Shape& shape) {
      return std::isfinite(shape.softness) && shape.softness > 0 ? shape.softness : 0.0F;
    }

    ShapeSector blendSector(const ShapeSector& a, const ShapeSector& b, float t) {
      const float start = a.a0 + std::remainder(b.a0 - a.a0, 360.0F) * t;
      const float span = lerp(a.a1 - a.a0, b.a1 - b.a0, t);
      return {
          .center = lerp(a.center, b.center, t),
          .r0 = lerp(a.r0, b.r0, t),
          .r1 = lerp(a.r1, b.r1, t),
          .a0 = start,
          .a1 = start + span,
      };
    }

    // Two shapes of one kind at `t` between them; identity, kind, order and hit target come from `b`.
    Shape blend(const Shape& a, const Shape& b, float t) {
      Shape out = b;
      out.opacity = lerp(a.opacity, b.opacity, t);
      out.fill = mixLinear(a.fill, b.fill, t);
      // A side without a stroke takes the other side's stroke color, so a stroke grows in one color.
      out.stroke = mixLinear(stroked(a) ? a.stroke : b.stroke, stroked(b) ? b.stroke : a.stroke, t);
      out.strokeWidth = lerp(stroked(a) ? a.strokeWidth : 0.0F, stroked(b) ? b.strokeWidth : 0.0F, t);
      out.pattern = t < 0.5F ? a.pattern : b.pattern;
      // A side without a hatch color draws its stripes in its stroke color.
      out.hatch = a.hatch || b.hatch
          ? std::optional(mixLinear(a.hatch.value_or(a.stroke), b.hatch.value_or(b.stroke), t))
          : std::nullopt;
      out.softness = lerp(soft(a), soft(b), t);
      switch (b.kind) {
      case ShapeKind::Rect:
      case ShapeKind::RoundedRect:
        out.rect = {
            .x = lerp(a.rect.x, b.rect.x, t),
            .y = lerp(a.rect.y, b.rect.y, t),
            .width = lerp(a.rect.width, b.rect.width, t),
            .height = lerp(a.rect.height, b.rect.height, t),
            .radius = lerp(a.rect.radius, b.rect.radius, t),
        };
        break;
      case ShapeKind::Sector:
        out.sector = blendSector(a.sector, b.sector, t);
        break;
      case ShapeKind::Circle:
        out.circle = {
            .center = lerp(a.circle.center, b.circle.center, t), .radius = lerp(a.circle.radius, b.circle.radius, t)
        };
        break;
      case ShapeKind::Ribbon:
        for (std::size_t i = 0; i < out.ribbon.top.size(); ++i) {
          out.ribbon.top[i] = lerp(a.ribbon.top[i], b.ribbon.top[i], t);
          out.ribbon.bottom[i] = lerp(a.ribbon.bottom[i], b.ribbon.bottom[i], t);
        }
        break;
      case ShapeKind::Line:
        out.line = {
            .from = lerp(a.line.from, b.line.from, t),
            .to = lerp(a.line.to, b.line.to, t),
            .width = lerp(a.line.width, b.line.width, t),
        };
        break;
      }
      return out;
    }

    Shape faded(const Shape& shape, float keep, bool leaving) {
      Shape out = shape;
      out.opacity *= keep;
      out.hittable = out.hittable && !leaving;
      return out;
    }

    // Indexes ordered by id, repeated ids in list order.
    std::vector<std::size_t> byId(const std::vector<Shape>& shapes) {
      std::vector<std::size_t> order(shapes.size());
      std::ranges::iota(order, std::size_t{0});
      std::ranges::stable_sort(order, {}, [&shapes](std::size_t i) {
        return std::pair(shapes[i].id.node, shapes[i].id.role);
      });
      return order;
    }
  } // namespace

  ShapeTween::ShapeTween(std::span<const Shape> from, std::span<const Shape> to)
      : m_from(from.begin(), from.end()), m_to(to.begin(), to.end()) {
    const auto a = byId(m_from);
    const auto b = byId(m_to);
    std::vector<std::size_t> partner(m_to.size(), kNone);
    std::vector<bool> pairedFrom(m_from.size(), false);
    const auto key = [](const Shape& shape) { return std::pair(shape.id.node, shape.id.role); };
    // Each step advances i, j or both, so the walk ends after at most a.size() + b.size() steps.
    for (std::size_t i = 0, j = 0; i < a.size() && j < b.size();) {
      const auto left = key(m_from[a[i]]);
      const auto right = key(m_to[b[j]]);
      if (left == right) {
        partner[b[j]] = a[i];
        pairedFrom[a[i]] = true;
        ++i;
        ++j;
      } else if (left < right) {
        ++i;
      } else {
        ++j;
      }
    }
    m_steps.reserve(m_from.size() + m_to.size());
    for (std::size_t f = 0; f < m_from.size(); ++f) {
      if (!pairedFrom[f]) {
        m_steps.push_back({.from = f, .to = kNone});
      }
    }
    for (std::size_t t = 0; t < m_to.size(); ++t) {
      const auto f = partner[t];
      if (f != kNone && m_from[f].kind != m_to[t].kind) {
        m_steps.push_back({.from = f, .to = kNone});
        m_steps.push_back({.from = kNone, .to = t});
      } else {
        m_steps.push_back({.from = f, .to = t});
      }
    }
  }

  void ShapeTween::frame(float progress, std::vector<Shape>& out) const {
    if (!(progress > 0)) {
      out.assign(m_from.begin(), m_from.end());
      return;
    }
    if (progress >= 1) {
      out.assign(m_to.begin(), m_to.end());
      return;
    }
    out.clear();
    out.reserve(m_steps.size());
    for (const auto& step : m_steps) {
      if (step.from != kNone && step.to != kNone) {
        out.push_back(blend(m_from[step.from], m_to[step.to], progress));
      } else if (step.from != kNone) {
        out.push_back(faded(m_from[step.from], 1 - progress, true));
      } else {
        out.push_back(faded(m_to[step.to], progress, false));
      }
    }
  }

  std::size_t ShapeTween::capacity() const { return m_from.size() + m_to.size(); }
} // namespace luciole::workbench
