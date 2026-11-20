#include "config/visit_service.h"
#include "config/visit_store.h"
#include "event_loop/glib_poll_source.h"
#include "fs/location.h"
#include "test_check.h"
#include "visit_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <stdexcept>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using luciole::AtomicWriteState;
  using luciole::Location;
  using luciole::Visit;
  using luciole::VisitRecord;
  using luciole::VisitService;
  using luciole::VisitStore;
  using test::readFile;
  using test::stored;
  using test::storedCount;
  using test::writeFile;
  using Fixture = test::VisitFixture;

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 10 : std::min(requested, 10)) >= 0);
        m_source.dispatch(fds, start);
      }
    }
    void drainFor(std::chrono::milliseconds duration) {
      const auto deadline = std::chrono::steady_clock::now() + duration;
      until([&] { return std::chrono::steady_clock::now() >= deadline; });
    }

  private:
    GlibPollSource m_source;
  };

  void testRecordsAndReads() {
    Fixture fixture;
    Pump pump;
    const auto folder = Location::fromPath((fixture.root / "folder").native());
    const auto inside = Location::fromPath((fixture.root / "folder" / "inside").native());
    const auto deeper = Location::fromPath((fixture.root / "folder" / "inside" / "deeper").native());
    const auto sibling = Location::fromPath((fixture.root / "folder-sibling").native());
    {
      // Reading an absent store creates nothing.
      VisitService absent(fixture.store());
      pump.until([&] { return absent.ready() && !absent.busy(); });
      TEST_CHECK(absent.warning().empty() && !fs::exists(fixture.store()));
    }
    VisitStore seeded(fixture.store());
    seeded.load();
    const std::vector<VisitRecord> seed{{inside.uri(), {4, 40}}};
    TEST_CHECK(seeded.add(seed) == AtomicWriteState::Durable);

    VisitService service(fixture.store(), 30ms);
    int changes = 0;
    service.setChangedCallback([&] { ++changes; });
    TEST_CHECK(service.enabled() && service.busy());
    pump.until([&] { return service.ready() && !service.busy(); });
    TEST_CHECK(changes >= 1 && service.warning().empty());
    TEST_CHECK((service.visit(inside) == Visit{4, 40}));
    TEST_CHECK((service.visit(folder) == Visit{}));

    const auto before =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    service.record(folder);
    service.record(folder);
    service.record(deeper);
    service.record(sibling);
    service.record(Location::fromUri("https://example.com/page?token=1"));
    const auto after =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    // Reads see unsaved visits at once; the write waits for a quiet moment.
    TEST_CHECK(service.visit(folder).count == 2 && service.busy());
    TEST_CHECK(service.visit(folder).last >= before && service.visit(folder).last <= after);
    TEST_CHECK(!fs::exists(fixture.part(VisitStore::partOf(folder.uri()))));
    const auto under = service.visitsUnder(folder);
    TEST_CHECK(under.size() == 3);
    TEST_CHECK(under[0].uri == folder.uri() && under[0].visit.count == 2);
    TEST_CHECK((under[1].uri == inside.uri() && under[1].visit == Visit{4, 40}));
    TEST_CHECK(under[2].uri == deeper.uri() && under[2].visit.count == 1);
    TEST_CHECK(service.visitsUnder(sibling).size() == 1);
    TEST_CHECK(service.visitsUnder(Location::fromPath("/")).size() == 4);

    pump.until([&] { return !service.busy(); });
    VisitStore written(fixture.store());
    written.load();
    TEST_CHECK(written.issues().empty() && storedCount(written) == 4);
    TEST_CHECK(stored(written, folder.uri()).count == 2 && stored(written, deeper.uri()).count == 1);
    TEST_CHECK((stored(written, inside.uri()) == Visit{4, 40}));
    TEST_CHECK(service.visit(folder).count == 2 && service.visitsUnder(folder).size() == 3);

    // Visits recorded while a write runs are kept for the next one.
    service.record(folder);
    service.flush();
    TEST_CHECK(service.busy());
    service.record(folder);
    TEST_CHECK(service.visit(folder).count == 4);
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.visit(folder).count == 4);
    VisitStore final(fixture.store());
    final.load();
    TEST_CHECK(stored(final, folder.uri()).count == 4);
  }

  // Recording that never pauses for a whole delay is still written within four delays of the first visit.
  void testWriteCap() {
    Fixture fixture;
    Pump pump;
    const auto folder = Location::fromPath((fixture.root / "folder").native());
    const auto part = fixture.part(VisitStore::partOf(folder.uri()));
    VisitService service(fixture.store(), 100ms);
    pump.until([&] { return service.ready() && !service.busy(); });
    // One visit every quarter delay for eight delays.
    bool written = false;
    for (int index = 0; index < 32 && !written; ++index) {
      service.record(folder);
      pump.drainFor(25ms);
      written = fs::exists(part);
    }
    TEST_CHECK(written);
    pump.until([&] { return !service.busy(); });
  }

  // 256 distinct unsaved locations start a write without waiting for a pause.
  void testPendingLimit() {
    Fixture fixture;
    Pump pump;
    VisitService service(fixture.store(), 60s);
    pump.until([&] { return service.ready() && !service.busy(); });
    std::vector<Location> locations;
    locations.reserve(256);
    for (int index = 0; index < 256; ++index) {
      locations.push_back(Location::fromPath((fixture.root / std::format("n{}", index)).native()));
    }
    for (std::size_t index = 0; index + 1 < locations.size(); ++index) {
      service.record(locations[index]);
      service.record(locations[index]);
    }
    pump.drainFor(50ms);
    TEST_CHECK(service.busy() && !fs::exists(fixture.store()));
    service.record(locations.back());
    pump.until([&] { return !service.busy(); });
    VisitStore written(fixture.store());
    written.load();
    TEST_CHECK(written.issues().empty() && storedCount(written) == 256);
    TEST_CHECK(
        stored(written, locations.front().uri()).count == 2 && stored(written, locations.back().uri()).count == 1
    );
  }

  void testDisabledAndClear() {
    Fixture fixture;
    Pump pump;
    const auto folder = Location::fromPath((fixture.root / "folder").native());
    VisitStore seeded(fixture.store());
    seeded.load();
    const std::vector<VisitRecord> seed{{folder.uri(), {3, 30}}};
    TEST_CHECK(seeded.add(seed) == AtomicWriteState::Durable);
    const auto part = fixture.part(VisitStore::partOf(folder.uri()));
    const auto saved = readFile(part);

    VisitService service(fixture.store(), 10ms);
    pump.until([&] { return service.ready() && !service.busy(); });
    service.record(folder);
    service.setEnabled(false);
    // Off: nothing is recorded or written, and every read is empty.
    TEST_CHECK(!service.enabled() && !service.busy());
    TEST_CHECK((service.visit(folder) == Visit{}) && service.visitsUnder(folder).empty());
    service.record(folder);
    service.flush();
    pump.drainFor(60ms);
    TEST_CHECK(!service.busy() && readFile(part) == saved);
    service.setEnabled(true);
    TEST_CHECK((service.visit(folder) == Visit{3, 30}));

    service.record(folder);
    service.flush();
    service.clear();
    TEST_CHECK((service.visit(folder) == Visit{}) && service.visitsUnder(Location::fromPath("/")).empty());
    // The write that was running when the visits were cleared never brings them back.
    std::uint32_t seen = 0;
    service.setChangedCallback([&] { seen = std::max(seen, service.visit(folder).count); });
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(seen == 0 && (service.visit(folder) == Visit{}));
    service.setChangedCallback({});
    VisitStore cleared(fixture.store());
    cleared.load();
    TEST_CHECK(cleared.issues().empty() && storedCount(cleared) == 0);

    service.record(folder);
    pump.until([&] { return !service.busy(); });
    VisitStore after(fixture.store());
    after.load();
    TEST_CHECK(stored(after, folder.uri()).count == 1);
  }

  void testReportsDamage() {
    Fixture fixture;
    Pump pump;
    const auto folder = Location::fromPath((fixture.root / "folder").native());
    writeFile(fixture.part(VisitStore::partOf(folder.uri())), "luciole visits 1\nbroken\n");
    VisitService service(fixture.store(), 10ms);
    pump.until([&] { return service.ready() && !service.busy(); });
    TEST_CHECK(service.warning().contains(fixture.part(VisitStore::partOf(folder.uri())).string()));
    TEST_CHECK((service.visit(folder) == Visit{}));
    service.record(folder);
    pump.until([&] { return !service.busy(); });
    VisitStore repaired(fixture.store());
    repaired.load();
    TEST_CHECK(repaired.issues().empty() && stored(repaired, folder.uri()).count == 1);
  }

  // A failed write leaves a warning until a later write or clear succeeds.
  void testWarningClears() {
    Fixture fixture;
    Pump pump;
    const auto folder = Location::fromPath((fixture.root / "folder").native());
    const auto part = fixture.part(VisitStore::partOf(folder.uri()));
    VisitService service(fixture.store(), 10ms);
    pump.until([&] { return service.ready() && !service.busy(); });
    fs::create_directories(fixture.store());
    const auto block = [&] {
      fs::remove(part);
      fs::create_symlink(fixture.root / "elsewhere.tsv", part);
    };

    block();
    service.record(folder);
    service.flush();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.warning().contains("visits not saved"));
    fs::remove(part);
    service.record(folder);
    service.flush();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.warning().empty());

    block();
    service.record(folder);
    service.flush();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!service.warning().empty());
    fs::remove(part);
    service.clear();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.warning().empty());
    VisitStore cleared(fixture.store());
    cleared.load();
    TEST_CHECK(cleared.issues().empty() && storedCount(cleared) == 0);

    // A clear that fails is reported.
    block();
    service.clear();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.warning().contains("visits not cleared"));
  }

  // Recorded visits are announced long before they are written: a burst gives one notice once the main loop runs,
  // never from inside record, and a later visit gives another. Disabled, a visit gives none.
  void testRecordsNotifyOncePerBurst() {
    Fixture fixture;
    Pump pump;
    VisitService service(fixture.store(), 60s);
    int changes = 0;
    service.setChangedCallback([&] { ++changes; });
    pump.until([&] { return service.ready() && !service.busy(); });
    const int loaded = changes;
    for (int i = 0; i < 50; ++i) {
      service.record(Location::fromPath((fixture.root / std::format("f{}", i)).native()));
    }
    TEST_CHECK(changes == loaded);
    pump.until([&] { return changes > loaded; });
    TEST_CHECK(changes == loaded + 1);
    pump.drainFor(100ms);
    TEST_CHECK(changes == loaded + 1 && service.busy());
    service.record(Location::fromPath((fixture.root / "later").native()));
    pump.until([&] { return changes > loaded + 1; });
    TEST_CHECK(changes == loaded + 2);
    service.setEnabled(false);
    const int disabled = changes;
    service.record(Location::fromPath((fixture.root / "ignored").native()));
    pump.drainFor(100ms);
    TEST_CHECK(changes == disabled);
  }

  void testLifetime() {
    Fixture fixture;
    Pump pump;
    const auto folder = Location::fromPath((fixture.root / "folder").native());
    auto service = std::make_unique<VisitService>(fixture.store(), 10ms);
    int changes = 0;
    service->setChangedCallback([&] { ++changes; });
    service->record(folder);
    service->flush();
    // Work that is already queued finishes after its owner is gone, without calling back.
    service.reset();
    pump.drainFor(300ms);
    TEST_CHECK(changes == 0);
    VisitStore written(fixture.store());
    written.load();
    TEST_CHECK(stored(written, folder.uri()).count == 1);
  }
} // namespace

int main() {
  try {
    testRecordsAndReads();
    testWriteCap();
    testPendingLimit();
    testDisabledAndClear();
    testReportsDamage();
    testWarningClears();
    testRecordsNotifyOncePerBurst();
    testLifetime();
    std::println("visit service checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "visit service test: {}", error.what());
    return 1;
  }
}
