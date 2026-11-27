#pragma once

#include "thumbnails/preview_service.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <string>
#include <vector>

class Label;
class Renderer;
class ScrollView;

namespace luciole {
  class FileIcon;
  class FileIconCache;

  // The page's own preview of a file in a Workbench column slot, below the slot's header: the rendered picture, or
  // the type icon while there is none, the preview's sentence, the text it read, and its facts under "Details". The
  // slot's header names the file, its kind, size and date and holds Open, so none of them repeat here. All its text
  // has one size, which the owner keeps below the list's. It scrolls when it is taller than the slot.
  class ColumnPreviewPage final : public Flex {
  public:
    // Facts past this many are left out.
    static constexpr std::size_t kMaxFacts = 32;

    explicit ColumnPreviewPage(FileIconCache& icons);
    ~ColumnPreviewPage() override;
    ColumnPreviewPage(const ColumnPreviewPage&) = delete;
    ColumnPreviewPage& operator=(const ColumnPreviewPage&) = delete;
    ColumnPreviewPage(ColumnPreviewPage&&) = delete;
    ColumnPreviewPage& operator=(ColumnPreviewPage&&) = delete;

    // Ignores sizes that are not positive and finite.
    void setTextSize(float size);
    // Shows `snapshot` in a slot `width` by `height` logical pixels, the picture boxed by its own shape and the
    // slot. Only the facts meant for a preview show.
    void sync(const PreviewSnapshot& snapshot, float width, float height);
    // Releases the picture's texture.
    void clear(Renderer& renderer);

    float textSize() const noexcept { return m_textSize; }

  private:
    struct Fact {
      Flex* row = nullptr;
      Label* key = nullptr;
      Label* value = nullptr;
    };

    void applyTextSize();

    FileIcon* m_image = nullptr;
    ScrollView* m_scroll = nullptr;
    Label* m_message = nullptr;
    Label* m_text = nullptr;
    Label* m_details = nullptr;
    std::vector<Fact> m_facts;
    std::string m_location; // the file shown, whose change scrolls back to the top
    float m_textSize;
  };
} // namespace luciole
