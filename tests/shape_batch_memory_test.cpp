#include "test_check.h"
#include "ui/controls/shape_batch_node.h"
#include "workbench/shape.h"

#include <cstddef>
#include <print>
#include <vector>

// The vertex memory of a shape batch follows the scene it shows: a big scene replaced by a small one gives its
// memory back, and a scene that stays about as big does not allocate again.
namespace {
  using namespace luciole::workbench;

  std::vector<Shape> rects(std::size_t count) {
    std::vector<Shape> shapes(count);
    for (std::size_t i = 0; i < count; ++i) {
      shapes[i].id = {.node = static_cast<NodeId>(i + 1), .role = 0};
      shapes[i].kind = ShapeKind::Rect;
      shapes[i].rect = {.x = static_cast<float>(i), .y = 0, .width = 4, .height = 4};
    }
    return shapes;
  }

  void aSmallSceneDropsTheMemoryOfABigOne() {
    ShapeBatchNode node;
    const auto big = rects(20000);
    node.setShapes(big, 1.0F);
    const std::size_t built = node.mesh().vertices.size();
    const std::size_t grown = node.mesh().vertices.capacity() * sizeof(ShapeVertex);
    TEST_CHECK(grown > 1'000'000);
    const auto small = rects(100);
    node.setShapes(small, 1.0F);
    TEST_CHECK(node.mesh().vertices.size() == 600);
    TEST_CHECK(node.mesh().vertices.capacity() * sizeof(ShapeVertex) < grown / 10);
    // The big scene is built again at once when it comes back.
    node.setShapes(big, 1.0F);
    TEST_CHECK(node.mesh().vertices.size() == built);
  }

  void aSceneOfAboutTheSameSizeKeepsItsBuffer() {
    ShapeBatchNode node;
    const auto first = rects(20000);
    node.setShapes(first, 1.0F);
    const auto* data = node.mesh().vertices.data();
    const auto second = rects(15000);
    node.setShapes(second, 1.0F);
    TEST_CHECK(node.mesh().vertices.data() == data);
  }
} // namespace

int main() {
  aSmallSceneDropsTheMemoryOfABigOne();
  aSceneOfAboutTheSameSizeKeepsItsBuffer();
  std::println("shape-batch-memory: all checks passed");
  return 0;
}
