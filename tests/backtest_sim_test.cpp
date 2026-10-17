// Phase 2: the tick-level backtester's exchange mechanics, scenario by
// scenario, each with an answer worked out by hand.
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "backtest/simulator.hpp"
#include "backtest/ticks.hpp"

namespace {

using bt::Context;
using bt::FillReport;
using bt::OrderEvent;
using bt::OrderKind;
using bt::OrderReport;
using bt::SimConfig;
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

// A strategy that does what it is told, when it is told, and writes down
// everything it hears.
class Script final : public bt::Strategy {
 public:
  void at(std::int64_t time_us, std::function<void(Context&)> action) { plan_.emplace_back(time_us, std::move(action)); }

  void on_start(Context& context) override {
    for (std::size_t index = 0; index < plan_.size(); ++index) {
      context.wake_at(plan_[index].first, index);
    }
  }
  void on_timer(Context& context, std::uint64_t token) override { plan_[token].second(context); }
  void on_market(Context&, const bt::MarketUpdate& update) override { market.push_back(update); }
  void on_fill(Context&, const FillReport& fill) override {
    fills.push_back(fill);
    fill_heard_at.push_back(0);
  }
  void on_order(Context&, const OrderReport& report) override { orders.push_back(report); }

  [[nodiscard]] std::uint32_t filled(std::uint64_t order_id) const {
    std::uint32_t total = 0;
    for (const FillReport& fill : fills) {
      if (fill.order_id == order_id) {
        total += fill.quantity;
      }
    }
    return total;
  }

  [[nodiscard]] bool heard(std::uint64_t order_id, OrderEvent event) const {
    for (const OrderReport& report : orders) {
      if (report.order_id == order_id && report.event == event) {
        return true;
      }
    }
    return false;
  }

  std::vector<FillReport> fills;
  std::vector<std::int64_t> fill_heard_at;
  std::vector<OrderReport> orders;
  std::vector<bt::MarketUpdate> market;

