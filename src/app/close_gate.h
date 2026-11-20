#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace luciole {
  // Asks the pages a close would take away before it happens: each page with unsaved changes in turn, in the order
  // given, then the close itself once. One request at a time.
  class CloseGate {
  public:
    // One page of a request. `unsaved` tells whether it holds unsaved changes now. `ask` opens its question with
    // the continuation the page calls once the user has answered; it returns true when the page may go at once
    // instead. Both are required.
    struct Page {
      std::function<bool()> unsaved;
      std::function<bool(std::function<void()> resume)> ask;
    };

    CloseGate() = default;
    CloseGate(const CloseGate&) = delete;
    CloseGate& operator=(const CloseGate&) = delete;
    CloseGate(CloseGate&&) = delete;
    CloseGate& operator=(CloseGate&&) = delete;
    ~CloseGate() = default;

    // Asks the first page that has unsaved changes and goes on when it resumes; runs `action` once when no page is
    // left to ask. Ignored while another request waits for an answer. A page that resumes but still has unsaved
    // changes, because its save failed, ends the request without the action. Throws std::invalid_argument when
    // `action` or a page's callback is empty. `abandoned`, when set, runs instead of the action when the request ends
    // without it: a page kept its changes after its answer, or another request was waiting.
    void request(std::vector<Page> pages, std::function<void()> action, std::function<void()> abandoned = {});

    // Whether the action of an answered request is running: a close it starts was agreed to and asks nothing again.
    bool acting() const { return m_acting; }

  private:
    void advance();
    void resume(std::uint64_t ask);
    void end();
    // Ends the request without its action and says so.
    void abandon();

    std::vector<Page> m_pages;
    std::function<void()> m_action;
    std::function<void()> m_abandoned;
    std::size_t m_next = 0;
    // Numbers each question, so a continuation answers only the question it was given with.
    std::uint64_t m_ask = 0;
    bool m_active = false;
    // The page at m_next was asked; finding it still unsaved after its answer ends the request.
    bool m_asked = false;
    bool m_asking = false;
    bool m_answered = false;
    bool m_waiting = false;
    bool m_acting = false;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  };
} // namespace luciole
