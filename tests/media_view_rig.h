#pragma once

#include "media_clips.h"
#include "render/animation/animation_manager.h"
#include "render/core/image_decoder.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <string>
#include <variant>

// What the media view tests share: a view inside two holders, drawn offscreen one frame at a time, with a real
// input dispatcher.
namespace test::media {
  using luciole::AudioOutput;
  using luciole::MediaState;

  inline constexpr int kWidth = 320;
  inline constexpr int kHeight = 240;

  // Gives its children its whole size, as a pane gives its content.
  class Holder final : public Node {
    void doLayout(Renderer& renderer) override {
      for (const auto& child : children()) {
        child->arrange(renderer, {0, 0, width(), height()});
      }
    }
  };

  inline std::string tooltipOf(const Button& button) {
    const auto content = button.inputArea()->tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  inline std::shared_ptr<const LoadedImageFile> solidPoster(int width, int height) {
    auto poster = std::make_shared<LoadedImageFile>();
    poster->width = width;
    poster->height = height;
    poster->rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 200);
    return poster;
  }

  // A view inside two holders, drawn offscreen one frame at a time.
  struct ViewRig {
    ::test::SceneCapture& capture;
    AnimationManager animations;
    Holder root;
    Holder* middle = nullptr;
    MediaView* view = nullptr;
    InputDispatcher dispatcher;
    Pump pump;
    std::uint32_t serial = 0;
    int width = kWidth;
    int height = kHeight;

    explicit ViewRig(::test::SceneCapture& rig, AudioOutput audio = AudioOutput::Silent) : capture(rig) {
      root.setAnimationManager(&animations);
      middle = static_cast<Holder*>(root.addChild(std::make_unique<Holder>()));
      view = static_cast<MediaView*>(middle->addChild(std::make_unique<MediaView>(audio)));
      dispatcher.setSceneRoot(&root);
      frame();
    }

    // One displayed frame: the main context and the timers run, animations tick, then the scene lays out and draws.
    ::test::GlPixels frame() {
      pump.once();
      animations.tick(16);
      return capture.capture(root, width, height, 1);
    }
    // Draws frames until `done` holds or `limit` passed on the steady clock; whether it held.
    bool until(const std::function<bool()>& done, std::chrono::milliseconds limit = kWait) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      // A frame takes at least a poll, so the frames end with the deadline; the count bounds the loop as well.
      for (std::int64_t i = 0; i < limit.count() * 100 && std::chrono::steady_clock::now() < deadline; ++i) {
        if (done()) {
          return true;
        }
        frame();
      }
      return done();
    }
    MediaState state() const { return view->player() != nullptr ? view->player()->state() : MediaState::Idle; }
    double position() const { return view->player()->position(); }

    void press(float x, float y) {
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 1000, false);
    }
    void release(float x, float y) {
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
      frame();
    }
    // A click at a share of `node`'s width, across its middle.
    void click(const Node& node, float share = 0.5F) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      TEST_CHECK(node.width() > 0 && node.height() > 0);
      press(x + node.width() * share, y + node.height() / 2);
      release(x + node.width() * share, y + node.height() / 2);
    }
    void key(std::uint32_t sym) {
      dispatcher.keyEvent(sym, 0, 0, true);
      dispatcher.keyEvent(sym, 0, 0, false);
      frame();
    }
    void playing() {
      TEST_CHECK(until([this] { return state() == MediaState::Playing && view->showsFrame(); }));
    }
  };

  // Pixels inside `node`'s box that differ between two frames.
  inline int differing(const ::test::GlPixels& a, const ::test::GlPixels& b, const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    int count = 0;
    for (int row = static_cast<int>(y); row < static_cast<int>(y + node.height()); ++row) {
      for (int column = static_cast<int>(x); column < static_cast<int>(x + node.width()); ++column) {
        count += a.at(column, row) != b.at(column, row) ? 1 : 0;
      }
    }
    return count;
  }

  // Draws until a fade has finished.
  inline ::test::GlPixels drawn(ViewRig& rig) {
    for (int i = 0; i < 40; ++i) {
      (void)rig.frame();
    }
    return rig.frame();
  }

  // Takes the pointer away and draws until the hover has faded.
  inline ::test::GlPixels settled(ViewRig& rig) {
    rig.dispatcher.pointerLeave();
    return drawn(rig);
  }
} // namespace test::media
