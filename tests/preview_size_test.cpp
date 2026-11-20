#include "config/settings.h"
#include "test_check.h"
#include "ui/workbench/workbench_preferences.h"

#include <array>
#include <limits>
#include <print>

// The preview size preference: its steps, its default, and how the preview's controls step along it.
namespace {
  void stepsAndDefault() {
    TEST_CHECK((luciole::kPreviewSizes == std::array{70, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300}));
    TEST_CHECK(luciole::kDefaultPreviewSize == 100);
    luciole::WorkbenchSettings settings;
    TEST_CHECK(luciole::workbenchPreviewSize(settings) == 100);
    for (const int step : {70, 125, 300}) {
      settings.previewSize = step;
      TEST_CHECK(luciole::workbenchPreviewSize(settings) == step);
    }
    for (const int other : {0, -100, 60, 95, 301, std::numeric_limits<int>::min()}) {
      settings.previewSize = other;
      TEST_CHECK(luciole::workbenchPreviewSize(settings) == 100);
    }
  }

  void stepping() {
    TEST_CHECK(luciole::steppedPreviewSize(100, 1) == 110);
    TEST_CHECK(luciole::steppedPreviewSize(100, -1) == 90);
    TEST_CHECK(luciole::steppedPreviewSize(110, 2) == 150);
    TEST_CHECK(luciole::steppedPreviewSize(70, -1) == 70); // held at the ends
    TEST_CHECK(luciole::steppedPreviewSize(300, 1) == 300);
    TEST_CHECK(luciole::steppedPreviewSize(80, 40) == 300);
    TEST_CHECK(luciole::steppedPreviewSize(250, std::numeric_limits<int>::min()) == 70);
    TEST_CHECK(luciole::steppedPreviewSize(250, std::numeric_limits<int>::max()) == 300);
    TEST_CHECK(luciole::steppedPreviewSize(150, 0) == 150);
    TEST_CHECK(luciole::steppedPreviewSize(95, 1) == 110); // a size off the steps counts as the default
    TEST_CHECK(luciole::steppedPreviewSize(95, 0) == 100);
  }
} // namespace

int main() {
  stepsAndDefault();
  stepping();
  std::println("preview-size: all checks passed");
  return 0;
}
