#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Button;
class InputArea;
class Label;

namespace luciole {
  // The edit bar under the header of a text preview.
  struct ColumnPreviewEditBar {
    bool shown = false;
    bool editing = false;         // Save, Stop Editing and the unsaved mark; otherwise Enable Editing
    bool enableAvailable = false; // Enable Editing can be pressed
    // Why Enable Editing cannot be pressed, its tooltip.
    std::string reason = {}; // NOLINT(readability-redundant-member-init): bars use designated initializers.
    bool saveAvailable = false;
    bool unsaved = false;
    // A sentence under the buttons; empty for none.
    std::string notice = {}; // NOLINT(readability-redundant-member-init)
    bool operator==(const ColumnPreviewEditBar&) const = default;
  };

  // The preview in a Workbench column slot: a header with the file's name, the size controls, an Open button and the
  // file's kind, a line of facts, then the content filling the rest, or a sentence in its place. It holds one content
  // node at a time and frees it when another takes its place. All its own text has one size, which the owner keeps
  // below the list's; the size controls choose the size the owner draws the content at.
  class ColumnPreviewView final : public Flex {
  public:
    ColumnPreviewView();
    ~ColumnPreviewView() override;
    ColumnPreviewView(const ColumnPreviewView&) = delete;
    ColumnPreviewView& operator=(const ColumnPreviewView&) = delete;
    ColumnPreviewView(ColumnPreviewView&&) = delete;
    ColumnPreviewView& operator=(ColumnPreviewView&&) = delete;

    // The file's name and its kind as the listing names it.
    void setFile(std::string_view name, std::string_view kind);
    // The kind as the content knows it, replacing the listing's until another file is set.
    void setKind(std::string_view kind);
    // Empty hides the line.
    void setFacts(std::string_view facts);
    // Ignores sizes that are not positive and finite.
    void setTextSize(float size);
    // Shows `content` below the facts and frees what was there; null leaves the area empty.
    void setContent(std::unique_ptr<Node> content);
    // The preview size in percent the controls show; the smaller and larger controls are disabled at the ends of
    // kPreviewSizes. A size off the steps shows as kDefaultPreviewSize.
    void setPreviewSize(int percent);
    // Enables the size controls, or disables them with `reason` as their tooltip.
    void setSizeAvailable(bool available, std::string_view reason = {});
    // Ctrl with the wheel turned `notches` steps, down positive, at scene point x, y: over the content, with the
    // controls enabled, a whole notch is one size step, down smaller; true when taken.
    bool wheelSize(float x, float y, double notches);
    // A sentence in place of the content, which is freed.
    void showMessage(std::string message);
    // No file: the header is emptied and the content freed. The edit bar stays as its owner set it.
    void clear();
    // The edit bar under the header and its buttons; a bar that is not shown takes no room.
    void setEditBar(const ColumnPreviewEditBar& bar);
    // While true, a click in the view that is no text field of its own, on a button enabled or dimmed, a label or the
    // space between them, leaves the keyboard focus where it was. The buttons still act. The owner sets it while an
    // editor of the content holds the keyboard.
    void setKeepsKeyboard(bool keeps);

    Node* content() const noexcept { return m_content; }
    const std::string& message() const noexcept { return m_message; }
    float textSize() const noexcept { return m_textSize; }
    int previewSize() const noexcept { return m_previewSize; }
    bool sizeAvailable() const noexcept { return m_sizeAvailable; }
    // The header and the line below it, in logical pixels as last laid out.
    float headerHeight() const;
    const Label& nameLabel() const noexcept { return *m_name; }
    const Label& kindLabel() const noexcept { return *m_kind; }
    const Label& factsLabel() const noexcept { return *m_facts; }
    const Label& messageLabel() const noexcept { return *m_messageLabel; }
    Button& openButton() const noexcept { return *m_open; }
    Button& smallerButton() const noexcept { return *m_smaller; }
    Button& sizeButton() const noexcept { return *m_size; }
    Button& largerButton() const noexcept { return *m_larger; }
    const ColumnPreviewEditBar& editBar() const noexcept { return m_bar; }
    Button& enableEditingButton() const noexcept { return *m_enable; }
    Button& saveButton() const noexcept { return *m_save; }
    Button& stopEditingButton() const noexcept { return *m_stop; }
    const Label& unsavedLabel() const noexcept { return *m_unsaved; }
    const Label& noticeLabel() const noexcept { return *m_notice; }
    bool keepsKeyboard() const noexcept { return m_keepsKeyboard; }

    // The Open button was pressed while a file is shown.
    std::function<void()> onOpen;
    // A control or the wheel chose a size in percent, one of kPreviewSizes; the view already shows it.
    std::function<void(int percent)> onPreviewSizeChosen;
    // The edit bar's buttons, each only while the bar lets it be pressed.
    std::function<void()> onEnableEditing;
    std::function<void()> onSave;
    std::function<void()> onStopEditing;

  private:
    std::unique_ptr<Node> makeNameRow();
    void dropContent();
    void applyTextSize();
    void choose(int percent);
    void applySizeControls();
    void applyEditBar();

    Label* m_name = nullptr;
    Button* m_open = nullptr;
    Button* m_smaller = nullptr;
    Button* m_size = nullptr;
    Button* m_larger = nullptr;
    Label* m_kind = nullptr;
    Label* m_facts = nullptr;
    Flex* m_editRow = nullptr;
    Button* m_enable = nullptr;
    Button* m_save = nullptr;
    Button* m_stop = nullptr;
    Label* m_unsaved = nullptr;
    Label* m_notice = nullptr;
    ColumnPreviewEditBar m_bar;
    InputArea* m_keeper = nullptr; // holds every part; takes the clicks no control inside it takes
    Flex* m_body = nullptr;
    Label* m_messageLabel = nullptr;
    Node* m_content = nullptr;
    std::string m_message;
    float m_textSize;
    int m_previewSize;
    double m_wheelRest = 0; // wheel notches not yet a whole step
    std::string m_sizeReason;
    bool m_sizeAvailable = false;
    bool m_keepsKeyboard = false;
  };
} // namespace luciole
