#include "ui/workbench/column_preview_picture.h"

#include "model/thumbnail_options.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/animated_image_view.h"
#include "ui/workbench/column_preview_card.h"
#include "ui/workbench/column_preview_view.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace luciole {
  namespace {
    // Plays or shows the still; see startPictureLoad.
    class PictureLoad final : public ColumnPreviewLoad {
    public:
      PictureLoad(
          ColumnPreviewView& view, const ColumnPreviewRequest& request, ThumbnailService* thumbnails,
          std::function<void()> frameNeeded
      )
          : ColumnPreviewLoad(view, std::move(frameNeeded)), m_entry(request.entry), m_thumbnails(thumbnails),
            m_slotPixels(request.posterPixels), m_scale(request.scale),
            m_vector(request.entry.contentType.value_or("").starts_with("image/svg")) {
        auto made = std::make_unique<AnimatedImageView>(kColumnPreviewSurface);
        m_image = made.get();
        m_image->setPreviewScale(m_scale);
        m_view.setContent(std::move(made));
        if (request.kind != ColumnPreviewKind::Animation) {
          askStill();
          return;
        }
        m_image->onStateChanged = [this](AnimationState state) {
          if (state == AnimationState::Animated) {
            m_status = ColumnPreviewStatus::Shown;
            if (const auto [width, height] = m_image->sourceSize(); width > 0 && height > 0) {
              m_facts.pixels = std::pair(width, height);
            }
          } else if (state == AnimationState::Still) {
            askStill();
          } else if (state == AnimationState::Failed) {
            m_status = ColumnPreviewStatus::Still;
          }
          needFrame();
        };
        m_image->setFile(m_entry.location());
      }
      ~PictureLoad() override {
        m_image->onStateChanged = {};
        m_image->setShown(false);
      }
      PictureLoad(const PictureLoad&) = delete;
      PictureLoad& operator=(const PictureLoad&) = delete;
      PictureLoad(PictureLoad&&) = delete;
      PictureLoad& operator=(PictureLoad&&) = delete;

      void update() override { takeStill(); }
      void setSize(float /*textSize*/, float scale) override {
        m_scale = scale;
        m_image->setPreviewScale(scale);
        if (m_asked > 0 && sizeClass(wantedPixels()) > sizeClass(m_asked)) {
          askStill();
        }
      }

    private:
      static int sizeClass(int pixels) { return ThumbnailService::pixelSizeClass(pixels, ThumbnailPurpose::Preview); }

      // The slot's longest side at the scale, in buffer pixels.
      int wantedPixels() const {
        const double wanted = std::ceil(static_cast<double>(m_slotPixels) * m_scale);
        return static_cast<int>(std::clamp(std::isfinite(wanted) ? wanted : 1.0, 1.0, double{kMaxPreviewImageSide}));
      }

      void askStill() {
        if (m_thumbnails == nullptr) {
          m_status = ColumnPreviewStatus::Still;
          return;
        }
        m_asked = wantedPixels();
        m_pending = m_thumbnails->request(m_entry, m_asked, true, ThumbnailPurpose::Preview);
        if (!m_pending) {
          unreadable(); // refused, as for a file without a modification time or while the cache is cleared
          return;
        }
        takeStill();
      }

      // No still to show: the page's own preview takes the file, unless a still is on screen already.
      void unreadable() {
        if (m_status != ColumnPreviewStatus::Shown) {
          m_status = ColumnPreviewStatus::Still;
        }
        needFrame();
      }

      // The picture on screen stays until the one asked for arrives, which then frees it.
      void takeStill() {
        if (!m_pending || !m_pending->ready) {
          return;
        }
        const auto arrived = std::exchange(m_pending, nullptr);
        if (!arrived->pixels) {
          unreadable();
          return;
        }
        m_image->setStill(arrived->pixels);
        m_shownStill = arrived;
        // A vector picture is drawn at the size asked for, which is not a size of its own.
        if (const auto [width, height] = m_image->sourceSize(); width > 0 && height > 0 && !m_vector) {
          m_facts.pixels = std::pair(width, height);
        }
        m_status = ColumnPreviewStatus::Shown;
        needFrame();
      }

      DirectoryEntry m_entry;
      ThumbnailService* m_thumbnails;
      int m_slotPixels;
      float m_scale;
      int m_asked = 0; // the size the still was last asked for; 0 while the file plays or before
      std::shared_ptr<const FileIconImage> m_pending;
      std::shared_ptr<const FileIconImage> m_shownStill;
      AnimatedImageView* m_image = nullptr;
      bool m_vector;
    };
  } // namespace

  std::unique_ptr<ColumnPreviewLoad> startPictureLoad(
      ColumnPreviewView& view, const ColumnPreviewRequest& request, ThumbnailService* thumbnails,
      std::function<void()> frameNeeded
  ) {
    return std::make_unique<PictureLoad>(view, request, thumbnails, std::move(frameNeeded));
  }
} // namespace luciole