 private:
  std::vector<std::pair<std::int64_t, std::function<void(Context&)>>> plan_;
};

// One symbol, its touch only (no modelled depth), and a millisecond for everything.
SimConfig plain(bool print_fills, bt::QueueModel queue = bt::QueueModel::ProRata) {
  SimConfig config;
  config.feed_latency_us = ms(1);
  config.order_latency_us = ms(1);
  config.cancel_latency_us = ms(1);
  config.print_fills = print_fills;
  config.queue = queue;
  config.depth_levels = 1;
  return config;
}

bt::TickVectorSource source_of(std::vector<Tick> ticks) {
  return bt::TickVectorSource({{"TEST", 10'000}}, std::move(ticks));
}

// --- Queue position ----------------------------------------------------------------------

TEST(Backtester, AnOrderWaitsBehindTheQueueAndFillsAsPrintsReachIt) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      print_at(ms(300), 10'000, 300, Side::Sell),  // 300 of the 500 ahead
      print_at(ms(400), 10'000, 250, Side::Sell),  // the last 200 ahead, and 50 of ours
      print_at(ms(500), 10'000, 1'000, Side::Sell),  // the rest of ours
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'000, 100); });
  bt::Simulator sim(source, script, plain(true));
  const bt::RunResult result = sim.run();

  ASSERT_EQ(script.fills.size(), 3u);  // two pieces, then the forced exit at the close
  EXPECT_EQ(script.fills[0].order_id, id);
  EXPECT_EQ(script.fills[0].quantity, 50u);
  EXPECT_EQ(script.fills[0].remaining, 50u);
  EXPECT_EQ(script.fills[0].time_us, ms(400));
  EXPECT_TRUE(script.fills[0].passive);
  EXPECT_EQ(script.fills[1].quantity, 50u);
  EXPECT_EQ(script.fills[1].remaining, 0u);
  EXPECT_EQ(script.fills[1].time_us, ms(500));
  EXPECT_EQ(result.print_filled, 100u);
  EXPECT_EQ(result.traded_through, 0u);
  EXPECT_EQ(result.passive_submitted, 100u);
  EXPECT_EQ(result.passive_filled, 100u);
}

// The size at our price shrinks by 400 with 500 ahead of us and 300 behind;
// a print of 300 then comes. How much of it reaches us depends on where the
// 400 came from.
std::uint32_t filled_after_shrink(bt::QueueModel queue) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      // The size a quote shows is the market's own, without us in it: 300 more
      // joins, behind our 100.
      quote_at(ms(300), 10'000, 10'002, 800, 500),
      quote_at(ms(400), 10'000, 10'002, 400, 500),  // 400 fewer: 800 down to 400
      print_at(ms(500), 10'000, 300, Side::Sell),
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'000, 100); });
  bt::Simulator sim(source, script, plain(true, queue));
  sim.run();
  return script.filled(id);
}

TEST(Backtester, WhereAShrinkingQueueShrinksIsTheQueueModels) {
  // Pro rata: 400 x 500/800 = 250 from ahead, leaving 250 ahead: 50 reach us.
  EXPECT_EQ(filled_after_shrink(bt::QueueModel::ProRata), 50u);
  // Back: the 300 behind go first, then 100 from ahead, leaving 400: nothing reaches us.
  EXPECT_EQ(filled_after_shrink(bt::QueueModel::Back), 0u);
  // Front: 400 from ahead, leaving 100: we are filled in full.
  EXPECT_EQ(filled_after_shrink(bt::QueueModel::Front), 100u);
}

TEST(Backtester, NewSizeAtAPriceJoinsBehindUs) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 200, 500),
      quote_at(ms(300), 10'000, 10'002, 5'000, 500),  // a wall of size arrives behind us
      print_at(ms(400), 10'000, 300, Side::Sell),     // 200 ahead, then 100 of ours
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'000, 100); });
  bt::Simulator sim(source, script, plain(true));
  sim.run();
  EXPECT_EQ(script.filled(id), 100u);
}

// --- Being traded through -----------------------------------------------------------------

TEST(Backtester, WhenTheMarketMovesThroughARestingOrderItFillsAtItsOwnPrice) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      quote_at(ms(300), 9'997, 9'999, 500, 300),  // the offer is now below our bid
      quote_at(ms(1'400), 9'995, 9'997, 500, 500),
      close_at(ms(2'000)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'000, 100); });
  bt::Simulator sim(source, script, plain(false));
  const bt::RunResult result = sim.run();

  ASSERT_GE(script.fills.size(), 1u);
  EXPECT_EQ(script.fills[0].order_id, id);
  EXPECT_EQ(script.fills[0].price, 10'000);  // ours, not the new offer's
  EXPECT_EQ(script.fills[0].quantity, 100u);
  EXPECT_TRUE(script.fills[0].passive);
  EXPECT_EQ(script.fills[0].time_us, ms(300));
  EXPECT_EQ(result.traded_through, 100u);
  // A second later the mid (9,996) is four cents below what we paid: adverse.
  EXPECT_EQ(result.adverse_1s_count, 100u);
  EXPECT_DOUBLE_EQ(result.adverse_1s_sum / static_cast<double>(result.adverse_1s_count), 4.0);
}

TEST(Backtester, TradedThroughFillsOnlyAsMuchAsTheMarketShows) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      quote_at(ms(300), 9'997, 9'999, 500, 300),  // 300 offered below our bid of 1,000
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'000, 1'000); });
  bt::Simulator sim(source, script, plain(false));
  sim.run();
  EXPECT_EQ(script.filled(id), 300u);
}

TEST(Backtester, PrintsFillNothingUnlessTheyAreTakenAsRealOrders) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      print_at(ms(300), 10'000, 100'000, Side::Sell),
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'000, 100); });
  bt::Simulator sim(source, script, plain(false));
  const bt::RunResult result = sim.run();
  EXPECT_EQ(script.filled(id), 0u);
  EXPECT_EQ(result.print_filled, 0u);
  // It still sees the print.
  bool saw = false;
  for (const bt::MarketUpdate& update : script.market) {
    saw = saw || (update.kind == TickKind::Trade && update.trade_size == 100'000);
  }
  EXPECT_TRUE(saw);
}

// --- Time ---------------------------------------------------------------------------------

TEST(Backtester, AnOrderReachesTheBookOnlyAfterItsDelay) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      quote_at(ms(205), 10'001, 10'003, 500, 500),  // the offer moves up before our order lands
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t id = 0;
  script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'002, 100, OrderKind::IOC); });
  SimConfig config = plain(false);
  config.order_latency_us = ms(10);  // lands at 210 ms
  bt::Simulator sim(source, script, config);
  sim.run();
  // At 200 ms it would have bought at 10,002; by 210 ms the offer is 10,003.
  EXPECT_EQ(script.filled(id), 0u);
  EXPECT_TRUE(script.heard(id, OrderEvent::Done));
}

TEST(Backtester, ACancelCanArriveTooLate) {
  for (const bool late : {true, false}) {
    auto source = source_of({
        open_at(0),
        quote_at(ms(100), 10'000, 10'002, 500, 500),
        print_at(ms(320), 10'001, 100, Side::Sell),  // hits our bid inside the spread
        close_at(ms(600)),
    });
    Script script;
    std::uint64_t id = 0;
    script.at(ms(200), [&](Context& c) { id = c.submit(0, Side::Buy, 10'001, 100); });
    script.at(ms(300), [&](Context& c) { c.cancel(id); });
    SimConfig config = plain(true);
    config.cancel_latency_us = late ? ms(50) : ms(5);  // lands at 350 ms, or at 305 ms
    bt::Simulator sim(source, script, config);
    sim.run();
    if (late) {
      EXPECT_EQ(script.filled(id), 100u);
      EXPECT_TRUE(script.heard(id, OrderEvent::CancelRejected));
    } else {
      EXPECT_EQ(script.filled(id), 0u);
      EXPECT_TRUE(script.heard(id, OrderEvent::Cancelled));
    }
  }
}

TEST(Backtester, TheStrategyHearsOfThingsAfterTheFeedDelay) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      close_at(ms(600)),
  });
  struct Clock final : bt::Strategy {
    std::vector<std::int64_t> heard_at;
    void on_market(Context& c, const bt::MarketUpdate& update) override {
      heard_at.push_back(c.now() - update.time_us);
    }
  } clock;
  SimConfig config = plain(false);
  config.feed_latency_us = 2'500;
  bt::Simulator sim(source, clock, config);
  sim.run();
  ASSERT_EQ(clock.heard_at.size(), 1u);
  EXPECT_EQ(clock.heard_at[0], 2'500);
}

// --- Taking liquidity ---------------------------------------------------------------------

TEST(Backtester, AnArrivingOrderTakesTheBookLevelByLevel) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      close_at(ms(600)),
  });
  Script script;
  std::uint64_t ioc = 0;
  std::uint64_t market = 0;
  script.at(ms(200), [&](Context& c) { ioc = c.submit(0, Side::Buy, 10'010, 700, OrderKind::IOC); });
  script.at(ms(300), [&](Context& c) { market = c.submit(0, Side::Sell, 0, 200, OrderKind::Market); });
  SimConfig config = plain(false);
  config.depth_levels = 2;  // the touch and one level behind it
  bt::Simulator sim(source, script, config);
  const bt::RunResult result = sim.run();

  // 500 at the offer, then the next level's worth; the rest of 700 is dropped.
  std::uint32_t at_touch = 0;
  std::uint32_t behind = 0;
  for (const FillReport& fill : script.fills) {
    if (fill.order_id == ioc) {
      EXPECT_FALSE(fill.passive);
      (fill.price == 10'002 ? at_touch : behind) += fill.quantity;
      EXPECT_TRUE(fill.price == 10'002 || fill.price == 10'003);
    }
  }
  EXPECT_EQ(at_touch, 500u);
  EXPECT_GT(behind, 0u);
  EXPECT_LE(at_touch + behind, 700u);
  EXPECT_TRUE(script.heard(ioc, OrderEvent::Done));
  EXPECT_EQ(script.filled(market), 200u);
  EXPECT_EQ(result.aggressive_filled, at_touch + behind + 200u);
}

TEST(Backtester, RefusesWhatTheBookCannotTake) {
  auto source = source_of({open_at(0), quote_at(ms(100), 10'000, 10'002, 500, 500), close_at(ms(600))});
  Script script;
  std::uint64_t far = 0;
  std::uint64_t empty = 0;
  script.at(ms(200), [&](Context& c) { far = c.submit(0, Side::Buy, 1, 100); });     // below the book's prices
  script.at(ms(200), [&](Context& c) { empty = c.submit(0, Side::Buy, 10'000, 0); });  // nothing to trade
  bt::Simulator sim(source, script, plain(false));
  sim.run();
  EXPECT_TRUE(script.heard(far, OrderEvent::Rejected));
  EXPECT_TRUE(script.heard(empty, OrderEvent::Rejected));
}

// --- Money ----------------------------------------------------------------------------------

TEST(Backtester, CountsEachRoundTripAfterCommission) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      quote_at(ms(300), 10'010, 10'012, 500, 500),
      quote_at(ms(500), 9'990, 9'992, 500, 500),
      close_at(ms(900)),
  });
  Script script;
  script.at(ms(200), [&](Context& c) { c.submit(0, Side::Buy, 0, 100, OrderKind::Market); });   // 100.02
  script.at(ms(400), [&](Context& c) { c.submit(0, Side::Sell, 0, 100, OrderKind::Market); });  // 100.10
  script.at(ms(600), [&](Context& c) { c.submit(0, Side::Buy, 0, 50, OrderKind::Market); });    // 99.92, held to the close
  SimConfig config = plain(false);
  config.commission_per_share = 0.005;
  bt::Simulator sim(source, script, config);
  const bt::RunResult result = sim.run();

  ASSERT_EQ(result.trips.size(), 2u);
  // Bought at 100.02, sold at 100.10: $8.00, less $1.00 commission.
  EXPECT_NEAR(result.trips[0].pnl, 7.0, 1e-9);
  EXPECT_EQ(result.trips[0].side, Side::Buy);
  EXPECT_EQ(result.trips[0].shares, 100u);
  EXPECT_FALSE(result.trips[0].forced);
  // Bought 50 at 99.92 and still held at the close: sold for it at the bid, 99.90.
  EXPECT_NEAR(result.trips[1].pnl, (99.90 - 99.92) * 50 - 0.005 * 100, 1e-9);
  EXPECT_TRUE(result.trips[1].forced);
  EXPECT_NEAR(result.net_pnl, result.trips[0].pnl + result.trips[1].pnl, 1e-9);
  EXPECT_NEAR(result.commission, 0.005 * 300, 1e-9);
  ASSERT_EQ(result.session_pnl.size(), 1u);
  EXPECT_NEAR(result.session_pnl[0], result.net_pnl, 1e-9);
}

TEST(Backtester, ARoundTripThroughZeroIsTwoTrips) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'002, 500, 500),
      quote_at(ms(300), 10'010, 10'012, 500, 500),
      close_at(ms(900)),
  });
  Script script;
  script.at(ms(200), [&](Context& c) { c.submit(0, Side::Buy, 0, 100, OrderKind::Market); });
  script.at(ms(400), [&](Context& c) { c.submit(0, Side::Sell, 0, 300, OrderKind::Market); });  // long 100 to short 200
  bt::Simulator sim(source, script, plain(false));
  const bt::RunResult result = sim.run();
  ASSERT_EQ(result.trips.size(), 2u);
  EXPECT_EQ(result.trips[0].side, Side::Buy);
  EXPECT_NEAR(result.trips[0].pnl, (100.10 - 100.02) * 100, 1e-9);
  EXPECT_EQ(result.trips[1].side, Side::Sell);
  EXPECT_EQ(result.trips[1].shares, 200u);
  EXPECT_TRUE(result.trips[1].forced);
}

TEST(Backtester, TellsTheMarketAboutOurTrades) {
  auto source = source_of({open_at(0), quote_at(ms(100), 10'000, 10'002, 500, 500), close_at(ms(600))});
  Script script;
  script.at(ms(200), [&](Context& c) { c.submit(0, Side::Buy, 0, 120, OrderKind::Market); });
  bt::Simulator sim(source, script, plain(false));
  sim.run();
  ASSERT_EQ(source.flows.size(), 1u);
  EXPECT_EQ(source.flows[0].second, 120);
}

TEST(Backtester, ShowsTheStrategyTheTopOfTheBookItsOwnOrdersIncluded) {
  auto source = source_of({
      open_at(0),
      quote_at(ms(100), 10'000, 10'004, 500, 400),
      quote_at(ms(300), 10'000, 10'004, 500, 400),
      close_at(ms(600)),
  });
  Script script;
  script.at(ms(200), [&](Context& c) { c.submit(0, Side::Buy, 10'002, 70); });  // inside the spread
  SimConfig config = plain(false);
  config.depth_levels = 5;
  bt::Simulator sim(source, script, config);
  sim.run();
  ASSERT_EQ(script.market.size(), 2u);
  const bt::MarketUpdate& after = script.market[1];
  EXPECT_EQ(after.bids[0].price, 10'002);
  EXPECT_EQ(after.bids[0].quantity, 70u);
  EXPECT_EQ(after.bids[1].price, 10'000);
  EXPECT_EQ(after.bids[1].quantity, 500u);
  EXPECT_EQ(after.asks[0].price, 10'004);
  EXPECT_EQ(after.bid_levels, 5);
  EXPECT_EQ(after.ask_levels, 5);
}

// --- On the practice market: the same input, the same result --------------------------------

// Rests a bid at the touch whenever flat, sells at once when filled.
class Pinger final : public bt::Strategy {
 public:
  void on_market(Context& c, const bt::MarketUpdate& update) override {
    if (update.kind != TickKind::Quote || update.bid_levels == 0) {
      return;
    }
    if (position_ == 0 && !working_) {
      c.submit(update.symbol, Side::Buy, update.bids[0].price, 100);
      working_ = true;
    }
  }
  void on_fill(Context& c, const FillReport& fill) override {
    position_ += fill.side == Side::Buy ? static_cast<std::int64_t>(fill.quantity) : -static_cast<std::int64_t>(fill.quantity);
    if (fill.side == Side::Buy && fill.remaining == 0) {
      c.submit(fill.symbol, Side::Sell, 0, 100, OrderKind::Market);
    }
    if (fill.side == Side::Sell) {
      working_ = false;
    }
  }
  void on_session(Context&, std::uint16_t, bool) override {
    working_ = false;
    position_ = 0;
  }

 private:
  std::int64_t position_ = 0;
  bool working_ = false;
};

bt::RunResult ping(std::uint32_t seed, std::int64_t latency_us) {
  bt::PracticeMarketSource source({.seed = seed, .sessions = 2, .symbols = {"AAPL", "F"}, .ticks_per_session = 3'000});
  Pinger pinger;
  SimConfig config;
  config.order_latency_us = latency_us;
  bt::Simulator sim(source, pinger, config);
  return sim.run();
}

TEST(Backtester, GivesTheSameResultForTheSameInputEveryTime) {
  const bt::RunResult first = ping(5, 1'000);
  const bt::RunResult second = ping(5, 1'000);
  ASSERT_GT(first.trips.size(), 10u);
  ASSERT_EQ(first.trips.size(), second.trips.size());
  for (std::size_t index = 0; index < first.trips.size(); ++index) {
    ASSERT_EQ(first.trips[index].pnl, second.trips[index].pnl) << index;
    ASSERT_EQ(first.trips[index].closed_us, second.trips[index].closed_us) << index;
  }
  EXPECT_EQ(first.net_pnl, second.net_pnl);
  EXPECT_EQ(first.max_drawdown, second.max_drawdown);
  EXPECT_EQ(first.sessions, 2u);
  // And a different input gives a different result: another market, or orders
  // slow enough to land after the next tick (ticks are half a second apart).
  EXPECT_NE(ping(6, 1'000).net_pnl, first.net_pnl);
  EXPECT_NE(ping(5, 700'000).net_pnl, first.net_pnl);
}

}  // namespace
