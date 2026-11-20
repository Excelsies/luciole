#include "app/close_gate.h"
#include "test_check.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  // A page as the gate sees it: whether it holds unsaved changes, and how it answers being asked.
  struct FakePage {
    std::string name;
    bool unsaved = false;
    // `ask` returns true: the page may go at once.
    bool goesAtOnce = false;
    // The page answers inside `ask`, before it returns.
    bool answersAtOnce = false;
    // What answering does to the page: false keeps its changes, as a failed save does.
    bool answerSaves = true;
    int asks = 0;
    std::function<void()> resume;
  };

  struct Log {
    std::vector<std::string> asked;
    int actions = 0;
  };

  luciole::CloseGate::Page pageOf(const std::shared_ptr<FakePage>& page, Log& log) {
    return {
        .unsaved = [page] { return page->unsaved; },
        .ask =
            [page, &log](std::function<void()> resume) {
              ++page->asks;
              log.asked.push_back(page->name);
              if (page->answersAtOnce) {
                page->unsaved = !page->answerSaves;
                resume();
                return false;
              }
              page->resume = std::move(resume);
              return page->goesAtOnce;
            },
    };
  }

  // The user answers the page's open question.
  void answer(FakePage& page) {
    page.unsaved = !page.answerSaves;
    auto resume = std::exchange(page.resume, {});
    TEST_CHECK(static_cast<bool>(resume));
    resume();
  }

  std::shared_ptr<FakePage> fake(std::string name, bool unsaved) {
    auto page = std::make_shared<FakePage>();
    page->name = std::move(name);
    page->unsaved = unsaved;
    return page;
  }

  void nothingUnsavedRunsAtOnce() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", false);
    auto b = fake("b", false);
    gate.request({pageOf(a, log), pageOf(b, log)}, [&log] { ++log.actions; });
    TEST_CHECK(log.actions == 1);
    TEST_CHECK(log.asked.empty());
    gate.request({}, [&log] { ++log.actions; });
    TEST_CHECK(log.actions == 2);
  }

  void asksInOrderOneAtATime() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", false);
    auto c = fake("c", true);
    gate.request({pageOf(a, log), pageOf(b, log), pageOf(c, log)}, [&log] { ++log.actions; });
    TEST_CHECK((log.asked == std::vector<std::string>{"a"}));
    TEST_CHECK(log.actions == 0);
    answer(*a);
    TEST_CHECK((log.asked == std::vector<std::string>{"a", "c"}));
    TEST_CHECK(log.actions == 0);
    TEST_CHECK(b->asks == 0);
    answer(*c);
    TEST_CHECK(log.actions == 1);
    TEST_CHECK(a->asks == 1 && c->asks == 1);
  }

  void secondRequestWhileWaitingIsIgnored() {
    luciole::CloseGate gate;
    Log log;
    int other = 0;
    auto a = fake("a", true);
    auto b = fake("b", true);
    gate.request({pageOf(a, log)}, [&log] { ++log.actions; });
    gate.request({pageOf(b, log)}, [&other] { ++other; });
    TEST_CHECK(b->asks == 0);
    answer(*a);
    TEST_CHECK(log.actions == 1);
    TEST_CHECK(other == 0);
    TEST_CHECK(b->asks == 0);
    // Once the first is done, a new request is taken.
    gate.request({pageOf(b, log)}, [&other] { ++other; });
    TEST_CHECK(b->asks == 1);
    answer(*b);
    TEST_CHECK(other == 1);
  }

  void failedSaveStopsTheRequest() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", true);
    a->answerSaves = false;
    gate.request({pageOf(a, log), pageOf(b, log)}, [&log] { ++log.actions; });
    answer(*a);
    TEST_CHECK(log.actions == 0);
    TEST_CHECK(a->asks == 1);
    TEST_CHECK(b->asks == 0);
    // The request ended, so the gate takes the next one.
    a->answerSaves = true;
    gate.request({pageOf(a, log)}, [&log] { ++log.actions; });
    TEST_CHECK(a->asks == 2);
    answer(*a);
    TEST_CHECK(log.actions == 1);
  }

  void pageThatMayGoAtOnceIsNotWaitedFor() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", true);
    a->goesAtOnce = true;
    gate.request({pageOf(a, log), pageOf(b, log)}, [&log] { ++log.actions; });
    TEST_CHECK((log.asked == std::vector<std::string>{"a", "b"}));
    TEST_CHECK(log.actions == 0);
    answer(*b);
    TEST_CHECK(log.actions == 1);
  }

  void answerInsideAskIsTakenOnce() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", true);
    a->answersAtOnce = true;
    b->answersAtOnce = true;
    gate.request({pageOf(a, log), pageOf(b, log)}, [&log] { ++log.actions; });
    TEST_CHECK(log.actions == 1);
    TEST_CHECK(a->asks == 1 && b->asks == 1);
    // An answer inside ask that keeps the changes still ends the request.
    auto c = fake("c", true);
    c->answersAtOnce = true;
    c->answerSaves = false;
    gate.request({pageOf(c, log)}, [&log] { ++log.actions; });
    TEST_CHECK(log.actions == 1);
    TEST_CHECK(c->asks == 1);
  }

  void repeatedAndStaleResumesChangeNothing() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", true);
    gate.request({pageOf(a, log), pageOf(b, log)}, [&log] { ++log.actions; });
    auto first = a->resume;
    answer(*a);
    TEST_CHECK(b->asks == 1);
    // A's continuation again must not count as B's answer.
    first();
    TEST_CHECK(log.actions == 0);
    TEST_CHECK(static_cast<bool>(b->resume));
    auto second = b->resume;
    answer(*b);
    TEST_CHECK(log.actions == 1);
    second();
    TEST_CHECK(log.actions == 1);
    // A continuation of a finished request does not answer the next request's page.
    auto c = fake("c", true);
    gate.request({pageOf(c, log)}, [&log] { ++log.actions; });
    second();
    first();
    TEST_CHECK(log.actions == 1);
    answer(*c);
    TEST_CHECK(log.actions == 2);
  }

  void continuationOfAPageThatWentAtOnceChangesNothing() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    a->goesAtOnce = true;
    int checks = 0;
    // While the gate looks at the next page, the page that went at once calls its continuation after all.
    const luciole::CloseGate::Page b{
        .unsaved =
            [&checks, a] {
              ++checks;
              if (auto resume = std::exchange(a->resume, {})) {
                resume();
              }
              return false;
            },
        .ask = [](const std::function<void()>&) { return false; },
    };
    gate.request({pageOf(a, log), b}, [&log] { ++log.actions; });
    TEST_CHECK(checks == 1);
    TEST_CHECK(log.actions == 1);
  }

  void pageSavedMeanwhileIsSkipped() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", true);
    gate.request({pageOf(a, log), pageOf(b, log)}, [&log] { ++log.actions; });
    b->unsaved = false;
    answer(*a);
    TEST_CHECK(b->asks == 0);
    TEST_CHECK(log.actions == 1);
  }

  void actionMayStartTheNextRequest() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    auto b = fake("b", true);
    gate.request({pageOf(a, log)}, [&] {
      ++log.actions;
      gate.request({pageOf(b, log)}, [&log] { ++log.actions; });
    });
    answer(*a);
    TEST_CHECK(log.actions == 1);
    TEST_CHECK(b->asks == 1);
    answer(*b);
    TEST_CHECK(log.actions == 2);
  }

  void actingOnlyWhileTheActionRuns() {
    luciole::CloseGate gate;
    auto a = fake("a", true);
    bool askedWhileActing = true;
    std::vector<bool> seen;
    TEST_CHECK(!gate.acting());
    gate.request(
        {{.unsaved = [a] { return a->unsaved; },
          .ask =
              [&](std::function<void()> resume) {
                askedWhileActing = gate.acting();
                a->resume = std::move(resume);
                return false;
              }}},
        [&] {
          seen.push_back(gate.acting());
          // A request the action starts that runs at once leaves the outer action acting.
          gate.request({}, [&] { seen.push_back(gate.acting()); });
          seen.push_back(gate.acting());
        }
    );
    TEST_CHECK(!askedWhileActing);
    TEST_CHECK(!gate.acting());
    answer(*a);
    TEST_CHECK((seen == std::vector<bool>{true, true, true}));
    TEST_CHECK(!gate.acting());
  }

  void closeStartedByTheActionIsNotAskedAgain() {
    // A window whose close guard asks through the gate and whose action closes it again, with a page that may go at
    // once although it still reports unsaved changes.
    luciole::CloseGate gate;
    auto page = fake("page", true);
    page->goesAtOnce = true;
    Log log;
    int guards = 0;
    bool open = true;
    std::function<void()> close;
    const auto mayClose = [&] {
      ++guards;
      if (gate.acting() || !page->unsaved) {
        return true;
      }
      // Bounds a regression that would ask again on every close.
      if (guards < 10) {
        gate.request({pageOf(page, log)}, [&close] { close(); });
      }
      return false;
    };
    close = [&] {
      if (open && mayClose()) {
        open = false;
      }
    };
    close();
    TEST_CHECK(!open);
    TEST_CHECK(guards == 2);
    TEST_CHECK(page->asks == 1);
  }

  void resumeAfterTheGateIsGoneDoesNothing() {
    Log log;
    auto a = fake("a", true);
    {
      luciole::CloseGate gate;
      gate.request({pageOf(a, log)}, [&log] { ++log.actions; });
    }
    answer(*a);
    TEST_CHECK(log.actions == 0);
  }

  void incompleteRequestsAreRefused() {
    luciole::CloseGate gate;
    Log log;
    auto a = fake("a", true);
    bool refused = false;
    try {
      gate.request({pageOf(a, log)}, {});
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
    refused = false;
    try {
      gate.request({{.unsaved = [] { return true; }, .ask = {}}}, [&log] { ++log.actions; });
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
    TEST_CHECK(a->asks == 0);
    // A refused request leaves the gate free.
    gate.request({pageOf(a, log)}, [&log] { ++log.actions; });
    answer(*a);
    TEST_CHECK(log.actions == 1);
  }
} // namespace

int main() {
  nothingUnsavedRunsAtOnce();
  asksInOrderOneAtATime();
  secondRequestWhileWaitingIsIgnored();
  failedSaveStopsTheRequest();
  pageThatMayGoAtOnceIsNotWaitedFor();
  answerInsideAskIsTakenOnce();
  repeatedAndStaleResumesChangeNothing();
  continuationOfAPageThatWentAtOnceChangesNothing();
  pageSavedMeanwhileIsSkipped();
  actionMayStartTheNextRequest();
  actingOnlyWhileTheActionRuns();
  closeStartedByTheActionIsNotAskedAgain();
  resumeAfterTheGateIsGoneDoesNothing();
  incompleteRequestsAreRefused();
  return 0;
}
