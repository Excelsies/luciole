#pragma once

#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "ui/workbench/scene_part.h"
#include "workbench/color.h"
#include "workbench/measure.h"
#include "workbench/nest_scene.h"
#include "workbench/nest_texture.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace luciole {
  // The Nest scene shown, with what it was laid out from and the texture of its tiles.
  struct NestShown {
    std::shared_ptr<const workbench::Tree> tree; // null before the first layout
    std::uint64_t serial = 0;
    workbench::LayoutSettings settings;
    workbench::ColorPalette palette;
    std::shared_ptr<const workbench::NestScene> scene;
    std::vector<std::vector<workbench::NestPatch>> texture; // by piece index; empty until built
  };

  // Lays out the Nest, builds its texture and its peek cards on workers, so the main context only takes up their
  // results. One job of each kind runs at a time. A result for a superseded request, or one that arrives after the
  // owner is gone, is dropped.
  class NestWork {
  public:
    struct Replies {
      // A new scene is shown; with the peek of the folder `layout` was asked to keep, rebuilt for it, and whether it
      // shows the same current folder as the scene before.
      std::function<void(std::optional<workbench::NestPeek>, bool sameFolder)> laidOut;
      // The shown scene's texture changed.
      std::function<void()> textured;
      // The peek asked for is built.
      std::function<void(workbench::NestPeek)> peeked;
    };

    explicit NestWork(Replies replies);
    ~NestWork();
    NestWork(const NestWork&) = delete;
    NestWork& operator=(const NestWork&) = delete;
    NestWork(NestWork&&) = delete;
    NestWork& operator=(NestWork&&) = delete;

    // Lays `frame` out; while a layout runs, the latest frame waits for it. `keep` is a tile of the shown scene whose
    // open peek the new scene rebuilds, when it shows the same folder and that tile again; kNoNode for none. A new
    // snapshot cancels the texture of the one before.
    void layout(const PartFrame& frame, workbench::NodeId keep);
    // Builds the peek of the shown scene's piece `piece`, replacing the one asked before.
    void peek(std::size_t piece);
    // Forgets the peek asked for.
    void dropPeek();

    const NestShown& shown() const noexcept { return m_shown; }

  private:
    struct LayoutReply;
    struct TextureReply;

    void startLayout();
    void finishLayout(JobResult<LayoutReply> result);
    void startTexture();
    void finishTexture(JobResult<TextureReply> result);
    // Whether the texture sources were built for this snapshot, settings and palette.
    bool sourcesFit(
        const std::shared_ptr<const workbench::Tree>& tree, const workbench::LayoutSettings& settings,
        const workbench::ColorPalette& palette
    ) const;

    Replies m_replies;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    PartFrame m_frame; // the latest inputs
    workbench::NodeId m_keep = workbench::kNoNode;
    NestShown m_shown;
    // Texture sources for m_sourcesTree with the settings and palette shown.
    std::shared_ptr<const workbench::NestTextureSources> m_sources;
    std::shared_ptr<const workbench::Tree> m_sourcesTree;
    workbench::LayoutSettings m_sourcesSettings;
    workbench::ColorPalette m_sourcesPalette;
    CancelToken m_textureToken;
    CancelToken m_peekToken;
    std::uint64_t m_peekAsk = 0; // counts asks; a reply for an older one is dropped
    bool m_laying = false;
    bool m_layAgain = false;
    bool m_texturing = false;
    bool m_textureAgain = false;
  };
} // namespace luciole
