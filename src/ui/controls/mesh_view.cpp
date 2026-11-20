#include "ui/controls/mesh_view.h"

#include "cursor-shape-v1-client-protocol.h"
#include "formats/mesh.h"
#include "render/backend/render_backend.h"
#include "render/core/renderer.h"
#include "ui/controls/button.h"
#include "ui/controls/pane_header.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <numbers>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  using luciole::MeshGeometry;
  using luciole::formats::Transform;

  constexpr float kButtonInset = Style::spaceXs;
  constexpr double kFieldOfView = 35.0 * std::numbers::pi / 180.0; // vertical
  constexpr double kFitMargin = 1.08;

  Transform translation(double x, double y, double z) {
    Transform m = luciole::formats::kIdentityTransform;
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
  }

  Transform turnAroundX(double angle) {
    Transform m = luciole::formats::kIdentityTransform;
    m[5] = std::cos(angle);
    m[6] = std::sin(angle);
    m[9] = -std::sin(angle);
    m[10] = std::cos(angle);
    return m;
  }

  Transform turnAroundY(double angle) {
    Transform m = luciole::formats::kIdentityTransform;
    m[0] = std::cos(angle);
    m[2] = -std::sin(angle);
    m[8] = std::sin(angle);
    m[10] = std::cos(angle);
    return m;
  }

  MeshMatrix toFloat(const Transform& m) {
    MeshMatrix out{};
    std::ranges::transform(m, out.begin(), [](double v) { return static_cast<float>(v); });
    return out;
  }
} // namespace

MeshCamera meshCamera(const MeshGeometry& geometry, const MeshOrbit& orbit, float aspect) {
  const auto& box = geometry.bounds;
  std::array<double, 3> center{
      (double{box.min.x} + box.max.x) / 2, (double{box.min.y} + box.max.y) / 2, (double{box.min.z} + box.max.z) / 2
  };
  double radius =
      std::hypot(double{box.max.x} - box.min.x, double{box.max.y} - box.min.y, double{box.max.z} - box.min.z) / 2;
  if (!std::ranges::all_of(center, [](double v) { return std::isfinite(v); })) {
    center = {};
  }
  if (!(radius > 0) || !std::isfinite(radius)) {
    radius = 1;
  }
  const double ratio = std::isfinite(aspect) && aspect > 0 ? std::clamp(aspect, kMinMeshAspect, 1 / kMinMeshAspect) : 1;
  const double yaw = std::isfinite(orbit.yaw) ? orbit.yaw : 0;
  const double pitch =
      std::isfinite(orbit.pitch) ? std::clamp(orbit.pitch, -MeshOrbit::kMaxPitch, MeshOrbit::kMaxPitch) : 0;
  const double zoom = std::isfinite(orbit.zoom) ? std::clamp(orbit.zoom, MeshOrbit::kMinZoom, MeshOrbit::kMaxZoom) : 1;
  const double half = kFieldOfView / 2;
  const double fit = std::min(half, std::atan(std::tan(half) * ratio));
  const double distance = radius * kFitMargin / std::sin(fit) / zoom;
  const double near = std::max(distance - (radius * 1.01), radius * 0.01);
  const double far = distance + (radius * 1.01);

  using luciole::formats::multiply;
  const Transform up =
      geometry.up == luciole::MeshUp::Z ? turnAroundX(-std::numbers::pi / 2) : luciole::formats::kIdentityTransform;
  const Transform model = multiply(up, translation(-center[0], -center[1], -center[2]));
  const Transform turned = multiply(turnAroundX(pitch), multiply(turnAroundY(yaw), model));
  Transform projection{};
  const double focal = 1 / std::tan(half);
  projection[0] = focal / ratio;
  projection[5] = focal;
  projection[10] = (far + near) / (near - far);
  projection[11] = -1;
  projection[14] = 2 * far * near / (near - far);
  return {.modelView = toFloat(multiply(translation(0, 0, -distance), turned)), .projection = toFloat(projection)};
}

MeshView::MeshView()
    : m_area(static_cast<InputArea*>(addChild(std::make_unique<InputArea>()))),
      m_reset(
          static_cast<Button*>(addChild(paneHeaderButton("focus-centered", "Reset view", [this] { resetOrbit(); })))
      ) {
  setClipChildren(true);
  m_area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
  m_area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
  m_area->setOnPress([this](const InputArea::PointerData& pointer) { press(pointer); });
  m_area->setOnMotion([this](const InputArea::PointerData& pointer) { motion(pointer); });
  m_area->setOnCancel([this] { m_dragging = false; });
  m_area->setOnAxisHandler([this](const InputArea::PointerData& pointer) {
    if (pointer.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    const float delta = pointer.scrollDelta(Style::scrollWheelStep);
    setOrbit(
        MeshOrbit{
            .yaw = m_orbit.yaw,
            .pitch = m_orbit.pitch,
            .zoom = m_orbit.zoom * std::exp(-delta * kZoomPerPixel),
        }
    );
    return true;
  });
  m_paletteConn = paletteChanged().connect([this] { markPaintDirty(); });
}

MeshView::~MeshView() {
  if (m_renderer != nullptr) {
    release(*m_renderer);
  }
}

void MeshView::setGeometry(std::shared_ptr<const luciole::MeshGeometry> geometry) {
  m_geometry = std::move(geometry);
  ++m_revision;
  m_orbit = MeshOrbit{};
  m_dragging = false;
  markPaintDirty();
}

void MeshView::resetOrbit() { setOrbit(MeshOrbit{}); }

void MeshView::setPreviewScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0 || scale == m_previewScale) {
    return;
  }
  m_previewScale = scale;
  markPaintDirty();
}

