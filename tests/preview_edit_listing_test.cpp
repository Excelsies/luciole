#include "app/page/preview_edit_listing.h"
#include "test_check.h"

#include <optional>
#include <print>

// What a listing says of an edited file: when it lists the saved file, which entry ids and identities count.
namespace {
  using luciole::DirectoryEntry;
  using luciole::EditedListing;
  using luciole::EntryPlace;
  using luciole::Location;
  using luciole::UserFileIdentity;

  constexpr std::uint64_t kDevice = 7;
  constexpr std::uint64_t kOldInode = 11;
  constexpr std::uint64_t kSavedInode = 12;

  DirectoryEntry entry(std::uint64_t id, std::optional<std::uint64_t> inode) {
    DirectoryEntry listed;
    listed.id = id;
    listed.place = EntryPlace(Location::fromPath("/notes/a.txt"));
    listed.name = "a.txt";
    if (inode) {
      listed.tags.identity = true;
      listed.tags.device = kDevice;
      listed.tags.inode = *inode;
    }
    return listed;
  }

  UserFileIdentity saved(std::uint64_t inode) { return {.device = kDevice, .inode = inode}; }

  // Another listing counts its entries from the start, so it gives the saved file the id the old one had: only the
  // identity says it is the saved file.
  void theSavedIdentityIsListedUnderTheSameId() {
    EditedListing listing(entry(5, kOldInode));
    const auto revision = saved(kOldInode);
    listing.saveStarted();
    TEST_CHECK(!listing.see(entry(5, kOldInode), revision, true).relisted);
    listing.saved(saved(kSavedInode));
    TEST_CHECK(listing.awaited());
    TEST_CHECK(listing.see(entry(5, kSavedInode), saved(kSavedInode), false).relisted);
    TEST_CHECK(!listing.awaited());
    TEST_CHECK(!listing.see(entry(5, kSavedInode), saved(kSavedInode), false).relisted); // reported once
  }

  // Positive match: the listing did not know the old file's identity, and the saved file's is still told apart.
  void anOldEntryWithoutIdentityDoesNotHideTheSavedOne() {
    EditedListing listing(entry(5, std::nullopt));
    listing.saveStarted();
    listing.saved(saved(kSavedInode));
    TEST_CHECK(!listing.see(entry(5, std::nullopt), saved(kOldInode), false).relisted);
    TEST_CHECK(listing.see(entry(5, kSavedInode), saved(kSavedInode), false).relisted);
  }

  // A file replaced by someone else is not the saved file: the listing does not claim to list the saved one.
  void anotherReplacementIsNotTheSavedFile() {
    EditedListing listing(entry(5, kOldInode));
    listing.saveStarted();
    listing.saved(saved(kSavedInode));
    TEST_CHECK(!listing.see(entry(5, 99), saved(kSavedInode), false).relisted);
    TEST_CHECK(listing.awaited());
  }

  // An entry that has no identity, as one from a provider that gives none: only a new entry id says it was listed
  // anew.
  void entriesWithoutIdentityAreToldApartById() {
    EditedListing listing(entry(5, std::nullopt));
    listing.saveStarted();
    listing.saved(saved(kSavedInode));
    TEST_CHECK(!listing.see(entry(5, std::nullopt), saved(kSavedInode), false).relisted);
    TEST_CHECK(listing.awaited());
    TEST_CHECK(listing.see(entry(6, std::nullopt), saved(kSavedInode), false).relisted);
    TEST_CHECK(!listing.awaited());
  }

  // A new id counts whatever the identity says.
  void aNewIdIsAlwaysRelisted() {
    EditedListing listing(entry(5, kOldInode));
    TEST_CHECK(listing.see(entry(6, kOldInode), saved(kOldInode), false).relisted);
    TEST_CHECK(!listing.see(entry(6, kOldInode), saved(kOldInode), false).relisted);
  }

  // A refused save replaced nothing, and the next save starts from nothing.
  void aRefusedSaveForgetsTheSavedFile() {
    EditedListing listing(entry(5, kOldInode));
    listing.saveStarted();
    listing.saveRefused();
    TEST_CHECK(!listing.awaited());
    TEST_CHECK(!listing.see(entry(5, kSavedInode), saved(kOldInode), false).relisted);
    listing.saveStarted();
    listing.saved(saved(kSavedInode));
    TEST_CHECK(listing.see(entry(5, kSavedInode), saved(kSavedInode), false).relisted);
  }

  // The listing listed the new file before the save's callback: the wait is over, so the callback arms nothing and
  // the next refresh does not report the same file again.
  void aRelistBeforeTheSaveCallbackIsReportedOnce() {
    EditedListing listing(entry(5, kOldInode));
    listing.saveStarted();
    TEST_CHECK(listing.see(entry(6, kSavedInode), saved(kOldInode), true).relisted);
    TEST_CHECK(!listing.awaited());
    listing.saved(saved(kSavedInode));
    TEST_CHECK(!listing.awaited());
    TEST_CHECK(!listing.see(entry(6, kSavedInode), saved(kSavedInode), false).relisted);
    TEST_CHECK(listing.see(entry(7, kSavedInode), saved(kSavedInode), false).relisted); // a new entry still counts
  }
} // namespace

int main() {
  theSavedIdentityIsListedUnderTheSameId();
  anOldEntryWithoutIdentityDoesNotHideTheSavedOne();
  anotherReplacementIsNotTheSavedFile();
  entriesWithoutIdentityAreToldApartById();
  aNewIdIsAlwaysRelisted();
  aRefusedSaveForgetsTheSavedFile();
  aRelistBeforeTheSaveCallbackIsReportedOnce();
  std::println("preview-edit-listing: all checks passed");
  return 0;
}
