#include "fs/location_within.h"
#include "test_check.h"

#include <print>

// Whether a location lies in a drive's root: a mount point is not a sibling that starts with the same letters, and
// the spelling of the root or of the location does not matter.
namespace {
  using luciole::Location;
  using luciole::locationWithin;

  void siblingWithTheSamePrefixIsOutside() {
    const auto usb = Location::fromPath("/mnt/usb");
    TEST_CHECK(!locationWithin(Location::fromPath("/mnt/usb2"), usb));
    TEST_CHECK(!locationWithin(Location::fromPath("/mnt/usb2/folder"), usb));
    TEST_CHECK(!locationWithin(Location::fromPath("/mnt/usb"), Location::fromPath("/mnt/usb2")));
    TEST_CHECK(!locationWithin(Location::fromPath("/mnt/us"), usb));
  }

  void theRootAndWhatIsBelowIt() {
    const auto usb = Location::fromPath("/mnt/usb");
    TEST_CHECK(locationWithin(usb, usb));
    TEST_CHECK(locationWithin(Location::fromPath("/mnt/usb/folder"), usb));
    TEST_CHECK(locationWithin(Location::fromPath("/mnt/usb/folder/file.txt"), usb));
    TEST_CHECK(!locationWithin(Location::fromPath("/mnt"), usb)); // the parent is not inside
    TEST_CHECK(locationWithin(usb, Location::fromPath("/")));
  }

  void aTrailingSlashChangesNothing() {
    const auto slashed = Location::fromPath("/mnt/usb/");
    TEST_CHECK(locationWithin(Location::fromPath("/mnt/usb"), slashed));
    TEST_CHECK(locationWithin(Location::fromPath("/mnt/usb/folder"), slashed));
    TEST_CHECK(!locationWithin(Location::fromPath("/mnt/usb2"), slashed));
  }

  void pathAndUriSpellingsAgree() {
    const auto usb = Location::fromPath("/mnt/usb");
    TEST_CHECK(locationWithin(Location::fromUri("file:///mnt/usb/folder"), usb));
    TEST_CHECK(locationWithin(Location::fromPath("/mnt/usb/folder"), Location::fromUri("file:///mnt/usb")));
    TEST_CHECK(locationWithin(Location::fromUri("file:///mnt/usb/folder"), Location::fromUri("file:///mnt/usb/")));
    TEST_CHECK(!locationWithin(Location::fromUri("file:///mnt/usb2/folder"), usb));
    // A name with a space is escaped the same way in both spellings.
    TEST_CHECK(locationWithin(Location::fromPath("/mnt/usb/my folder/a"), Location::fromUri("file:///mnt/usb")));
  }

  void anUnrelatedLocationIsOutside() {
    const auto usb = Location::fromPath("/mnt/usb");
    TEST_CHECK(!locationWithin(Location::fromPath("/home/user/file"), usb));
    TEST_CHECK(!locationWithin(Location::fromUri("sftp://host/mnt/usb/file"), usb));
    TEST_CHECK(!locationWithin(Location::trash(), usb));
  }
} // namespace

int main() {
  siblingWithTheSamePrefixIsOutside();
  theRootAndWhatIsBelowIt();
  aTrailingSlashChangesNothing();
  pathAndUriSpellingsAgree();
  anUnrelatedLocationIsOutside();
  std::println("location-within: all checks passed");
  return 0;
}
