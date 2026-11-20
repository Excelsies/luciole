#pragma once

#include "config/visit_store.h"
#include "fs/location.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace luciole {
  // Records and reads visits on the main loop while a serial worker reads and writes a VisitStore. Visits are
  // written once recording has paused for `delay`, and no later than four times `delay` after the first unsaved
  // one; `flush` writes at once. A write that fails is reported in `warning` and its visits are dropped.
  class VisitService {
  public:
    static constexpr std::chrono::milliseconds defaultDelay{2000};
    explicit VisitService(std::filesystem::path directory, std::chrono::milliseconds delay = defaultDelay);
    // Unsaved visits are still written; the worker keeps its state alive until it finishes.
    ~VisitService();
    VisitService(const VisitService&) = delete;
    VisitService& operator=(const VisitService&) = delete;

    // Off drops unsaved visits; nothing is then recorded or written, and every read is empty.
    void setEnabled(bool enabled);
    bool enabled() const;
    // Counts one visit now, which reads see at once. The changed callback follows from the main loop, once for all
    // the visits recorded within about a frame. A location the store may not keep is ignored.
    void record(const Location& location);
    Visit visit(const Location& location) const;
    // The location and every location below it, ordered by URI.
    std::vector<VisitRecord> visitsUnder(const Location& location) const;
    // Forgets every visit, saved or not.
    void clear();
    void flush();
    // True once the saved visits have been read.
    bool ready() const;
    // True while a read or write runs or visits wait to be written.
    bool busy() const;
    // The first problem of the latest read, write or clear of the store; empty when that one had none.
    const std::string& warning() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