MeshCamera MeshView::camera(float aspect) const {
  if (!m_geometry) {
    return {};
  }
  return meshCamera(
      *m_geometry, MeshOrbit{.yaw = m_orbit.yaw, .pitch = m_orbit.pitch, .zoom = m_orbit.zoom * m_previewScale}, aspect
  );
}

void MeshView::setOrbit(const MeshOrbit& orbit) {
  const MeshOrbit next{
      .yaw = std::isfinite(orbit.yaw) ? std::remainder(orbit.yaw, 2 * std::numbers::pi_v<float>) : m_orbit.yaw,
      .pitch = std::isfinite(orbit.pitch) ? std::clamp(orbit.pitch, -MeshOrbit::kMaxPitch, MeshOrbit::kMaxPitch)
                                          : m_orbit.pitch,
      .zoom =
          std::isfinite(orbit.zoom) ? std::clamp(orbit.zoom, MeshOrbit::kMinZoom, MeshOrbit::kMaxZoom) : m_orbit.zoom,
  };
  if (next == m_orbit) {
    return;
  }
  m_orbit = next;
  markPaintDirty();
}

void MeshView::press(const InputArea::PointerData& pointer) {
  m_dragging = pointer.pressed && pointer.button == BTN_LEFT;
  m_dragX = pointer.localX;
  m_dragY = pointer.localY;
  m_area->setCursorShape(m_dragging ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
}

void MeshView::motion(const InputArea::PointerData& pointer) {
  if (!m_dragging) {
    return;
  }
  const float dx = pointer.localX - m_dragX;
  const float dy = pointer.localY - m_dragY;
  m_dragX = pointer.localX;
  m_dragY = pointer.localY;
  setOrbit(
      MeshOrbit{
          .yaw = m_orbit.yaw + (dx * kTurnPerPixel),
          .pitch = m_orbit.pitch + (dy * kTurnPerPixel),
          .zoom = m_orbit.zoom,
      }
  );
}

void MeshView::release(Renderer& renderer) {
  if (!m_gpu) {
    return;
  }
  // The renderer's texture manager makes its context current, which freeing GL objects needs.
  (void)renderer.textureManager();
  m_gpu->release();
}

std::uint64_t MeshView::renders() const noexcept { return m_gpu ? m_gpu->renders() : 0; }

std::size_t MeshView::gpuBytes() const noexcept { return m_gpu ? m_gpu->bytes() : 0; }

void MeshView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  m_area->setFrameSize(width(), height());
  const LayoutSize button = m_reset->measure(renderer, LayoutConstraints::unconstrained());
  m_reset->arrange(
      renderer,
      {.x = std::max(0.0F, width() - button.width - kButtonInset),
       .y = kButtonInset,
       .width = button.width,
       .height = button.height}
  );
}

void MeshView::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  if (!m_geometry || m_geometry->indices.empty() || !(context.width > 0) || !(context.height > 0)) {
    return;
  }
  const float pixelWidth = std::round(context.width * context.pixelScaleX);
  const float pixelHeight = std::round(context.height * context.pixelScaleY);
  if (!(pixelWidth >= 1) || !(pixelHeight >= 1) || !std::isfinite(pixelWidth) || !std::isfinite(pixelHeight)) {
    return;
  }
  auto& program = backend.extension<MeshProgram>();
  program.ensureInitialized();
  if (!m_gpu || m_gpu->program() != program.serial()) {
    m_gpu = program.createGpu();
  }
  const MeshLook look{
      .camera = camera(context.width / context.height),
      .keyLight = colorForRole(ColorRole::Primary),
      .fillLight = colorForRole(ColorRole::Tertiary),
  };
  const auto limit = static_cast<float>(MeshProgram::kMaxImageSide);
  if (!program.render(
          *m_gpu, *m_geometry, m_revision, look, static_cast<int>(std::min(pixelWidth, limit)),
          static_cast<int>(std::min(pixelHeight, limit))
      )) {
    return;
  }
  backend.drawImage(
      RenderImageDraw{
          .texture = m_gpu->image(),
          .surfaceWidth = context.surfaceWidth,
          .surfaceHeight = context.surfaceHeight,
          .width = context.width,
          .height = context.height,
          .opacity = context.opacity,
          .fitMode = RenderImageFitMode::Stretch,
          .textureWidth = static_cast<float>(m_gpu->imageWidth()),
          .textureHeight = static_cast<float>(m_gpu->imageHeight()),
          .transform = context.transform,
      }
  );
}
