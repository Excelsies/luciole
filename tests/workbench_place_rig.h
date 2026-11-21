#pragma once

#include "app/page/workbench_preview.h"
#include "audio_visual_clips.h"
#include "browser/workbench_services.h"
#include "column_preview_fixture.h"
#include "imaging/text_tile_service.h"
#include "media/media_player.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/media_view.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/text_tile_view.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/pane_layout.h"
#include "workbench/preview_home.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// A Workbench page with the real loaders, laid out and drawn offscreen frame by frame as a page does, for the tests of
// where a preview is shown. Window sizes are those of real windows (see workbench_preview_home_test.cpp).
namespace luciole::test::place {
  using luciole::MediaState;
  using luciole::WorkbenchPreviewPlace;
  using luciole::workbench::PreviewHome;
  using Clock = std::chrono::steady_clock;
  namespace wb = luciole::workbench;

  struct Size {
    int width = 0;
    int height = 0;
  };
  inline constexpr Size kSmall{883, 606};
  inline constexpr Size kRoomy{1184, 806};
  inline constexpr Size kWide{1700, 806};

  struct Rig {
    ::test::SceneCapture& capture;
    luciole::WorkbenchServices services;
    luciole::TextTileService text;
    luciole::ThumbnailService thumbnails;
    luciole::FileIconCache icons;
    AnimationManager animations;
    luciole::BrowserController browser{{}, false};
    Node parent;
    Node* pane = nullptr;
    InputArea* focus = nullptr;
    std::unique_ptr<luciole::WorkbenchSession> session;
    std::unique_ptr<luciole::WorkbenchPreview> preview;
    Clock::duration skew{};
    std::vector<std::optional<luciole::ColumnPreview::Placement>> overlays;
    InputDispatcher dispatcher;
    std::uint32_t serial = 0;
    Size size = kSmall;
    WorkbenchPreviewPlace choice = WorkbenchPreviewPlace::JumpAheadWhenNarrow;

    Rig(const luciole::test::preview::PreviewFiles& files, ::test::SceneCapture& rig) : capture(rig) {
      parent.setAnimationManager(&animations);
      auto made = std::make_unique<Node>();
      made->setFlexGrow(1);
      focus = static_cast<InputArea*>(made->addChild(std::make_unique<InputArea>()));
      pane = parent.addChild(std::move(made));
      browser.open(luciole::Location::fromPath(files.folder().native()));
      until([this] { return browser.model().snapshot().status == luciole::DirectoryStatus::Ready; });
      session = std::make_unique<luciole::WorkbenchSession>(
          *pane, focus, services.measureCache(), icons, [] { return 0U; },
          luciole::WorkbenchModelOptions{.ancestorLevels = 0}
      );
      dispatcher.setSceneRoot(&session->view());
      preview = std::make_unique<luciole::WorkbenchPreview>(
          session->previewSlot(), luciole::WorkbenchPreviewWorkers{.text = &text, .thumbnails = &thumbnails},
          luciole::MediaPlayer::available(),
          luciole::WorkbenchPreview::Hooks{
              .frameNeeded = {},
              .open = [](const luciole::DirectoryEntry&) {},
              .overlay = [this](
                             const std::optional<luciole::ColumnPreview::Placement>& placement, float
                         ) { overlays.push_back(placement); },
              .previewSizeChosen = [](int) {},
              .clock = [this] { return Clock::now() + skew; },
          }
      );
    }
    ~Rig() {
      preview.reset();
      session.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void until(const std::function<bool()>& done) {
      for (std::size_t round = 0; round < 4000 && !done(); ++round) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(done());
    }
    static void pump() {
      for (int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE) != FALSE; ++i) {
      }
      TimerManager::instance().tick();
    }

    // One page frame, with the slot's bounds chosen the way the page chooses them for its home.
    void frame() {
      pump();
      animations.tick(16);
      luciole::WorkbenchChoices choices;
      choices.previewPlace = choice;
      session->update(luciole::workbenchFeed(browser), choices);
      preview->update();
      (void)capture.capture(session->view(), size.width, size.height, 1);
      auto& slot = session->previewSlot();
      std::optional<LayoutRect> bounds;
      if (session->shows(slot) && slot.width() > 0 && slot.height() > 0) {
        LayoutRect at{.x = 0, .y = 0, .width = slot.width(), .height = slot.height()};
        Node::absolutePosition(&slot, at.x, at.y);
        bounds = session->previewInJump() ? luciole::jumpPreviewBounds(at) : luciole::previewBounds(at);
      }
      preview->place(*session, browser.model(), bounds, 1);
      thumbnails.endFrame();
    }
    // Frames until `done` holds or `budget` ends.
    void frames(const std::function<bool()>& done, std::chrono::seconds budget = std::chrono::seconds(10)) {
      const auto start = Clock::now();
      while (!done() && Clock::now() - start < budget) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(done());
    }
    // A few frames: the home follows the layout the frame before it.
    void settle() {
      for (int i = 0; i < 4; ++i) {
        frame();
      }
    }
    void select(std::string_view name) {
      auto& model = browser.model();
      for (const auto id : model.rows()) {
        if (model.entry(id)->name == name) {
          TEST_CHECK(model.select(id));
          return;
        }
      }
      TEST_CHECK(false);
    }
    // The selected text file's content is drawn in the slot.
    void showsFile(std::string_view name) {
      skew += luciole::WorkbenchPreview::kDwell;
      frames([&] {
        const auto* tiles = dynamic_cast<TextTileView*>(session->previewSlot().content());
        return session->previewSlot().nameLabel().text() == name && tiles != nullptr && tiles->settled();
      });
    }

    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      frame();
    }

    void press(float x, float y) {
      dispatcher.pointerEnter(x, y, ++serial);
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      frame();
    }
    void drag(float x, float y) {
      dispatcher.pointerMotion(x, y, ++serial);
      frame();
    }
    void release(float x, float y) {
      (void)dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      frame();
    }

    luciole::WorkbenchView& view() const { return session->view(); }
    luciole::ColumnPreviewView& slot() const { return session->previewSlot(); }
    bool shownJump() const { return session->shows(view().jumpList()); }
    std::string jumpTitle() { return std::string(view().shell().header(wb::Pane::Jump).titleLabel()->text()); }
  };

  inline bool inside(const Node& node, const Node& ancestor) {
    for (const Node* at = node.parent(); at != nullptr; at = at->parent()) {
      if (at == &ancestor) {
        return true;
      }
    }
    return false;
  }

  inline float rightEdge(const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return x + node.width();
  }
} // namespace luciole::test::place
