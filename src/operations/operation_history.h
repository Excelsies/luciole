#pragma once

#include "fs/cancel_token.h"
#include "fs/transfer_types.h"

#include <atomic>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace luciole {
  class CopyReceipt;
  class MoveReceipt;
  class HistoryRevisions;

  // Application Undo/Redo records operation metadata and owned-output proofs, never retained file bodies.
  // The worker receipts used to compose publication/recovery remain independent of this policy.
  class OperationHistory {
  public:
    struct Publication;
    explicit OperationHistory(
        TransferAction action, CancelToken token, std::shared_ptr<HistoryRevisions> revisions = {}
    );
    ~OperationHistory();
    OperationHistory(const OperationHistory&) = delete;
    OperationHistory& operator=(const OperationHistory&) = delete;

    std::shared_ptr<Publication> prepareNative(
        const Location& source, const Location& destination, int parent, const std::string& name, bool recursive = true
    );
    std::shared_ptr<Publication>
    prepareProvider(const Location& source, const Location& destination, const Location& staged);
    void published(const std::shared_ptr<Publication>& publication, bool exchanged = false);
    void beforeTransfer(const Location& source, const Location& destination);
    void sourceRemoved(const Location& source);
    void observed(const struct stat& value, bool removed);
    void beforeMerge(const Location& source);
    void removedMerge(const Location& source);
    void capture(
        const std::vector<TransferResult>& results, std::shared_ptr<const CopyReceipt>& copy,
        std::shared_ptr<const MoveReceipt>& move
    );
    bool owns(const Location& destination) const;
    std::vector<TransferResult> replayLink(const Location& source, const Location& destination);
    static bool cleanupPending();

  private:
    friend class OperationCopyHistory;
    friend class OperationMoveHistory;
    struct State;
    std::shared_ptr<State> m_state;
  };

  // Synchronous ordinary transfer execution used by history on its worker. Existing publication, provider,
  // cancellation and durability rules apply. Exact names preserve Keep Both destinations across replay.
  std::vector<TransferResult> runHistoryTransfer(
      const Location& source, const Location& destination, TransferAction action, OperationHistory& history,
      const CancelToken& token, std::atomic<std::uint64_t>& bytes
  );
} // namespace luciole
