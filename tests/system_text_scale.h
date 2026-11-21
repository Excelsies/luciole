#pragma once

#include "render/render_context.h"
#include "render/text/system_font.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/style.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <glib.h>
#include <string>

namespace test {
  // Makes the system font give the toolkit text scale `scale`, as a user's GTK settings do: writes gtk-font-name under
  // the user config directory of the test's environment, which is a test-only one (never the user's) and reads the font
  // again. Returns the scale the font gives, which the views read. Needs XDG_CONFIG_HOME set to a test folder and
  // GSETTINGS_BACKEND=memory.
  inline float setSystemTextScale(float scale) {
    const std::filesystem::path config = g_get_user_config_dir(); // where the font is read from
    TEST_CHECK(config.filename() != ".config");                   // never the user's own
    const auto folder = config / "gtk-3.0";
    std::filesystem::create_directories(folder);
    constexpr float kBodyPoints = Style::fontSizeBody * 72.0F / 96.0F;
    std::ofstream(folder / "settings.ini", std::ios::trunc)
        << std::format("[Settings]\ngtk-font-name=Sans {:.4f}\n", scale * kBodyPoints);
    (void)text::refreshSystemFont();
    const float given = RenderContext::clampTextScale(text::currentSystemFont().textScale(Style::fontSizeBody));
    TEST_CHECK(std::abs(given - scale) < 0.01F);
    return given;
  }

  // The same for a rig: the renderer it draws and measures with follows the font.
  inline float setSystemTextScale(SceneCapture& capture, float scale) {
    const float given = setSystemTextScale(scale);
    capture.setTextScale(given);
    return given;
  }
} // namespace test
