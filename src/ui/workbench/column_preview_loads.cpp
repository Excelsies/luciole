#include "ui/workbench/column_preview_loads.h"

#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "imaging/text_document_loader.h"
#include "operations/archive_service.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/table_loader.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/media_view.h"
#include "ui/controls/mesh_view.h"
#include "ui/controls/table_view.h"
#include "ui/controls/text_tile_view.h"
#include "ui/workbench/column_preview_picture.h"
#include "ui/workbench/column_preview_view.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace luciole {
  namespace {
    constexpr std::string_view kLoading = "Loading…";
    constexpr std::string_view kArchiveUnreadable = "This archive could not be read.";
    constexpr std::string_view kModelUnreadable = "This model could not be read.";
    constexpr std::string_view kModelKind = "3D model";

    // A preview lists an archive within these, much tighter than extracting it.
    constexpr ArchiveLimits kArchiveLimits{.maxTime = std::chrono::seconds(15)};

    std::string contentTypeOf(const DirectoryEntry& entry) { return std::string(entry.contentType.value_or("")); }

    struct ArchiveListing {
      formats::TableGrid grid;
      std::size_t members = 0;
    };

    // Colored lines: text of every kind, and the hex dump of bytes that are not text.
    class TextLoad final : public ColumnPreviewLoad {
    public:
      TextLoad(
          ColumnPreviewView& view, const ColumnPreviewRequest& request, TextTileService& service,
          std::function<void()> frameNeeded
      )
          : ColumnPreviewLoad(view, std::move(frameNeeded)) {
        auto tiles = std::make_unique<TextTileView>(service);
        m_tiles = tiles.get();
        m_tiles->setTextSize(request.textSize * request.scale);
        m_tiles->onFrameNeeded = [this] { needFrame(); };
        m_tiles->showLoading();
        m_view.setContent(std::move(tiles));
        startTextDocumentLoad(
            request.entry.location(), request.entry.name, contentTypeOf(request.entry), m_token,
            [this, alive = lifetime()](const TextDocument& document) {
              if (!alive.expired() && document.state != TextDocumentState::Cancelled) {
                shown(document);
              }
            }
        );
      }
      ~TextLoad() override {
        m_token.cancel();
        m_tiles->onFrameNeeded = {};
        m_tiles->setShown(false);
      }
      TextLoad(const TextLoad&) = delete;
      TextLoad& operator=(const TextLoad&) = delete;
      TextLoad(TextLoad&&) = delete;
      TextLoad& operator=(TextLoad&&) = delete;

      void update() override { m_tiles->update(); }
      void setSize(float textSize, float scale) override { m_tiles->setTextSize(textSize * scale); }

    private:
      void shown(const TextDocument& document) {
        m_tiles->show(document);
        const bool text = document.state == TextDocumentState::Shown || document.state == TextDocumentState::Cut;
        if (document.state == TextDocumentState::Shown) {
          m_facts.lines = document.facts.lines; // a cut text counts only the part read
        }
        const bool words =
            text || document.state == TextDocumentState::NotText || document.state == TextDocumentState::PrivateKey;
        m_status = words ? ColumnPreviewStatus::Shown : ColumnPreviewStatus::Failed;
        m_showsText = text || document.state == TextDocumentState::PrivateKey;
        needFrame();
      }

      CancelToken m_token;
      TextTileView* m_tiles = nullptr;
    };

    // The table view over a TableGrid; the owner fills it.
    class GridLoad : public ColumnPreviewLoad {
    public:
      ~GridLoad() override {
        if (m_table != nullptr) {
          m_table->onFrameNeeded = {};
          m_table->onSheetChosen = {};
        }
      }
      GridLoad(const GridLoad&) = delete;
      GridLoad& operator=(const GridLoad&) = delete;
      GridLoad(GridLoad&&) = delete;
      GridLoad& operator=(GridLoad&&) = delete;

      void update() override {
        if (m_table != nullptr) {
          m_table->update();
        }
      }
      void setSize(float textSize, float scale) override {
        m_textSize = textSize * scale;
        if (m_table != nullptr) {
          m_table->setTextSize(m_textSize);
        }
      }

    protected:
      GridLoad(ColumnPreviewView& view, const ColumnPreviewRequest& request, std::function<void()> frameNeeded)
          : ColumnPreviewLoad(view, std::move(frameNeeded)), m_textSize(request.textSize * request.scale) {
        m_view.showMessage(std::string(kLoading));
      }

      TableView& table() {
        if (m_table == nullptr) {
          auto made = std::make_unique<TableView>();
          m_table = made.get();
          m_table->setTextSize(m_textSize);
          m_table->onFrameNeeded = [this] { needFrame(); };
          m_view.setContent(std::move(made));
        }
        return *m_table;
      }
      bool hasTable() const noexcept { return m_table != nullptr; }

    private:
      TableView* m_table = nullptr;
      float m_textSize;
    };

    // CSV, TSV, XLSX and ODS; a workbook reloads the sheet picked in its sheet list.
    class TableLoad final : public GridLoad {
    public:
      TableLoad(ColumnPreviewView& view, const ColumnPreviewRequest& request, std::function<void()> frameNeeded)
          : GridLoad(view, request, std::move(frameNeeded)), m_request{
                                                                 .location = request.entry.location(),
                                                                 .name = request.entry.name,
                                                                 .contentType = contentTypeOf(request.entry),
                                                                 .sheetIndex = 0
                                                             } {
        load(0);
      }
      ~TableLoad() override { m_token.cancel(); }
      TableLoad(const TableLoad&) = delete;
      TableLoad& operator=(const TableLoad&) = delete;
      TableLoad(TableLoad&&) = delete;
      TableLoad& operator=(TableLoad&&) = delete;

    private:
      void load(std::size_t sheet) {
        m_token.cancel();
        m_token = CancelToken{};
        m_request.sheetIndex = sheet;
        startTableLoad(m_request, m_token, [this, alive = lifetime(), token = m_token](TableResult result) {
          if (!alive.expired() && !token.cancelled()) {
            arrived(std::move(result));
          }
        });
      }

      void arrived(TableResult result) {
        if (result && *result) {
          auto& view = table();
          view.onSheetChosen = [this](std::size_t sheet) { load(sheet); };
          if (!(*result)->rowsCut) {
            m_facts.cells = std::pair((*result)->rowCount, (*result)->columnCount); // a cut grid counts its own rows
          } else {
            m_facts.cells.reset();
          }
          view.setTable(std::move(*result));
          m_status = ColumnPreviewStatus::Shown;
          m_showsTable = true;
        } else if (hasTable()) {
          table().setMessage(result ? std::string("This table could not be read.") : result.error());
        } else {
          fail(result ? std::string("This table could not be read.") : result.error());
        }
        needFrame();
      }

      TableRequest m_request;
      CancelToken m_token;
    };

    // The archive's catalog as a table of its entries.
    class ArchiveLoad final : public GridLoad {
    public:
      ArchiveLoad(ColumnPreviewView& view, const ColumnPreviewRequest& request, std::function<void()> frameNeeded)
          : GridLoad(view, request, std::move(frameNeeded)),
            m_service(std::make_unique<ArchiveService>(kArchiveLimits)) {
        m_service->setChangedCallback([this] {
          m_changed = true;
          needFrame();
        });
        if (!m_service->inspect(request.entry.location())) {
          m_service.reset();
          fail(std::string(kArchiveUnreadable));
        }
      }
      ~ArchiveLoad() override { m_token.cancel(); }
      ArchiveLoad(const ArchiveLoad&) = delete;
      ArchiveLoad& operator=(const ArchiveLoad&) = delete;
      ArchiveLoad(ArchiveLoad&&) = delete;
      ArchiveLoad& operator=(ArchiveLoad&&) = delete;

      void update() override {
        if (m_service && std::exchange(m_changed, false)) {
          read();
        }
        GridLoad::update();
      }

    private:
      // The service is let go here, never inside its own callback.
      void read() {
        if (m_service->phase() == ArchivePhase::WaitingPassword) {
          m_service.reset();
          fail("This archive asks for a password, so its entries are not listed.");
          return;
        }
        if (m_service->busy()) {
          return;
        }
        auto catalog = m_service->catalog();
        const auto error = m_service->error();
        m_service.reset();
        if (!catalog) {
          fail(error ? archiveRefusal(*error) : std::string(kArchiveUnreadable));
          return;
        }
        runJob<ArchiveListing>(
            m_token, [catalog] { return ArchiveListing{archiveGrid(*catalog), archiveMemberCount(*catalog)}; },
            [this, alive = lifetime()](JobResult<ArchiveListing> listing) {
              if (alive.expired()) {
                return;
              }
              if (!listing) {
                fail(std::string(kArchiveUnreadable));
              } else {
                m_facts.entries = listing->members;
                table().setTable(std::make_shared<const formats::TableGrid>(std::move(listing->grid)));
                m_status = ColumnPreviewStatus::Shown;
              }
              needFrame();
            }
        );
      }

      std::unique_ptr<ArchiveService> m_service;
      CancelToken m_token;
      bool m_changed = false;
    };

    // A model read in the helper, shown in the orbit view.
    class MeshLoad final : public ColumnPreviewLoad {
    public:
      MeshLoad(ColumnPreviewView& view, const ColumnPreviewRequest& request, std::function<void()> frameNeeded)
          : ColumnPreviewLoad(view, std::move(frameNeeded)), m_scale(request.scale) {
        m_view.showMessage(std::string(kLoading));
        runJob<MeshPreview>(
            m_token,
            [location = request.entry.location(), token = m_token] { return loadMeshPreview(location, token); },
            [this, alive = lifetime()](JobResult<MeshPreview> mesh) {
              if (!alive.expired()) {
                arrived(std::move(mesh));
              }
            }
        );
      }
      ~MeshLoad() override { m_token.cancel(); }
      MeshLoad(const MeshLoad&) = delete;
      MeshLoad& operator=(const MeshLoad&) = delete;
      MeshLoad(MeshLoad&&) = delete;
      MeshLoad& operator=(MeshLoad&&) = delete;

      void setSize(float /*textSize*/, float scale) override {
        m_scale = scale;
        if (m_mesh != nullptr) {
          m_mesh->setPreviewScale(scale);
        }
      }

    private:
      void arrived(JobResult<MeshPreview> mesh) {
        if (!mesh) {
          fail(std::string(kModelUnreadable));
        } else if (!*mesh) {
          fail(mesh->error());
        } else {
          auto made = std::make_unique<MeshView>();
          m_mesh = made.get();
          made->setGeometry(**mesh);
          made->setPreviewScale(m_scale);
          m_facts.triangles = (**mesh)->triangleCount();
          m_view.setContent(std::move(made));
          m_view.setKind(kModelKind);
          m_status = ColumnPreviewStatus::Shown;
        }
        needFrame();
      }

      CancelToken m_token;
      MeshView* m_mesh = nullptr;
      float m_scale;
    };

    // Playback controls over the still, which stands as the poster until Play.
    class MediaLoad final : public ColumnPreviewLoad {
    public:
      MediaLoad(
          ColumnPreviewView& view, const ColumnPreviewRequest& request, ThumbnailService* thumbnails,
          std::function<void()> frameNeeded
      )
          : ColumnPreviewLoad(view, std::move(frameNeeded)) {
        auto made = std::make_unique<MediaView>();
        m_media = made.get();
        m_media->onFactsChanged = [this] {
          factsChanged();
          needFrame();
        };
        m_media->setSource(request.entry.location().nativePath().value_or(""));
        m_media->setTextSize(request.textSize);
        m_media->audioVisual().setChoice(request.audioVisual);
        m_media->audioVisual().setModifiers(request.modifiers);
        m_media->audioVisual().onChosen = request.audioVisualChosen;
        m_media->setLoop(request.mediaLoop);
        m_media->onLoopChosen = request.mediaLoopChosen;
        m_view.setContent(std::move(made));
        if (thumbnails != nullptr) {
          m_poster = thumbnails->request(request.entry, request.posterPixels, true, ThumbnailPurpose::Preview);
        }
        m_status = ColumnPreviewStatus::Shown;
        takePoster();
      }
      ~MediaLoad() override {
        m_media->onFactsChanged = {};
        m_media->audioVisual().onChosen = {};
        m_media->onLoopChosen = {};
        m_media->setShown(false);
      }
      MediaLoad(const MediaLoad&) = delete;
      MediaLoad& operator=(const MediaLoad&) = delete;
      MediaLoad(MediaLoad&&) = delete;
      MediaLoad& operator=(MediaLoad&&) = delete;

      void update() override { takePoster(); }
      void setSize(float textSize, float /*scale*/) override { m_media->setTextSize(textSize); }
      void setAudioVisual(const AudioVisualChoice& choice) override { m_media->audioVisual().setChoice(choice); }
      void setMediaLoop(bool loop) override { m_media->setLoop(loop); }

    private:
      // The playing time and picture size as soon as the container or the player states them.
      void factsChanged() {
        const auto& facts = m_media->facts();
        if (facts.seconds > 0) {
          m_facts.seconds = facts.seconds;
        }
        if (facts.video && facts.width > 0 && facts.height > 0) {
          m_facts.pixels = std::pair(facts.width, facts.height);
        }
      }

      void takePoster() {
        if (m_poster && m_poster->ready) {
          if (m_poster->pixels) {
            m_media->setPoster(m_poster->pixels);
          }
          m_poster.reset();
        }
      }

      MediaView* m_media = nullptr;
      std::shared_ptr<const FileIconImage> m_poster;
    };
  } // namespace

  ColumnPreviewLoad::ColumnPreviewLoad(ColumnPreviewView& view, std::function<void()> frameNeeded)
      : m_view(view), m_frameNeeded(std::move(frameNeeded)) {}

  ColumnPreviewLoad::~ColumnPreviewLoad() { m_view.setContent(nullptr); }

  void ColumnPreviewLoad::fail(std::string message) {
    m_view.showMessage(std::move(message));
    m_status = ColumnPreviewStatus::Failed;
  }

  void ColumnPreviewLoad::needFrame() const {
    if (m_frameNeeded) {
      m_frameNeeded();
    }
  }

  std::unique_ptr<ColumnPreviewLoad> startColumnPreviewLoad(
      ColumnPreviewView& view, const ColumnPreviewRequest& request, const WorkbenchPreviewWorkers& workers,
      std::function<void()> frameNeeded
  ) {
    switch (request.kind) {
    case ColumnPreviewKind::Text:
    case ColumnPreviewKind::Binary:
      if (workers.text == nullptr) {
        throw std::invalid_argument("a text preview needs the text tile service");
      }
      return std::make_unique<TextLoad>(view, request, *workers.text, std::move(frameNeeded));
    case ColumnPreviewKind::Table:
      return std::make_unique<TableLoad>(view, request, std::move(frameNeeded));
    case ColumnPreviewKind::Archive:
      return std::make_unique<ArchiveLoad>(view, request, std::move(frameNeeded));
    case ColumnPreviewKind::Mesh:
      return std::make_unique<MeshLoad>(view, request, std::move(frameNeeded));
    case ColumnPreviewKind::Animation:
    case ColumnPreviewKind::Picture:
      return startPictureLoad(view, request, workers.thumbnails, std::move(frameNeeded));
    case ColumnPreviewKind::Media:
      return std::make_unique<MediaLoad>(view, request, workers.thumbnails, std::move(frameNeeded));
    case ColumnPreviewKind::Still:
      return nullptr;
    }
    return nullptr;
  }
} // namespace luciole
