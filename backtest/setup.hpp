#pragma once

#include <string>
#include <vector>

#include "backtest/config.hpp"
#include "backtest/simulator.hpp"
#include "backtest/strategy.hpp"
#include "backtest/ticks.hpp"

// Everything one run needs, read from the one settings file:
//
//   [market]      which practice market to generate (or ticks = a tick file)
//   [exchange]    the backtester's rules: delays, queue, commission, fills
//   [strategy]    HFT Strategy's settings (see strategy.hpp)
//   [validation]  how Phase 4 tests it (see validate.hpp)

namespace bt {

// Validation runs the [market] as one stream of `sessions + holdout_sessions`
// sessions, one day after another: the walk-forward steps use the first
// `sessions`, and the holdout is the rest.
struct ValidationConfig {
  std::uint32_t sessions = 20;         // for walk-forward
  std::uint32_t train_sessions = 4;    // each walk-forward step tunes on this many...
  std::uint32_t test_sessions = 2;     // ...and is then tested on the next this many
  std::uint32_t holdout_sessions = 6;  // never looked at while tuning; tested once, at the end
  std::vector<std::string> grid;       // settings tried while tuning, each "key=v1|v2|v3"
  std::uint32_t threads = 0;           // 0: one per core
};

struct RunSetup {
  PracticeMarketOptions market;
  std::string ticks_file;            // replay this tick file instead of generating a market
  SimConfig exchange;
  StrategyConfig strategy;
  ValidationConfig validation;
};

[[nodiscard]] inline bool parse_queue(const std::string& text, QueueModel& out) {
  if (text == "prorata" || text == "pro-rata") {
    out = QueueModel::ProRata;
  } else if (text == "back") {
    out = QueueModel::Back;
  } else if (text == "front") {
    out = QueueModel::Front;
  } else if (text == "hft") {
    out = QueueModel::Hft;
  } else {
    return false;
  }
  return true;
}

[[nodiscard]] inline const char* queue_name(QueueModel queue) {
  switch (queue) {
    case QueueModel::ProRata:
      return "prorata";
    case QueueModel::Back:
      return "back";
    case QueueModel::Front:
      return "front";
    case QueueModel::Hft:
      return "hft";
  }
  return "?";
}

// Reads every section. Problems (bad values, unknown names) are left in
// settings.problems() for the caller to report.
[[nodiscard]] inline RunSetup read_setup(Settings& settings) {
  RunSetup setup;
  PracticeMarketOptions& market = setup.market;
  market.seed = static_cast<std::uint32_t>(settings.integer("market.seed", market.seed));
  market.sessions = static_cast<std::uint32_t>(settings.integer("market.sessions", market.sessions));
  market.symbols = settings.list("market.symbols", market.symbols);
  if (market.symbols.size() == 1 && market.symbols[0] == "all") {
    market.symbols.clear();  // every listing in the practice market
  }
  market.impact = settings.flag("market.impact", market.impact);
  market.ticks_per_session =
      static_cast<std::uint32_t>(settings.integer("market.ticks_per_session", market.ticks_per_session));
  setup.ticks_file = settings.text("market.ticks", "");

  SimConfig& exchange = setup.exchange;
  exchange.feed_latency_us = settings.integer("exchange.feed_latency_us", exchange.feed_latency_us);
  exchange.order_latency_us = settings.integer("exchange.order_latency_us", exchange.order_latency_us);
  exchange.cancel_latency_us = settings.integer("exchange.cancel_latency_us", exchange.cancel_latency_us);
  exchange.commission_per_share = settings.number("exchange.commission_per_share", exchange.commission_per_share);
  const std::string queue = settings.text("exchange.queue", queue_name(exchange.queue));
  if (!parse_queue(queue, exchange.queue)) {
    settings.complain("exchange.queue = " + queue + " is not prorata, back, front or hft");
  }
  exchange.print_fills = settings.flag("exchange.print_fills", exchange.print_fills);
  exchange.depth_levels = static_cast<std::size_t>(
      settings.integer("exchange.depth_levels", static_cast<std::int64_t>(exchange.depth_levels)));

  setup.strategy.read(settings);
  setup.strategy.commission_per_share = exchange.commission_per_share;

  ValidationConfig& validation = setup.validation;
  validation.sessions = static_cast<std::uint32_t>(settings.integer("validation.sessions", validation.sessions));
  validation.train_sessions =
      static_cast<std::uint32_t>(settings.integer("validation.train_sessions", validation.train_sessions));
  validation.test_sessions =
      static_cast<std::uint32_t>(settings.integer("validation.test_sessions", validation.test_sessions));
  validation.holdout_sessions =
      static_cast<std::uint32_t>(settings.integer("validation.holdout_sessions", validation.holdout_sessions));
  validation.grid = settings.list("validation.grid", validation.grid);
  validation.threads = static_cast<std::uint32_t>(settings.integer("validation.threads", validation.threads));

  // Ranges a run cannot work outside of.
  const auto require = [&](bool good, const std::string& message) {
    if (!good) {
      settings.complain(message);
    }
  };
  require(market.sessions >= 1, "market.sessions must be at least 1");
  require(market.ticks_per_session >= 1, "market.ticks_per_session must be at least 1");
  require(exchange.feed_latency_us >= 0 && exchange.order_latency_us >= 0 && exchange.cancel_latency_us >= 0,
          "exchange latencies cannot be negative");
  require(exchange.commission_per_share >= 0, "exchange.commission_per_share cannot be negative");
  require(exchange.depth_levels >= 1 && exchange.depth_levels <= kDepthLevels, "exchange.depth_levels must be 1 to 10");
  const StrategyConfig& strategy = setup.strategy;
  require(strategy.obi_levels >= 1 && strategy.obi_levels <= kViewLevels, "strategy.obi_levels must be 1 to 5");
  require(strategy.flow_window >= 1, "strategy.flow_window must be at least 1");
  require(strategy.order_size >= 1, "strategy.order_size must be at least 1");
  require(strategy.target_ticks >= 1, "strategy.target_ticks must be at least 1");
  require(strategy.breakeven_ticks >= 0 && strategy.stop_ticks >= 0, "strategy breakeven and stop ticks cannot be negative");
  require(strategy.max_volatility >= 0 && strategy.vol_halflife_ms >= 0, "strategy volatility settings cannot be negative");
  require(strategy.time_stop_ms >= 1 && strategy.entry_timeout_ms >= 1, "strategy time limits must be at least 1 ms");
  require(strategy.min_spread_ticks >= 1 && strategy.max_spread_ticks >= strategy.min_spread_ticks,
          "strategy spread range must be 1 or more, low to high");
  require(strategy.max_position >= 1 && strategy.max_total_position >= 1, "strategy position limits must be at least 1");
  require(strategy.max_session_loss > 0 && strategy.max_run_loss > 0, "strategy loss limits must be above 0");
  require(strategy.max_orders_per_second >= 1, "strategy.max_orders_per_second must be at least 1");
  require(validation.train_sessions >= 1 && validation.test_sessions >= 1,
          "validation train and test sessions must be at least 1");
  require(validation.sessions >= validation.train_sessions + validation.test_sessions,
          "validation.sessions must cover at least one train and test step");
  require(validation.holdout_sessions >= 1, "validation.holdout_sessions must be at least 1");
  return setup;
}

}  // namespace bt
