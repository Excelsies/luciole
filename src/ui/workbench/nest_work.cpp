#include "ui/workbench/nest_work.h"

#include "fs/background_job.h"
#include "fs/file_error.h"
#include "workbench/flame_row.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    constexpr double kTileNameFont = 12;

    using workbench::NestPeek;
    using workbench::NestScene;
    using workbench::NodeId;

    // Names from the top of the snapshot down to its current folder.
    std::vector<std::string> rootPath(const workbench::Tree& tree) {
      std::vector<std::string> names;
      for (const auto id : workbench::pathFromTop(tree, tree.root)) {
        names.push_back(tree.nodes[id].name);
      }
      return names;
    }

    // The core tile of the scene that shows an entry named `name`.
    std::optional<std::size_t> tileNamed(const workbench::Tree& tree, const NestScene& scene, std::string_view name) {
      for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
        const auto& piece = scene.pieces[i];
        if (piece.kind == workbench::NestPieceKind::Tile
            && workbench::hasNode(tree, piece.node)
            && tree.nodes[piece.node].name == name) {
          return i;
        }
      }
      return std::nullopt;
    }

    // Widths of names as the tiles draw them: semibold at the smallest label size.
    workbench::NestTextMeasure nameMeasure(const LabelMetrics& metrics) {
      return [font = metrics.font(kTileNameFont, workbench::LabelWeight::Semibold)](std::string_view text) {
        return workbench::labelTextWidth(text, font);
      };
    }

    struct LayoutJob {
      PartFrame frame;
      std::shared_ptr<const workbench::NestTextureSources> sources; // null when none fit the frame
      std::string keepName;                                         // empty for no peek to keep
      std::vector<std::string> shownPath;                           // the folder shown before; empty for none
    };
  } // namespace

  struct NestWork::LayoutReply {
    PartFrame frame;
    std::shared_ptr<const NestScene> scene;
    std::vector<std::vector<workbench::NestPatch>> texture;
    std::optional<NestPeek> peek;
    bool textured = false;   // every folder tile had its source
    bool sameFolder = false; // the scene shows the folder shown before
  };

  struct NestWork::TextureReply {
    std::shared_ptr<const workbench::Tree> tree;
    workbench::LayoutSettings settings;
    workbench::ColorPalette palette;
    std::shared_ptr<const NestScene> scene;
    std::shared_ptr<const workbench::NestTextureSources> sources;
    std::vector<std::vector<workbench::NestPatch>> texture;
  };

  NestWork::NestWork(Replies replies) : m_replies(std::move(replies)) {}

  NestWork::~NestWork() {
    m_textureToken.cancel();
    m_peekToken.cancel();
  }

  void NestWork::layout(const PartFrame& frame, NodeId keep) {
    if (frame.tree != m_frame.tree) {
      m_textureToken.cancel();
    }
    m_frame = frame;
    m_keep = keep;
    if (m_laying) {
      m_layAgain = true;
      return;
    }
    startLayout();
  }

  bool NestWork::sourcesFit(
      const std::shared_ptr<const workbench::Tree>& tree, const workbench::LayoutSettings& settings,
      const workbench::ColorPalette& palette
  ) const {
    return m_sources != nullptr
        && tree == m_sourcesTree
        && settings == m_sourcesSettings
        && samePalette(palette, m_sourcesPalette);
  }

  void NestWork::startLayout() {
    m_laying = true;
    m_layAgain = false;
    LayoutJob job{
        .frame = m_frame,
        .sources = sourcesFit(m_frame.tree, m_frame.settings, m_frame.palette) ? m_sources : nullptr,
        .keepName = {},
        .shownPath = {},
    };
    if (m_shown.tree != nullptr) {
      job.shownPath = rootPath(*m_shown.tree);
      if (workbench::hasNode(*m_shown.tree, m_keep)) {
        job.keepName = m_shown.tree->nodes[m_keep].name;
      }
    }
    runJob<LayoutReply>(
        [job = std::move(job)] {
          const auto& tree = *job.frame.tree;
          const auto& f = job.frame;
          auto scene = std::make_shared<const NestScene>(
              workbench::buildNestScene(tree, f.settings, f.width, f.height, f.palette, nameMeasure(f.metrics))
          );
          LayoutReply reply{
              .frame = f, .scene = nullptr, .texture = {}, .peek = std::nullopt, .textured = false, .sameFolder = false
          };
          const workbench::NestTextureSources none;
          reply.textured = workbench::missingNestSources(*scene, job.sources != nullptr ? *job.sources : none).empty();
          if (job.sources != nullptr) {
            reply.texture = workbench::nestTexturePatches(*scene, *job.sources);
          }
          reply.sameFolder = !job.shownPath.empty() && rootPath(tree) == job.shownPath;
          if (!job.keepName.empty() && reply.sameFolder) {
            if (const auto tile = tileNamed(tree, *scene, job.keepName)) {
              reply.peek = workbench::buildNestPeek(tree, f.settings, *scene, *tile, f.palette);
            }
          }
          reply.scene = std::move(scene);
          return reply;
        },
        [this, alive = std::weak_ptr<int>(m_lifetime)](JobResult<LayoutReply> result) {
          if (!alive.expired()) {
            finishLayout(std::move(result));
          }
        }
    );
  }

  void NestWork::finishLayout(JobResult<LayoutReply> result) {
    m_laying = false;
    if (!result) {
      if (m_layAgain) {
        startLayout();
      }
      return;
    }
    auto& reply = *result;
    m_shown = NestShown{
        .tree = reply.frame.tree,
        .serial = reply.frame.serial,
        .settings = reply.frame.settings,
        .palette = reply.frame.palette,
        .scene = std::move(reply.scene),
        .texture = std::move(reply.texture),
    };
    if (!reply.textured) {
      startTexture();
    }
    if (m_layAgain) {
      startLayout();
    }
    if (m_replies.laidOut) {
      m_replies.laidOut(std::move(reply.peek), reply.sameFolder);
    }
  }

  void NestWork::startTexture() {
    if (m_texturing) {
      m_textureAgain = true;
      return;
    }
    if (m_shown.scene == nullptr) {
      return;
    }
    m_texturing = true;
    m_textureAgain = false;
    m_textureToken = CancelToken{};
    const bool fit = sourcesFit(m_shown.tree, m_shown.settings, m_shown.palette);
    TextureReply job{
        .tree = m_shown.tree,
        .settings = m_shown.settings,
        .palette = m_shown.palette,
        .scene = m_shown.scene,
        .sources = fit ? m_sources : nullptr,
        .texture = {},
    };
    runJob<TextureReply>(
        m_textureToken,
        [job = std::move(job), token = m_textureToken] mutable {
          auto sources = job.sources != nullptr ? *job.sources : workbench::NestTextureSources{};
          // Each folder is sorted once; a newer snapshot stops the work between folders.
          for (const auto folder : workbench::missingNestSources(*job.scene, sources)) {
            if (token.cancelled()) {
              throwFileError(FileErrorKind::Cancelled, "Cancelled.");
            }
            sources.emplace(folder, workbench::nestTextureSource(*job.tree, job.settings, folder, job.palette));
          }
          job.texture = workbench::nestTexturePatches(*job.scene, sources);
          job.sources = std::make_shared<const workbench::NestTextureSources>(std::move(sources));
          return std::move(job);
        },
        [this, alive = std::weak_ptr<int>(m_lifetime)](JobResult<TextureReply> result) {
          if (!alive.expired()) {
            finishTexture(std::move(result));
          }
        }
    );
  }

  void NestWork::finishTexture(JobResult<TextureReply> result) {
    m_texturing = false;
    if (result
        && result->tree == m_shown.tree
        && result->settings == m_shown.settings
        && samePalette(result->palette, m_shown.palette)) {
      m_sources = std::move(result->sources);
      m_sourcesTree = result->tree;
      m_sourcesSettings = result->settings;
      m_sourcesPalette = result->palette;
      if (result->scene == m_shown.scene) {
        m_shown.texture = std::move(result->texture);
        if (m_replies.textured) {
          m_replies.textured();
        }
      } else {
        // The shown scene changed meanwhile: its patches come from the sources just built.
        m_textureAgain = true;
      }
    }
    if (m_textureAgain) {
      startTexture();
    }
  }

  void NestWork::peek(std::size_t piece) {
    dropPeek();
    if (m_shown.scene == nullptr || piece >= m_shown.scene->pieces.size()) {
      return;
    }
    m_peekToken = CancelToken{};
    runJob<std::optional<NestPeek>>(
        m_peekToken,
        [tree = m_shown.tree, settings = m_shown.settings, palette = m_shown.palette, scene = m_shown.scene, piece] {
          return workbench::buildNestPeek(*tree, settings, *scene, piece, palette);
        },
        [this, alive = std::weak_ptr<int>(m_lifetime), ask = m_peekAsk,
         scene = m_shown.scene](JobResult<std::optional<NestPeek>> result) {
          if (alive.expired() || ask != m_peekAsk || scene != m_shown.scene || !result || !result->has_value()) {
            return;
          }
          if (m_replies.peeked) {
            m_replies.peeked(std::move(**result));
          }
        }
    );
  }

  void NestWork::dropPeek() {
    m_peekToken.cancel();
    ++m_peekAsk;
  }
} // namespace luciole
