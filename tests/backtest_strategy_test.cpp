// Phases 3 and 4: HFT Strategy's rules, scenario by scenario; the settings
// file; the numbers a report leads with; validation; and tick data imported
// from a recording of an HFT session.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "backtest/config.hpp"
#include "backtest/import.hpp"
#include "backtest/report.hpp"
#include "backtest/setup.hpp"
#include "backtest/simulator.hpp"
#include "backtest/strategy.hpp"
#include "backtest/ticks.hpp"
#include "backtest/validate.hpp"
#include "lob/order_book.hpp"
#include "lob/recording.hpp"

namespace {

using bt::Tick;
using bt::TickKind;
using lob::Side;

constexpr std::int64_t ms(std::int64_t milliseconds) { return milliseconds * 1'000; }

Tick open_at(std::int64_t at) {
  Tick tick;
  tick.time_us = at;
  tick.kind = TickKind::Open;
  return tick;
}

Tick close_at(std::int64_t at) {
  Tick tick;
  tick.time_us = at;
  tick.kind = TickKind::Close;
  return tick;
}

Tick quote_at(std::int64_t at, std::int64_t bid, std::int64_t ask, std::uint32_t bid_size, std::uint32_t ask_size) {
  Tick tick;
  tick.time_us = at;
  tick.kind = TickKind::Quote;
  tick.bid = bid;
  tick.ask = ask;
  tick.bid_size = bid_size;
  tick.ask_size = ask_size;
  return tick;
}

Tick print_at(std::int64_t at, std::int64_t price, std::uint32_t size, Side aggressor) {
  Tick tick;
  tick.time_us = at;
  tick.kind = TickKind::Trade;
  tick.bid = price;
  tick.bid_size = size;
  tick.aggressor = aggressor;
  return tick;
}

// Only the touch: so the imbalance is exactly the quote's sizes.
bt::SimConfig touch_only() {
  bt::SimConfig config;
  config.depth_levels = 1;
  return config;
}

struct Outcome {
  bt::RunResult result;
  bt::StrategyCounters counters;
};

Outcome run(const std::vector<Tick>& ticks, const bt::StrategyConfig& config, bt::SimConfig exchange = touch_only()) {
  bt::TickVectorSource source({{"TEST", 10'000}}, ticks);
  bt::HftStrategy strategy(config);
  bt::Simulator simulator(source, strategy, exchange);
  Outcome outcome;
  outcome.result = simulator.run();
  outcome.counters = strategy.counters();
  return outcome;
}

// A book leaning to the bid: imbalance 0.8, micro-price 0.4 ticks above the
// mid, so the score is 1.2 and the strategy wants to buy at 99.99.
Tick leaning_up(std::int64_t at) { return quote_at(at, 9'999, 10'000, 900, 100); }

// --- The strategy -------------------------------------------------------------------

TEST(HftStrategy, JoinsTheBidWhenTheBookLeansUpAndTakesItsTarget) {
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          // The offer comes down to our bid: we are filled at 99.99, and
          // offer the position out a tick up, at 100.00.
          quote_at(ms(1'000), 9'998, 9'999, 500, 500),
          // The bid comes up to our offer: out at 100.00.
          quote_at(ms(1'500), 10'000, 10'001, 500, 500),
          quote_at(ms(2'000), 10'000, 10'001, 500, 500),
          close_at(ms(2'500)),
      },
      bt::StrategyConfig{});
  ASSERT_EQ(outcome.result.trips.size(), 1u);
  EXPECT_EQ(outcome.result.trips[0].side, Side::Buy);
  EXPECT_EQ(outcome.result.trips[0].shares, 100u);
  EXPECT_NEAR(outcome.result.trips[0].pnl, 1.00, 1e-9);
  EXPECT_FALSE(outcome.result.trips[0].forced);
  EXPECT_EQ(outcome.counters.entries, 1u);
  EXPECT_EQ(outcome.counters.entries_filled, 1u);
  EXPECT_EQ(outcome.counters.target_exits, 1u);
  EXPECT_EQ(bt::success_text(bt::summarize(outcome.result)), "100.0% (1 of 1 trades)");
}

TEST(HftStrategy, JoinsTheOfferWhenTheBookLeansDown) {
  const Outcome outcome = run(
      {
          open_at(0),
          quote_at(ms(500), 9'999, 10'000, 100, 900),
          quote_at(ms(1'000), 10'000, 10'001, 500, 500),  // the bid comes up to our offer
          quote_at(ms(1'500), 9'998, 9'999, 500, 500),    // the offer comes down to our bid
          close_at(ms(2'000)),
      },
      bt::StrategyConfig{});
  ASSERT_EQ(outcome.result.trips.size(), 1u);
  EXPECT_EQ(outcome.result.trips[0].side, Side::Sell);
  EXPECT_NEAR(outcome.result.trips[0].pnl, 1.00, 1e-9);
}

TEST(HftStrategy, TimeStopGetsOutAtTheMarket) {
  std::vector<Tick> ticks{open_at(0), leaning_up(ms(500))};
  // Filled at 99.99; the price then sits a tick lower and never reaches the target.
  for (std::int64_t at = 1'000; at <= 4'500; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'998, 9'999, 500, 500));
  }
  ticks.push_back(close_at(ms(5'000)));
  const Outcome outcome = run(ticks, bt::StrategyConfig{});
  ASSERT_EQ(outcome.result.trips.size(), 1u);
  EXPECT_NEAR(outcome.result.trips[0].pnl, -1.00, 1e-9);  // sold at the bid, 99.98
  EXPECT_FALSE(outcome.result.trips[0].forced);
  EXPECT_EQ(outcome.counters.time_stops, 1u);
  EXPECT_EQ(outcome.counters.target_exits, 0u);
  // The position was 3 seconds old when it went: the fill at 1.000 s, the stop at 4.000 s.
  EXPECT_GE(outcome.result.trips[0].closed_us - outcome.result.trips[0].opened_us, ms(3'000));
  EXPECT_LT(outcome.result.trips[0].closed_us - outcome.result.trips[0].opened_us, ms(3'010));
  EXPECT_EQ(bt::success_text(bt::summarize(outcome.result)), "0.0% (0 of 1 trades)");
}

TEST(HftStrategy, ScratchesWhenTheSignalTurns) {
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          quote_at(ms(1'000), 9'998, 9'999, 500, 500),  // filled at 99.99
          quote_at(ms(1'500), 9'998, 9'999, 100, 900),  // the book now leans hard the other way
          quote_at(ms(2'000), 9'998, 9'999, 100, 900),
          close_at(ms(2'500)),
      },
      bt::StrategyConfig{});
  ASSERT_EQ(outcome.result.trips.size(), 1u);
  EXPECT_EQ(outcome.counters.scratches, 1u);
  EXPECT_EQ(outcome.counters.time_stops, 0u);
  EXPECT_NEAR(outcome.result.trips[0].pnl, -1.00, 1e-9);
  EXPECT_LT(outcome.result.trips[0].closed_us, ms(1'600));  // out at once, not at the close
}

TEST(HftStrategy, StaysOutOfABalancedBook) {
  std::vector<Tick> ticks{open_at(0)};
  for (std::int64_t at = 500; at <= 5'000; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'999, 10'000, 500, 500));
  }
  ticks.push_back(close_at(ms(5'500)));
  const Outcome outcome = run(ticks, bt::StrategyConfig{});
  EXPECT_EQ(outcome.result.orders_sent, 0u);
  EXPECT_TRUE(outcome.result.trips.empty());
}

TEST(HftStrategy, ToxicFlowKeepsItOut) {
  std::vector<Tick> ticks{open_at(0)};
  for (int index = 0; index < 10; ++index) {
    ticks.push_back(print_at(ms(100 + index), 9'999, 100, Side::Sell));  // everyone is selling
  }
  ticks.push_back(leaning_up(ms(500)));
  ticks.push_back(leaning_up(ms(1'000)));
  ticks.push_back(close_at(ms(1'500)));
  const Outcome outcome = run(ticks, bt::StrategyConfig{});
  EXPECT_EQ(outcome.result.orders_sent, 0u);
  EXPECT_EQ(outcome.counters.too_toxic, 2u);
}

TEST(HftStrategy, WithdrawsAnEntryWhenTheSignalFades) {
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          quote_at(ms(1'000), 9'999, 10'000, 500, 500),  // same prices, no lean any more
          quote_at(ms(1'500), 9'999, 10'000, 500, 500),
          close_at(ms(2'000)),
      },
      bt::StrategyConfig{});
  EXPECT_EQ(outcome.result.orders_sent, 1u);
  EXPECT_EQ(outcome.result.cancels_sent, 1u);
  EXPECT_EQ(outcome.counters.entries_withdrawn, 1u);
  EXPECT_TRUE(outcome.result.trips.empty());
}

TEST(HftStrategy, WithdrawsAnEntryWhenTheBidMovesAway) {
  bt::StrategyConfig config;
  config.cancel_score = -10;  // the signal never fades: only the price can make it go
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          // The bid moves up a tick: our order at 99.99 is no longer at the front of anything.
          quote_at(ms(1'000), 10'000, 10'001, 900, 100),
          close_at(ms(1'500)),
      },
      config);
  EXPECT_EQ(outcome.counters.entries_withdrawn, 1u);
  EXPECT_GE(outcome.result.cancels_sent, 1u);
}

TEST(HftStrategy, WithdrawsWhenItIsLeftAloneAtItsPrice) {
  bt::StrategyConfig config;
  config.cancel_score = -10;
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          // Everyone else leaves 99.99: our order is all that is there, the
          // best bid only because nobody else wants to buy there.
          quote_at(ms(1'000), 9'998, 10'000, 900, 100),
          close_at(ms(1'500)),
      },
      config);
  EXPECT_EQ(outcome.counters.entries_withdrawn, 1u);
  EXPECT_TRUE(outcome.result.trips.empty());
}

TEST(HftStrategy, WithdrawsAnEntryThatWaitsTooLong) {
  bt::StrategyConfig config;
  config.cancel_score = -10;
  config.entry_timeout_ms = 1'200;
  std::vector<Tick> ticks{open_at(0)};
  for (std::int64_t at = 500; at <= 3'000; at += 500) {
    ticks.push_back(leaning_up(ms(at)));  // never filled: the offer never comes down
  }
  ticks.push_back(close_at(ms(3'500)));
  const Outcome outcome = run(ticks, config);
  EXPECT_GE(outcome.counters.entries_withdrawn, 1u);
  EXPECT_TRUE(outcome.result.trips.empty());
}

TEST(HftStrategy, KillSwitchStopsTheSessionAfterItsLossLimit) {
  bt::StrategyConfig config;
  config.max_session_loss = 0.50;
  std::vector<Tick> ticks{open_at(0), leaning_up(ms(500))};
  for (std::int64_t at = 1'000; at <= 4'500; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'998, 9'999, 500, 500));  // a $1 loss at the time stop
  }
  for (std::int64_t at = 5'000; at <= 7'000; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'998, 9'999, 900, 100));  // tempting, but stopped
  }
  ticks.push_back(close_at(ms(7'500)));
  const Outcome outcome = run(ticks, config);
  EXPECT_EQ(outcome.counters.kill_switches, 1u);
  EXPECT_EQ(outcome.counters.entries, 1u);
  EXPECT_EQ(outcome.result.trips.size(), 1u);
}

TEST(HftStrategy, KillSwitchIsResetByANewSession) {
  bt::StrategyConfig config;
  config.max_session_loss = 0.50;
  std::vector<Tick> ticks{open_at(0), leaning_up(ms(500))};
  for (std::int64_t at = 1'000; at <= 4'500; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'998, 9'999, 500, 500));
  }
  ticks.push_back(close_at(ms(5'000)));
  ticks.push_back(open_at(ms(10'000)));
  ticks.push_back(leaning_up(ms(10'500)));
  ticks.push_back(close_at(ms(11'000)));
  const Outcome outcome = run(ticks, config);
  EXPECT_EQ(outcome.counters.kill_switches, 1u);
  EXPECT_EQ(outcome.counters.entries, 2u);  // trading again the next day
}

TEST(HftStrategy, RunKillSwitchLastsForTheRestOfTheRun) {
  bt::StrategyConfig config;
  config.max_session_loss = 100;
  config.max_run_loss = 0.50;
  std::vector<Tick> ticks{open_at(0), leaning_up(ms(500))};
  for (std::int64_t at = 1'000; at <= 4'500; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'998, 9'999, 500, 500));
  }
  ticks.push_back(close_at(ms(5'000)));
  ticks.push_back(open_at(ms(10'000)));
  ticks.push_back(leaning_up(ms(10'500)));
  ticks.push_back(close_at(ms(11'000)));
  const Outcome outcome = run(ticks, config);
  EXPECT_EQ(outcome.counters.kill_switches, 1u);
  EXPECT_EQ(outcome.counters.entries, 1u);
}

TEST(HftStrategy, PositionLimitsRefuseEntries) {
  bt::StrategyConfig config;
  config.max_total_position = 50;  // smaller than one order
  const Outcome outcome = run({open_at(0), leaning_up(ms(500)), leaning_up(ms(1'000)), close_at(ms(1'500))}, config);
  EXPECT_EQ(outcome.result.orders_sent, 0u);
  EXPECT_EQ(outcome.counters.refused_by_limits, 2u);
}

TEST(HftStrategy, OrderRateLimitRefusesEntries) {
  bt::StrategyConfig config;
  config.max_orders_per_second = 1;
  // An entry, withdrawn at once because the signal fades, and then the
  // signal is back within the second: the second entry must wait.
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          quote_at(ms(600), 9'999, 10'000, 500, 500),
          leaning_up(ms(700)),
          leaning_up(ms(800)),
          close_at(ms(900)),
      },
      config);
  EXPECT_EQ(outcome.counters.entries, 1u);
  EXPECT_GE(outcome.counters.refused_by_limits, 1u);
}

TEST(HftStrategy, SpreadOutsideItsRangeKeepsItOut) {
  const Outcome outcome = run({open_at(0), quote_at(ms(500), 9'990, 10'000, 900, 100), close_at(ms(1'000))},
                              bt::StrategyConfig{});
  EXPECT_EQ(outcome.result.orders_sent, 0u);
}

TEST(HftStrategy, CoinTossBaselineEntersEvenInABalancedBook) {
  bt::StrategyConfig config;
  config.random_entries = true;
  std::vector<Tick> ticks{open_at(0)};
  for (std::int64_t at = 500; at <= 3'000; at += 500) {
    ticks.push_back(quote_at(ms(at), 9'999, 10'000, 500, 500));
  }
  ticks.push_back(close_at(ms(3'500)));
  const Outcome outcome = run(ticks, config);
  EXPECT_GE(outcome.counters.entries, 1u);
}

TEST(HftStrategy, CommissionCountsTowardsTheKillSwitch) {
  // Held, the position is marked $3.50 down ($3 commission in, half a tick
  // to the mid); closed, the trade made a tick ($1) and paid $6. Without the
  // commission it would be $1 up and the switch would never trip.
  bt::StrategyConfig config;
  config.max_session_loss = 4.00;
  config.commission_per_share = 0.03;
  bt::SimConfig exchange = touch_only();
  exchange.commission_per_share = 0.03;
  const Outcome outcome = run(
      {
          open_at(0),
          leaning_up(ms(500)),
          quote_at(ms(1'000), 9'998, 9'999, 500, 500),
          quote_at(ms(1'500), 10'000, 10'001, 500, 500),
          leaning_up(ms(2'000)),
          close_at(ms(2'500)),
      },
      config, exchange);
  ASSERT_EQ(outcome.result.trips.size(), 1u);
  EXPECT_NEAR(outcome.result.trips[0].pnl, -5.00, 1e-9);
  EXPECT_EQ(outcome.counters.target_exits, 1u);
  EXPECT_EQ(outcome.counters.kill_switches, 1u);
  EXPECT_EQ(outcome.counters.entries, 1u);
}

TEST(HftStrategy, PracticeMarketRunRespectsItsLimits) {
  bt::PracticeMarketOptions options;
  options.ticks_per_session = 2'000;
  options.sessions = 2;
  bt::PracticeMarketSource source(options);
  bt::StrategyConfig config;
  config.max_total_position = 300;
  bt::HftStrategy strategy(config);
  bt::Simulator simulator(source, strategy);
  const bt::RunResult result = simulator.run();
  EXPECT_GT(result.trips.size(), 0u);
  for (const bt::RoundTrip& trip : result.trips) {
    EXPECT_LE(trip.shares, static_cast<std::uint64_t>(config.max_position));
  }
  EXPECT_EQ(result.by_session.size(), 2u);
}

TEST(SignalCheck, FindsASignalThatPredictsAndIgnoresOneThatDoesNot) {
  bt::SignalCheck check;
  bt::Mulberry32 random(5);
  double mid = 10'000;
  for (int step = 0; step < 2'000; ++step) {
    const double predictive = random() < 0.5 ? 1 : -1;
    const double noise = random() < 0.5 ? 1 : -1;
    check.observe(0, ms(1'000) * step, {predictive, noise, 0, noise, predictive}, mid);
    mid += predictive;  // the next second's move is what the first signal said
  }
  EXPECT_NEAR(check.sums(0, 0).correlation(), 1.0, 1e-9);
  EXPECT_NEAR(check.sums(0, 0).slope(), 1.0, 1e-9);
  EXPECT_LT(std::abs(check.sums(0, 1).correlation()), 0.1);
  EXPECT_EQ(check.sums(0, 2).correlation(), 0.0);  // a constant says nothing
  EXPECT_GT(check.sums(0, 0).n, 1'990);
}

// --- Settings -----------------------------------------------------------------------

TEST(Settings, ReadsSectionsValuesAndComments) {
  std::string error;
  std::optional<bt::Settings> settings =
      bt::Settings::parse("# a comment\n[a]\nx = 1.5 ; another\ny=yes\nz = p, q ,r\n\n[b]\nn = 7\n", error);
  ASSERT_TRUE(settings) << error;
  EXPECT_DOUBLE_EQ(settings->number("a.x", 0), 1.5);
  EXPECT_TRUE(settings->flag("a.y", false));
  EXPECT_EQ(settings->list("a.z", {}), (std::vector<std::string>{"p", "q", "r"}));
  EXPECT_EQ(settings->integer("b.n", 0), 7);
  EXPECT_EQ(settings->number("b.missing", 3.0), 3.0);
  EXPECT_TRUE(settings->problems().empty());
}

TEST(Settings, ReportsUnknownNamesAndBadValues) {
  std::string error;
  std::optional<bt::Settings> settings = bt::Settings::parse("[a]\nx = abc\nwhole = 1.5\nflag = maybe\ntypo = 1\n", error);
  ASSERT_TRUE(settings);
  EXPECT_EQ(settings->number("a.x", 2.0), 2.0);
  EXPECT_EQ(settings->integer("a.whole", 4), 4);
  EXPECT_FALSE(settings->flag("a.flag", false));
  const std::vector<std::string> problems = settings->problems();
  ASSERT_EQ(problems.size(), 4u);
  EXPECT_NE(problems[0].find("a.x"), std::string::npos);
  EXPECT_NE(problems[1].find("a.whole"), std::string::npos);
  EXPECT_NE(problems[2].find("a.flag"), std::string::npos);
  EXPECT_EQ(problems[3], "unknown setting a.typo");
}

TEST(Settings, RefusesLinesItCannotRead) {
  std::string error;
  EXPECT_FALSE(bt::Settings::parse("[a\n", error));
  EXPECT_NE(error.find("line 1"), std::string::npos);
  EXPECT_FALSE(bt::Settings::parse("[a]\n\njust words\n", error));
  EXPECT_NE(error.find("line 3"), std::string::npos);
  EXPECT_FALSE(bt::Settings::parse("= 4\n", error));
  EXPECT_FALSE(bt::Settings::load("/no/such/file.ini", error));
}

TEST(Settings, OverridesWinOverTheFile) {
  std::string error;
  std::optional<bt::Settings> settings = bt::Settings::parse("[strategy]\nenter_score = 0.5\n", error);
  ASSERT_TRUE(settings);
  settings->set("strategy.enter_score", "0.9");
  const bt::RunSetup setup = bt::read_setup(*settings);
  EXPECT_DOUBLE_EQ(setup.strategy.enter_score, 0.9);
}

TEST(Settings, TheShippedFileReadsCleanly) {
  std::string error;
  std::optional<bt::Settings> settings = bt::Settings::load(std::string(LOB_SOURCE_DIR) + "/backtest/hft_strategy.ini", error);
  ASSERT_TRUE(settings) << error;
  const bt::RunSetup setup = bt::read_setup(*settings);
  EXPECT_TRUE(settings->problems().empty()) << settings->problems().front();
  EXPECT_EQ(setup.market.symbols.size(), 10u);
  EXPECT_FALSE(setup.exchange.print_fills);
  EXPECT_EQ(setup.exchange.queue, bt::QueueModel::ProRata);
  EXPECT_DOUBLE_EQ(setup.strategy.enter_score, 0.5);
  EXPECT_EQ(setup.strategy.target_ticks, 1);
  EXPECT_EQ(setup.validation.grid.size(), 3u);
}

TEST(Settings, ValuesOutOfRangeAreProblems) {
  std::string error;
  std::optional<bt::Settings> settings = bt::Settings::parse(
      "[exchange]\nqueue = sideways\ncommission_per_share = -1\n[strategy]\nobi_levels = 9\ntarget_ticks = 0\n", error);
  ASSERT_TRUE(settings);
  static_cast<void>(bt::read_setup(*settings));
  std::string all;
  for (const std::string& problem : settings->problems()) {
    all += problem + "\n";
  }
  EXPECT_NE(all.find("exchange.queue"), std::string::npos);
  EXPECT_NE(all.find("commission_per_share"), std::string::npos);
  EXPECT_NE(all.find("obi_levels"), std::string::npos);
  EXPECT_NE(all.find("target_ticks"), std::string::npos);
}

// --- Reports ------------------------------------------------------------------------

bt::RunResult four_trips() {
  bt::RunResult run;
  const double pnls[] = {1.0, -2.0, 1.0, 0.0};
  const std::uint32_t sessions[] = {0, 0, 1, 1};
  for (int index = 0; index < 4; ++index) {
    bt::RoundTrip trip;
    trip.pnl = pnls[index];
    trip.session = sessions[index];
    trip.forced = index == 3;
    run.trips.push_back(trip);
  }
  run.sessions = 2;
  run.by_session.resize(2);
  run.by_session[0].passive_submitted = 400;
  run.by_session[0].passive_filled = 100;
  run.by_session[0].adverse_1s_sum = 50;
  run.by_session[0].adverse_1s_count = 100;
  run.by_session[1].passive_submitted = 100;
  run.by_session[1].passive_filled = 100;
  return run;
}

TEST(Report, SummaryLeadsWithTheSuccessRate) {
  const bt::Summary summary = bt::summarize(four_trips());
  EXPECT_EQ(summary.trades, 4u);
  EXPECT_EQ(summary.wins, 2u);
  EXPECT_EQ(summary.losses, 1u);
  EXPECT_EQ(summary.forced, 1u);
  EXPECT_DOUBLE_EQ(summary.net, 0.0);
  EXPECT_DOUBLE_EQ(summary.worst, -2.0);
  EXPECT_DOUBLE_EQ(summary.best, 1.0);
  EXPECT_DOUBLE_EQ(summary.max_drawdown, 2.0);  // up to +1, down to -1
  EXPECT_DOUBLE_EQ(summary.fill_rate, 200.0 / 500.0);
  EXPECT_DOUBLE_EQ(summary.adverse_1s, 0.5);
  EXPECT_EQ(bt::success_text(summary), "50.0% (2 of 4 trades)");
  const std::string lines = bt::summary_lines(summary);
  EXPECT_EQ(lines.rfind("Success rate:", 0), 0u);
  EXPECT_LT(lines.find("Net profit"), lines.find("Profit per trade"));
  EXPECT_LT(lines.find("Profit per trade"), lines.find("Worst trade"));
  EXPECT_LT(lines.find("Worst trade"), lines.find("Max drawdown"));
  EXPECT_LT(lines.find("Max drawdown"), lines.find("Fill rate"));
  EXPECT_LT(lines.find("Fill rate"), lines.find("Adverse selection"));
  const auto [low, high] = summary.success_range();
  EXPECT_LT(low, 0.5);
  EXPECT_GT(high, 0.5);
}

TEST(Report, SummarizesAStretchOfSessions) {
  const bt::RunResult run = four_trips();
  const bt::Summary second = bt::summarize(run, 1, 2);
  EXPECT_EQ(second.trades, 2u);
  EXPECT_EQ(second.wins, 1u);
  EXPECT_DOUBLE_EQ(second.net, 1.0);
  EXPECT_DOUBLE_EQ(second.fill_rate, 1.0);
  const bt::Summary joined = bt::combine({{&run, {1, 2}}, {&run, {0, 1}}});
  EXPECT_EQ(joined.trades, 4u);
  EXPECT_EQ(joined.sessions, 2u);
  EXPECT_DOUBLE_EQ(joined.max_drawdown, 2.0);  // +1, +1, +2, 0
}

TEST(Report, TStatisticAndBaselineGap) {
  bt::RunResult steady;
  bt::RunResult noisy;
  for (int index = 0; index < 100; ++index) {
    bt::RoundTrip trip;
    trip.pnl = index % 2 == 0 ? 1.1 : 0.9;
    steady.trips.push_back(trip);
    trip.pnl = index % 2 == 0 ? 1.0 : -1.0;
    noisy.trips.push_back(trip);
  }
  steady.sessions = noisy.sessions = 1;
  const bt::Summary a = bt::summarize(steady);
  const bt::Summary b = bt::summarize(noisy);
  EXPECT_GT(a.t_stat, 50);
  EXPECT_NEAR(b.t_stat, 0, 1e-9);
  EXPECT_GT(bt::welch_t(a, b), 5);
  EXPECT_LT(bt::welch_t(b, a), -5);
}

TEST(Report, RankCorrelation) {
  EXPECT_NEAR(bt::rank_correlation({1, 2, 3, 4}, {10, 20, 30, 40}), 1.0, 1e-12);
  EXPECT_NEAR(bt::rank_correlation({1, 2, 3, 4}, {4, 3, 2, 1}), -1.0, 1e-12);
  EXPECT_EQ(bt::ranks({3, 1, 2, 1}), (std::vector<double>{4, 1.5, 3, 1.5}));
  EXPECT_EQ(bt::rank_correlation({1, 2}, {1, 2}), 0.0);  // too few to say
}

// --- Validation ---------------------------------------------------------------------

TEST(Validation, ExpandsTheGrid) {
  std::string error;
  const auto grid = bt::expand_grid({"strategy.a=1|2", "strategy.b = x | y | z"}, error);
  ASSERT_TRUE(grid) << error;
  ASSERT_EQ(grid->size(), 6u);
  EXPECT_EQ((*grid)[0].label(), "a 1, b x");
  EXPECT_EQ((*grid)[5].label(), "a 2, b z");
  EXPECT_FALSE(bt::expand_grid({"nonsense"}, error));
  EXPECT_FALSE(bt::expand_grid({"strategy.a=|"}, error));
  const auto none = bt::expand_grid({}, error);
  ASSERT_TRUE(none);
  EXPECT_EQ(none->size(), 1u);
}

constexpr const char* kSmallValidation =
    "[market]\nticks_per_session = 150\nsymbols = AAPL, MSFT\n"
    "[validation]\nsessions = 4\ntrain_sessions = 2\ntest_sessions = 1\nholdout_sessions = 1\n"
    "grid = strategy.enter_score=0.3|0.6\nthreads = 3\n";

TEST(Validation, RunsEveryStageAndReachesAVerdict) {
  std::string error;
  const std::optional<bt::Settings> settings = bt::Settings::parse(kSmallValidation, error);
  ASSERT_TRUE(settings);
  const bt::Validation v = bt::validate(*settings);
  ASSERT_TRUE(v.error.empty()) << v.error;
  EXPECT_EQ(v.candidates.size(), 2u);
  ASSERT_EQ(v.steps.size(), 2u);  // train 1-2 test 3, train 2-3 test 4
  EXPECT_EQ(v.steps[0].test_first, 2u);
  EXPECT_EQ(v.steps[1].test_end, 4u);
  EXPECT_EQ(v.out_of_sample.sessions, 2u);
  EXPECT_EQ(v.holdout.sessions, 1u);
  EXPECT_EQ(v.total_sessions, 5u);
  EXPECT_FALSE(v.sensitivity.empty());
  EXPECT_GE(v.stress.size(), 8u);
  EXPECT_FALSE(v.verdict.empty());
  EXPECT_GT(v.signals.sums(0, 0).n, 0);
  if (!v.edge) {
    EXPECT_FALSE(v.reasons.empty());
  }
  const bt::RunSetup setup = *bt::make_setup(*settings, {}, error);
  const std::string md = bt::validation_markdown(v, setup);
  EXPECT_EQ(md.find("**Success rate out of sample: "), md.find("**"));  // it leads
  EXPECT_NE(md.find("Verdict: "), std::string::npos);
  const std::string json = bt::validation_json(v);
  EXPECT_NE(json.find("\"out_of_sample\":{\"success_rate\":"), std::string::npos);
}

TEST(Validation, SameSettingsSameAnswer) {
  std::string error;
  const std::optional<bt::Settings> settings = bt::Settings::parse(kSmallValidation, error);
  ASSERT_TRUE(settings);
  const bt::Validation first = bt::validate(*settings);
  bt::Settings one_thread = *settings;
  one_thread.set("validation.threads", "1");
  const bt::Validation second = bt::validate(one_thread);
  ASSERT_TRUE(first.error.empty() && second.error.empty());
  EXPECT_EQ(bt::summary_json(first.out_of_sample), bt::summary_json(second.out_of_sample));
  EXPECT_EQ(bt::summary_json(first.holdout), bt::summary_json(second.holdout));
  EXPECT_EQ(bt::summary_json(first.baseline_out_of_sample), bt::summary_json(second.baseline_out_of_sample));
  ASSERT_EQ(first.stress.size(), second.stress.size());
  for (std::size_t index = 0; index < first.stress.size(); ++index) {
    EXPECT_EQ(bt::summary_json(first.stress[index].summary), bt::summary_json(second.stress[index].summary));
  }
  EXPECT_EQ(first.verdict, second.verdict);
}

TEST(Validation, RefusesSettingsThatCannotWork) {
  std::string error;
  const std::optional<bt::Settings> settings =
      bt::Settings::parse("[validation]\nsessions = 2\ntrain_sessions = 2\ntest_sessions = 1\n", error);
  ASSERT_TRUE(settings);
  const bt::Validation v = bt::validate(*settings);
  EXPECT_NE(v.error.find("validation.sessions"), std::string::npos);
  const std::optional<bt::Settings> typo = bt::Settings::parse("[strategy]\nenter_scor = 1\n", error);
  ASSERT_TRUE(typo);
  EXPECT_NE(bt::validate(*typo).error.find("unknown setting strategy.enter_scor"), std::string::npos);
}

// --- Importing an HFT recording -----------------------------------------------------

class TempFile {
 public:
  TempFile()
      : path_((std::filesystem::temp_directory_path() /
               ("backtest-" + std::to_string(::getpid()) + "-" + std::to_string(next_++) + ".rec"))
                  .string()) {}
  ~TempFile() { std::remove(path_.c_str()); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  static inline int next_ = 0;
  std::string path_;
};

lob::Command submit(lob::SymbolId symbol, Side side, lob::Price price, lob::Quantity quantity,
                    lob::OrderType type = lob::OrderType::Limit) {
  lob::Command command{};
  command.type = lob::CommandType::Submit;
  command.symbol = symbol;
  command.side = side;
  command.price = price;
  command.quantity = quantity;
  command.order_type = type;
  return command;
}

lob::Command cancel(lob::SymbolId symbol, lob::OrderId id) {
  lob::Command command{};
  command.type = lob::CommandType::Cancel;
  command.symbol = symbol;
  command.order_id = id;
  return command;
}

// Writes a recording by hand, in the recorder's own format.
void write_recording(const std::string& path, const std::vector<lob::BookConfig>& books,
                     const std::vector<lob::Command>& commands) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  ASSERT_NE(file, nullptr);
  lob::RecordingHeader header{};
  std::copy(std::begin(lob::kRecordingMagic), std::end(lob::kRecordingMagic), header.magic);
  header.byte_order = lob::kRecordingByteOrder;
  header.version = lob::kRecordingVersion;
  header.book_count = static_cast<std::uint32_t>(books.size());
  header.command_size = sizeof(lob::RecordedCommand);
  std::fwrite(&header, sizeof(header), 1, file);
  for (const lob::BookConfig& book : books) {
    lob::RecordedBook recorded{};
    std::copy(book.symbol.begin(), book.symbol.end(), recorded.symbol);
    recorded.min_price = book.min_price;
    recorded.num_levels = book.num_levels;
    recorded.max_orders = book.max_orders;
    std::fwrite(&recorded, sizeof(recorded), 1, file);
  }
  for (const lob::Command& command : commands) {
    const lob::RecordedCommand recorded = lob::to_recorded(command);
    std::fwrite(&recorded, sizeof(recorded), 1, file);
  }
  std::fclose(file);
}

TEST(ImportHft, TurnsARecordingIntoTicks) {
  const std::vector<lob::BookConfig> books{
      {.symbol = "AAA", .min_price = 9'000, .num_levels = 2'000, .max_orders = 64},
      {.symbol = "BBB", .min_price = 4'000, .num_levels = 2'000, .max_orders = 64},
  };
  // The ID the first order in AAA's book will get, as the engine gave it.
  lob::OrderBook reference(books[0]);
  const lob::OrderId first_bid = reference.submit(Side::Buy, 9'999, 100, [](const lob::Trade&) {}).id;

  const TempFile file;
  write_recording(file.path(), books,
                  {
                      submit(0, Side::Buy, 9'999, 100),   // tick 1: AAA's ladder...
                      submit(0, Side::Sell, 10'001, 100),
                      submit(1, Side::Buy, 4'999, 50),    // ...then BBB's
                      submit(1, Side::Sell, 5'001, 50),
                      submit(0, Side::Buy, 10'001, 40, lob::OrderType::IOC),  // tick 2: AAA again; someone buys
                      cancel(0, first_bid),               // and AAA's bid moves down
                      submit(0, Side::Buy, 9'998, 70),
                  });
  const std::optional<lob::Recording> recording = lob::Recording::load(file.path());
  ASSERT_TRUE(recording);
  const bt::ImportResult imported = bt::import_recording(*recording);
  EXPECT_FALSE(imported.damaged);
  EXPECT_EQ(imported.commands, 7u);
  EXPECT_EQ(imported.submits, 6u);
  EXPECT_EQ(imported.cancels, 1u);
  EXPECT_EQ(imported.trades, 1u);
  EXPECT_EQ(imported.quotes, 3u);
  ASSERT_EQ(imported.symbols.size(), 2u);
  EXPECT_EQ(imported.symbols[0].name, "AAA");
  EXPECT_EQ(imported.symbols[0].reference_cents, 10'000);

  const std::vector<Tick>& ticks = imported.ticks;
  ASSERT_EQ(ticks.size(), 2u + 3u + 1u + 2u);
  EXPECT_EQ(ticks[0].kind, TickKind::Open);
  EXPECT_EQ(ticks[1].kind, TickKind::Open);
  EXPECT_EQ(ticks[2].kind, TickKind::Quote);
  EXPECT_EQ(ticks[2].symbol, 0);
  EXPECT_EQ(ticks[2].time_us, ms(500));
  EXPECT_EQ(ticks[2].bid, 9'999);
  EXPECT_EQ(ticks[2].ask, 10'001);
  EXPECT_EQ(ticks[3].symbol, 1);
  EXPECT_EQ(ticks[3].time_us, ms(500));
  EXPECT_EQ(ticks[4].kind, TickKind::Trade);
  EXPECT_EQ(ticks[4].time_us, ms(1'000));
  EXPECT_EQ(ticks[4].bid, 10'001);
  EXPECT_EQ(ticks[4].bid_size, 40u);
  EXPECT_EQ(ticks[4].aggressor, Side::Buy);
  EXPECT_EQ(ticks[5].kind, TickKind::Quote);  // the cancel found its order
  EXPECT_EQ(ticks[5].bid, 9'998);
  EXPECT_EQ(ticks[5].bid_size, 70u);
  EXPECT_EQ(ticks[5].ask_size, 60u);
  EXPECT_EQ(ticks[6].kind, TickKind::Close);
  EXPECT_GT(ticks[6].time_us, ticks[5].time_us);

  // And the strategy can be run on it.
  bt::TickVectorSource source(imported.symbols, imported.ticks);
  bt::HftStrategy strategy(bt::StrategyConfig{});
  bt::Simulator simulator(source, strategy);
  const bt::RunResult result = simulator.run();
  EXPECT_EQ(result.sessions, 1u);
}

}  // namespace
