#include "app/drive_removal.h"
#include "app/operation_hub.h"
#include "app/page/page_context.h"
#include "app/page/sidebar_controller.h"
#include "fs/drive_monitor.h"
#include "ui/browser/places_sidebar.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// Removing a drive from the sidebar: unsaved edits of its files are asked about, then the panes leave it, the
// removal is prepared and the drive is unmounted, ejected or powered off.

void SidebarController::removeDrive(const std::string& key) {
  const auto* drive = m_drives.find(key);
  if (drive == nullptr || !drive->root || m_removingDrive) {
    return;
  }
  const auto root = *drive->root;
  auto verb = std::string(luciole::driveRemovalLabel(drive->removal));
  std::ranges::transform(verb, verb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  m_removingDrive = true;
  // Unsaved edits of the drive's files are asked about first, while a Save can still reach the drive.
  m_page.jobs().leaveDriveThen(
      root,
      [this, key, root, verb = std::move(verb), lifetime = std::weak_ptr(m_lifetime)] {
        if (!lifetime.expired()) {
          m_page.jobs().leaveDrive(root);
          removeDriveWithoutAsking(key, root, std::move(verb));
        }
      },
      [this, lifetime = std::weak_ptr(m_lifetime)](luciole::CloseGate::Abandon reason) {
        if (lifetime.expired()) {
          return;
        }
        m_removingDrive = false;
        // A page that kept its changes has said why (its failed save, or the question was closed on purpose).
        if (reason == luciole::CloseGate::Abandon::Busy) {
          m_page.actionError("Answer the open question first, then try again.");
        }
      }
  );
}

void SidebarController::removeDriveWithoutAsking(
    const std::string& key, const luciole::Location& root, std::string verb
) {
  removeDriveAfterPreparation(
      [this, root, lifetime = std::weak_ptr(m_lifetime)](auto done) {
        if (lifetime.expired()) {
          done(luciole::FileError{luciole::FileErrorKind::Cancelled, "Drive removal cancelled."});
          return;
        }
        m_page.jobs().prepareDriveRemoval(root, std::move(done));
      },
      [this, root, lifetime = std::weak_ptr(m_lifetime)] {
        return lifetime.expired() || m_page.jobs().removalReady(root);
      },
      [this, key, root, lifetime = std::weak_ptr(m_lifetime)](auto done) {
        if (lifetime.expired()) {
          done({.error = "Drive removal cancelled.", .busy = false, .blockers = {}});
          return;
        }
        const auto* current = m_drives.find(key);
        if (!current || current->root != root) {
          done({.error = "This drive can no longer be removed.", .busy = false, .blockers = {}});
          return;
        }
        m_drives.remove(key, std::move(done));
      },
      [this, verb = std::move(verb), lifetime = std::weak_ptr(m_lifetime)](luciole::DriveRemoveResult result) {
        if (lifetime.expired()) {
          return;
        }
        m_removingDrive = false;
        if (result.error.empty()) {
          return;
        }
        if (!result.busy) {
          m_page.actionError(std::format("Could not {} the drive: {}", verb, result.error));
          return;
        }
        // Name who keeps it busy; for this process, its running file jobs say more than its name.
        std::vector<std::string> users;
        for (const auto& blocker : result.blockers) {
          if (blocker.pid == getpid()) {
            const auto jobs = m_page.runningJobTitles();
            users.insert(users.end(), jobs.begin(), jobs.end());
            if (jobs.empty()) {
              users.emplace_back("Luciole");
            }
          } else {
            users.push_back(blocker.name);
          }
        }
        std::ranges::sort(users);
        users.erase(std::ranges::unique(users).begin(), users.end());
        std::string names;
        for (std::size_t i = 0; i < users.size(); ++i) {
          names += i == 0 ? "" : (i + 1 == users.size() ? " and " : ", ");
          names += users[i];
        }
        m_page.actionError(
            names.empty() ? std::format("Could not {} the drive: files on it are still open.", verb)
                          : std::format("Could not {} the drive: it is in use by {}.", verb, names)
        );
      }
  );
}
