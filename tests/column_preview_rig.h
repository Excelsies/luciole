#pragma once

#include "app/page/workbench_preview.h"
#include "column_preview_fixture.h"
#include "imaging/text_tile_service.h"
#include "render/animation/animation_manager.h"
#include "scene_capture.h"
#include "test_check.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/animated_image_view.h"
#include "ui/controls/table_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/workbench/column_preview_view.h"

#include <chrono>
#include <glib.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

// A column preview over a fixture folder of real files, drawn offscreen one frame at a time: the view, its owner
// and the workers a Workbench slot has.
namespace luciole::test::preview {
  using Clock = std::chrono::steady_clock;

  struct Rig {
    ::test::SceneCapture& capture;
    const luciole::test::preview::PreviewFiles& files;
    luciole::TextTileService text;
    luciole::ThumbnailService thumbnails;
    AnimationManager animations;
    luciole::BrowserController browser{{}, false};
    std::unique_ptr<luciole::ColumnPreviewView> view = std::make_unique<luciole::ColumnPreviewView>();
    std::unique_ptr<WorkbenchPreview> preview;

    Rig(::test::SceneCapture& rig, const luciole::test::preview::PreviewFiles& fixture) : capture(rig), files(fixture) {
      view->setAnimationManager(&animations);
      preview = std::make_unique<WorkbenchPreview>(
          *view, luciole::WorkbenchPreviewWorkers{.text = &text, .thumbnails = &thumbnails}, false,
          WorkbenchPreview::Hooks{}
      );
      browser.open(luciole::Location::fromPath(files.folder().native()));
      for (int round = 0; round < 4000 && browser.model().snapshot().status != luciole::DirectoryStatus::Ready;
           ++round) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(browser.model().snapshot().status == luciole::DirectoryStatus::Ready);
    }
    ~Rig() { preview.reset(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    static void pump() {
      for (int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE) != FALSE; ++i) {
      }
      TimerManager::instance().tick();
    }

    void show(std::string_view name) {
      const auto& model = browser.model();
      for (const auto id : model.rows()) {
        if (model.entry(id)->name == name) {
          preview->follow(
              luciole::WorkbenchPreviewTarget{
                  .previewed = {.entry = *model.entry(id), .typeLabel = std::string(model.typeLabel(id))},
                  .slot = 0,
                  .rested = true,
                  .posterPixels = 512,
              },
              true
          );
          return;
        }
      }
      TEST_CHECK(false);
    }

    ::test::GlPixels frame(int width, int height, float scale) {
      pump();
      animations.tick(16);
      preview->update();
      auto pixels = capture.capture(*view, width, height, scale);
      thumbnails.endFrame();
      return pixels;
    }

    bool shown() const {
      const auto* load = preview->load();
      if (load == nullptr || load->status() == ColumnPreviewStatus::Loading) {
        return false;
      }
      if (const auto* tiles = dynamic_cast<const TextTileView*>(view->content())) {
        return tiles->settled();
      }
      if (const auto* table = dynamic_cast<const luciole::TableView*>(view->content())) {
        return table->table() != nullptr;
      }
      if (const auto* animation = dynamic_cast<const AnimatedImageView*>(view->content())) {
        return animation->pictureShown();
      }
      return view->content() != nullptr;
    }

    // Frames until the content shows and a few more for the views that finish over two frames.
    ::test::GlPixels settle(int width, int height, float scale) {
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      while (!shown() && Clock::now() < deadline) {
        (void)frame(width, height, scale);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      TEST_CHECK(shown());
      ::test::GlPixels pixels;
      for (int i = 0; i < 4; ++i) {
        pixels = frame(width, height, scale);
      }
      return pixels;
    }
  };
} // namespace luciole::test::preview
