#include "config/visit_store.h"
#include "fs/location.h"
#include "test_check.h"
#include "visit_fixture.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::AtomicWriteState;
  using luciole::Location;
  using luciole::Visit;
  using luciole::VisitRecord;
  using luciole::VisitStore;
  using test::readFile;
  using test::stored;
  using test::storedCount;
  using test::urisInPart;
  using test::writeFile;
  using Fixture = test::VisitFixture;

  constexpr std::size_t kPrivateFileLimit = 256 * 1024;
  const std::string kHeader = "luciole visits 1\n";
  const std::string kGood = "file:///tmp/good";

  std::string uri(const fs::path& path) { return Location::fromPath(path.native()).uri(); }

  bool hasIssue(const VisitStore& store, const fs::path& file, std::string_view text) {
    return std::ranges::any_of(store.issues(), [&](const luciole::ConfigIssue& item) {
      return item.file == file && item.format().contains(text);
    });
  }

  void testUriRules() {
    TEST_CHECK(!luciole::visitUriProblem("file:///tmp/visit"));
    TEST_CHECK(!luciole::visitUriProblem("sftp://user@example.com/home/user"));
    TEST_CHECK(!luciole::visitUriProblem("trash:///"));
    TEST_CHECK(luciole::visitUriProblem(""));
    TEST_CHECK(luciole::visitUriProblem("relative/path"));
    TEST_CHECK(luciole::visitUriProblem("sftp://user:secret@example.com/home"));
    TEST_CHECK(luciole::visitUriProblem("https://example.com/page?token=1"));
    TEST_CHECK(luciole::visitUriProblem("file:///tmp/a#part"));
    TEST_CHECK(luciole::visitUriProblem("file:///tmp/a\tb"));
    TEST_CHECK(luciole::visitUriProblem("file:///tmp/a\nb"));
    TEST_CHECK(luciole::visitUriProblem(std::string("file:///tmp/a\0b", 15)));
    TEST_CHECK(!luciole::visitUriProblem("file:///" + std::string(4088, 'a')));
    TEST_CHECK(luciole::visitUriProblem("file:///" + std::string(4089, 'a')));
    // The part is the 64-bit FNV-1a hash of the URI modulo 32, so every process and version agrees on it.
    TEST_CHECK(VisitStore::partOf("file:///tmp/visit") == 19);
    TEST_CHECK(VisitStore::partOf(kGood) == 29);
    TEST_CHECK(VisitStore::partOf("file:///tmp/neighbor") == 18);
  }

  void testRoundTrip() {
    Fixture fixture;
    VisitStore missing(fixture.root / "absent" / "visits");
    missing.load();
    TEST_CHECK(missing.issues().empty() && storedCount(missing) == 0 && !fs::exists(fixture.root / "absent"));

    const auto a = uri(fixture.root / "a");
    const auto b = uri(fixture.root / "b");
    VisitStore store(fixture.store());
    store.load();
    TEST_CHECK(store.issues().empty());
    const std::vector<VisitRecord> first{{a, {1, 100}}, {b, {2, 200}}};
    TEST_CHECK(store.add(first) == AtomicWriteState::Durable);
    TEST_CHECK((stored(store, a) == Visit{1, 100}) && (stored(store, b) == Visit{2, 200}));
    const std::vector<VisitRecord> second{{a, {3, 50}}};
    TEST_CHECK(store.add(second) == AtomicWriteState::Durable);
    TEST_CHECK((stored(store, a) == Visit{4, 100}));

    VisitStore reloaded(fixture.store());
    reloaded.load();
    TEST_CHECK(reloaded.issues().empty() && storedCount(reloaded) == 2);
    TEST_CHECK((stored(reloaded, a) == Visit{4, 100}) && (stored(reloaded, b) == Visit{2, 200}));
    const auto text = readFile(fixture.part(VisitStore::partOf(a)));
    TEST_CHECK(text.starts_with(kHeader) && text.contains(std::format("4\t100\t{}\n", a)));
    const auto mode = fs::status(fixture.part(VisitStore::partOf(a))).permissions() & fs::perms::all;
    TEST_CHECK(mode == (fs::perms::owner_read | fs::perms::owner_write));

    // Counts saturate rather than wrap, and a time is never moved backward.
    const std::vector<VisitRecord> huge{{b, {0xFFFFFFFFU, 150}}};
    TEST_CHECK(reloaded.add(huge) == AtomicWriteState::Durable && (stored(reloaded, b) == Visit{0xFFFFFFFFU, 200}));

    // A location the store may not keep is refused before anything is written.
    const auto before = readFile(fixture.part(VisitStore::partOf(b)));
    const std::vector<VisitRecord> secret{{"sftp://user:secret@example.com/x", {1, 1}}, {b, {1, 300}}};
    TEST_CHECK(reloaded.add(secret) == AtomicWriteState::NotCommitted && !reloaded.issues().empty());
    TEST_CHECK(!reloaded.issues().front().format().contains("secret"));
    TEST_CHECK(readFile(fixture.part(VisitStore::partOf(b))) == before);
    const std::vector<VisitRecord> zero{{b, {0, 300}}};
    TEST_CHECK(reloaded.add(zero) == AtomicWriteState::NotCommitted);
    TEST_CHECK(reloaded.add({}) == AtomicWriteState::Unchanged);

    VisitStore relative(fs::path("relative") / "visits");
    TEST_CHECK(relative.add(first) == AtomicWriteState::NotCommitted && !relative.issues().empty());
    relative.load();
    TEST_CHECK(!relative.issues().empty() && storedCount(relative) == 0);
  }

  void testPartBounds() {
    Fixture fixture;
    VisitStore store(fixture.store());
    store.load();
    const auto base = uri(fixture.root);
    const auto crowded = urisInPart(3, VisitStore::partLimit + 75, base);
    std::vector<VisitRecord> records;
    records.reserve(crowded.size());
    for (std::size_t index = 0; index < crowded.size(); ++index) {
      records.push_back({crowded[index], {1, static_cast<std::int64_t>(1000 + index)}});
    }
    TEST_CHECK(store.add(records) == AtomicWriteState::Durable);
    TEST_CHECK(store.parts()[3].size() == VisitStore::partLimit);
    // The 75 least recently visited are gone; the rest stay.
    for (std::size_t index = 0; index < crowded.size(); ++index) {
      TEST_CHECK(store.parts()[3].contains(crowded[index]) == (index >= 75));
    }
    // A newer visit to a dropped location replaces the oldest kept one.
    const std::vector<VisitRecord> again{{crowded[0], {1, 5000}}};
    TEST_CHECK(store.add(again) == AtomicWriteState::Durable);
    TEST_CHECK(store.parts()[3].size() == VisitStore::partLimit);
    TEST_CHECK(store.parts()[3].contains(crowded[0]) && !store.parts()[3].contains(crowded[75]));

    // Long locations fill a part's file before its count does; the file stays within the private file limit.
    const auto longBase = base + "/" + std::string(3900, 'x');
    const auto wide = urisInPart(9, 120, longBase);
    records.clear();
    for (std::size_t index = 0; index < wide.size(); ++index) {
      records.push_back({wide[index], {1, static_cast<std::int64_t>(index)}});
    }
    TEST_CHECK(store.add(records) == AtomicWriteState::Durable);
    const auto kept = store.parts()[9].size();
    TEST_CHECK(kept > 50 && kept < wide.size());
    TEST_CHECK(readFile(fixture.part(9)).size() <= kPrivateFileLimit);
    for (std::size_t index = 0; index < wide.size(); ++index) {
      TEST_CHECK(store.parts()[9].contains(wide[index]) == (index >= wide.size() - kept));
    }

    VisitStore reloaded(fixture.store());
    reloaded.load();
    TEST_CHECK(reloaded.issues().empty() && reloaded.parts() == store.parts());
  }

  void testLocationLimit() {
    Fixture fixture;
    VisitStore store(fixture.store());
    store.load();
    const auto base = uri(fixture.root);
    std::vector<VisitRecord> records;
    records.reserve(VisitStore::locationLimit + 4000);
    for (std::size_t index = 0; index < VisitStore::locationLimit + 4000; ++index) {
      records.push_back({std::format("{}/{}", base, index), {1, static_cast<std::int64_t>(index)}});
    }
    TEST_CHECK(store.add(records) == AtomicWriteState::Durable);
    TEST_CHECK(VisitStore::locationLimit == 20000);
    TEST_CHECK(storedCount(store) <= VisitStore::locationLimit && storedCount(store) > 19000);
    for (std::size_t part = 0; part < VisitStore::partCount; ++part) {
      TEST_CHECK(store.parts()[part].size() <= VisitStore::partLimit);
      TEST_CHECK(readFile(fixture.part(part)).size() <= kPrivateFileLimit);
    }
    // The most recent visits are always kept.
    for (std::size_t index = records.size() - 100; index < records.size(); ++index) {
      TEST_CHECK(stored(store, records[index].uri).count == 1);
    }
  }

  void testDamagedParts() {
    const std::size_t goodPart = VisitStore::partOf(kGood);
    std::vector<std::string> damaged{
        "",
        "not a visit file\n",
        "luciole visits\n",
        "luciole visits two\n",
        "luciole visits 1 \n",
        kHeader + "2\t20\tfile:///tmp/good",
        kHeader + "0\t20\tfile:///tmp/good\n",
        kHeader + "-1\t20\tfile:///tmp/good\n",
        kHeader + "two\t20\tfile:///tmp/good\n",
        kHeader + "4294967296\t20\tfile:///tmp/good\n",
        kHeader + "2\t-20\tfile:///tmp/good\n",
        kHeader + "2\t99999999999999999999\tfile:///tmp/good\n",
        kHeader + "2\t20\n",
        kHeader + "2 20 file:///tmp/good\n",
        kHeader + "2\t20\tsftp://user:secret@example.com/x\n",
        kHeader + "2\t20\tfile:///tmp/good\n2\t20\tfile:///tmp/good\n",
        kHeader + "\n",
        std::string("luciole visits 1\0\n", 18),
    };
    // A location filed under the wrong part is damage too.
    for (std::size_t index = 0; index < 1000; ++index) {
      const auto other = std::format("file:///tmp/other-{}", index);
      if (VisitStore::partOf(other) != goodPart) {
        damaged.push_back(std::format("{}1\t1\t{}\n", kHeader, other));
        break;
      }
    }
    std::string crowded = kHeader;
    for (const auto& item : urisInPart(goodPart, VisitStore::partLimit + 1, "file:///tmp/crowd")) {
      crowded += std::format("1\t1\t{}\n", item);
    }
    damaged.push_back(crowded);
    for (const auto& text : damaged) {
      Fixture fixture;
      const std::string neighbor = "file:///tmp/neighbor";
      const auto neighborPart = VisitStore::partOf(neighbor);
      TEST_CHECK(neighborPart != goodPart);
      writeFile(fixture.part(neighborPart), std::format("{}5\t50\t{}\n", kHeader, neighbor));
      writeFile(fixture.part(goodPart), text);
      VisitStore store(fixture.store());
      store.load();
      // The damaged part starts empty and says so; the other parts are read as usual.
      TEST_CHECK(store.issues().size() == 1 && store.issues().front().file == fixture.part(goodPart));
      TEST_CHECK(store.parts()[goodPart].empty() && (stored(store, neighbor) == Visit{5, 50}));
      TEST_CHECK(readFile(fixture.part(goodPart)) == text);
      // The next visit in that part replaces the damaged file.
      const std::vector<VisitRecord> visit{{kGood, {1, 70}}};
      TEST_CHECK(store.add(visit) == AtomicWriteState::Durable);
      TEST_CHECK(readFile(fixture.part(goodPart)) == std::format("{}1\t70\t{}\n", kHeader, kGood));
    }

    Fixture valid;
    writeFile(valid.part(goodPart), std::format("{}2\t20\t{}\n", kHeader, kGood));
    VisitStore store(valid.store());
    store.load();
    TEST_CHECK(store.issues().empty() && (stored(store, kGood) == Visit{2, 20}));
  }

  // A file the store may not open stays as it is: every write to its part is refused and reported.
  void testUnwritableParts() {
    const std::size_t goodPart = VisitStore::partOf(kGood);
    const auto good = std::format("{}2\t20\t{}\n", kHeader, kGood);
    const std::vector<VisitRecord> visit{{kGood, {1, 70}}};

    Fixture linked;
    writeFile(linked.root / "elsewhere.tsv", good);
    fs::create_directories(linked.store());
    fs::create_symlink(linked.root / "elsewhere.tsv", linked.part(goodPart));
    VisitStore symlinked(linked.store());
    symlinked.load();
    TEST_CHECK(symlinked.issues().size() == 1 && symlinked.parts()[goodPart].empty());
    TEST_CHECK(symlinked.add(visit) == AtomicWriteState::NotCommitted);
    TEST_CHECK(hasIssue(symlinked, linked.part(goodPart), "visits not saved"));
    TEST_CHECK(readFile(linked.root / "elsewhere.tsv") == good && fs::is_symlink(linked.part(goodPart)));

    Fixture oversized;
    const auto large = kHeader + std::string(kPrivateFileLimit, 'x');
    writeFile(oversized.part(goodPart), large);
    VisitStore store(oversized.store());
    store.load();
    TEST_CHECK(store.issues().size() == 1 && store.parts()[goodPart].empty());
    for (int attempt = 0; attempt < 2; ++attempt) {
      TEST_CHECK(store.add(visit) == AtomicWriteState::NotCommitted);
      TEST_CHECK(hasIssue(store, oversized.part(goodPart), "256 KiB"));
      TEST_CHECK(store.parts()[goodPart].empty() && readFile(oversized.part(goodPart)) == large);
    }
  }

  // A file in another format version is kept for the version that wrote it.
  void testOtherVersion() {
    const std::size_t goodPart = VisitStore::partOf(kGood);
    for (const std::string header : {"luciole visits 2\n", "luciole visits 0\n", "luciole visits 4294967295\n"}) {
      Fixture fixture;
      const auto newer = std::format("{}2\t20\t{}\textra\n", header, kGood);
      writeFile(fixture.part(goodPart), newer);
      VisitStore store(fixture.store());
      store.load();
      TEST_CHECK(store.issues().size() == 1 && store.issues().front().format().contains("visit format"));
      TEST_CHECK(store.parts()[goodPart].empty());
      const std::vector<VisitRecord> visit{{kGood, {1, 70}}};
      TEST_CHECK(store.add(visit) == AtomicWriteState::NotCommitted);
      TEST_CHECK(hasIssue(store, fixture.part(goodPart), "visit format"));
      TEST_CHECK(store.clear() == AtomicWriteState::NotCommitted);
      TEST_CHECK(readFile(fixture.part(goodPart)) == newer);
    }
  }

  void testWritersMerge() {
    Fixture fixture;
    const auto a = uri(fixture.root / "a");
    VisitStore first(fixture.store());
    VisitStore second(fixture.store());
    first.load();
    second.load();
    const std::vector<VisitRecord> one{{a, {1, 10}}};
    const std::vector<VisitRecord> two{{a, {2, 30}}};
    TEST_CHECK(first.add(one) == AtomicWriteState::Durable);
    // The second writer read before the first saved; its save still keeps the first writer's visit.
    TEST_CHECK(second.add(two) == AtomicWriteState::Durable && (stored(second, a) == Visit{3, 30}));
    VisitStore third(fixture.store());
    third.load();
    TEST_CHECK((stored(third, a) == Visit{3, 30}));
  }

  void testClear() {
    Fixture fixture;
    VisitStore store(fixture.store());
    store.load();
    TEST_CHECK(store.clear() == AtomicWriteState::Unchanged);
    const auto a = uri(fixture.root / "a");
    const auto b = uri(fixture.root / "b");
    const std::vector<VisitRecord> visits{{a, {1, 10}}, {b, {1, 10}}};
    TEST_CHECK(store.add(visits) == AtomicWriteState::Durable);
    TEST_CHECK(store.clear() == AtomicWriteState::Durable && storedCount(store) == 0);
    VisitStore reloaded(fixture.store());
    reloaded.load();
    TEST_CHECK(reloaded.issues().empty() && storedCount(reloaded) == 0);
    TEST_CHECK(readFile(fixture.part(VisitStore::partOf(a))) == kHeader);
    TEST_CHECK(store.clear() == AtomicWriteState::Unchanged);

    // A part that cannot be rewritten is reported as not cleared; the others are cleared.
    const std::string neighbor = "file:///tmp/neighbor";
    TEST_CHECK(VisitStore::partOf(neighbor) != VisitStore::partOf(kGood));
    const std::vector<VisitRecord> fixed{{kGood, {1, 10}}, {neighbor, {1, 10}}};
    TEST_CHECK(store.add(fixed) == AtomicWriteState::Durable);
    const auto blocked = fixture.part(VisitStore::partOf(kGood));
    fs::remove(blocked);
    writeFile(fixture.root / "elsewhere.tsv", std::format("{}1\t10\t{}\n", kHeader, kGood));
    fs::create_symlink(fixture.root / "elsewhere.tsv", blocked);
    TEST_CHECK(store.clear() == AtomicWriteState::NotCommitted);
    TEST_CHECK(hasIssue(store, blocked, "visits not cleared"));
    TEST_CHECK((stored(store, neighbor) == Visit{}));
    TEST_CHECK(readFile(fixture.part(VisitStore::partOf(neighbor))) == kHeader);
  }
} // namespace

int main() {
  try {
    testUriRules();
    testRoundTrip();
    testPartBounds();
    testLocationLimit();
    testDamagedParts();
    testUnwritableParts();
    testOtherVersion();
    testWritersMerge();
    testClear();
    std::println("visit store checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "visit store test: {}", error.what());
    return 1;
  }
}
