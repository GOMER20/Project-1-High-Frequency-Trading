#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "backtest/config.hpp"
#include "backtest/practice_market.hpp"
#include "backtest/simulator.hpp"

// HFT Strategy: a market maker that only quotes when the book says a quick
// fill is likely to be a good one.
//
// For each symbol it reads three things from the market, on every update:
//
//   order-book imbalance  how much more is bid than offered over the top five
//                         levels: (bids - offers) / (bids + offers), from -1 to 1.
//                         Its own orders are left out.
//   micro-price           the mid nudged towards the thinner side:
//                         (bid x offer size + offer x bid size) / (both sizes).
//                         How far it sits from the mid, in ticks.
//   trade flow            of the last N trades, buying minus selling volume over
//                         the total (signed flow), and the same without the sign
//                         (toxicity, a VPIN-like measure: one-sided flow means
//                         someone knows something, and a resting order is what
//                         they trade against).
//
// plus, optionally, a short trend: the recent drift of the mid.
//
// These combine into one score, positive when the next move is more likely up.
// Then, per symbol:
//
//   flat      If the score is above `enter_score` it joins the best bid; below
//             minus that, the best offer. Not if trade flow is too toxic, the
//             spread is outside its range, or any risk limit says no.
//   entering  Its order waits in the queue. It is withdrawn if the score fades,
//             if the market's touch moves off its price ("the queue turns
//             against it"), or if it has waited `entry_timeout_ms`.
//   holding   Once filled it offers the position out `target_ticks` away. If
//             the market moves `breakeven_ticks` against it, the offer comes
//             back to the entry price, to scratch the trade without a loss; at
//             `stop_ticks` against it, it gets out at once. If none of that has
//             happened within `time_stop_ms`, or the score turns
//             `scratch_score` against the position, it gets out at once at the
//             market.
//
// Limits: shares per symbol and in all; orders a second; and a kill switch
// that stops the session (and, past a larger loss, the whole run) and gets
// out of everything.
//
// With `random_entries` it enters on a coin toss, using none of what it reads
// from the market (neither the score nor the toxicity and volatility filters),
// with every other rule the same: the baseline a real edge has to beat. With
// `random_filters` as well, the filters stay and only the side is a coin toss,
// which shows how much of an edge comes from the filters alone.

namespace bt {

struct StrategyConfig {
  // --- Signals ---
  std::size_t obi_levels = 5;          // levels a side in the imbalance
  std::size_t flow_window = 50;        // trades in the flow and toxicity window
  double trend_halflife_ms = 5'000;    // how fast the trend forgets
  double w_obi = 1.0;                  // weights in the score
  double w_micro = 1.0;
  double w_flow = 0.5;
  double w_trend = 0.0;
  double vol_halflife_ms = 60'000;     // how fast the volatility measure forgets
  // --- Entry ---
  double enter_score = 0.4;
  double cancel_score = 0.1;           // a working entry is withdrawn once the score is below this
  double max_toxicity = 0.6;
  // Only enter when the mid has been moving at most this many ticks a second
  // (0: any). With a one-tick spread, a market maker earns the spread only if
  // the price moves slowly compared with how fast the queue fills.
  double max_volatility = 0;
  // Which side to join: "score" (the side the score points to, when it is
  // past enter_score) or "queue" (the side with less ahead in the queue, so
  // the quicker to fill; the score still has to clear enter_score on neither
  // side against it).
  bool queue_side = false;
  std::int64_t min_spread_ticks = 1;
  std::int64_t max_spread_ticks = 3;
  std::uint32_t order_size = 100;
  std::int64_t entry_timeout_ms = 2'000;
  // --- Exit ---
  std::int64_t target_ticks = 1;
  std::int64_t time_stop_ms = 3'000;
  double scratch_score = 0.4;          // out at once if the score is this far against the position (0: never)
  std::int64_t breakeven_ticks = 0;    // market this many ticks against: offer out at the entry price instead (0: never)
  std::int64_t stop_ticks = 0;         // market this many ticks against: out at once (0: never)
  // --- Risk ---
  std::int64_t max_position = 200;     // shares, per symbol
  std::int64_t max_total_position = 1'000;
  double max_session_loss = 500;       // dollars: the kill switch for a session
  double max_run_loss = 2'000;         // dollars: the kill switch for the whole run
  std::int64_t max_orders_per_second = 20;
  // --- Baseline ---
  bool random_entries = false;
  bool random_filters = false;
  std::uint32_t random_seed = 1;
  // Not read from [strategy]: the exchange's commission, so that the kill
  // switch counts what trading costs.
  double commission_per_share = 0;
  // Not read from [strategy]: whether the market data shows the strategy's
  // own orders in the book. The backtester's does, and they are taken out of
  // the signals; Gomer Trading's feed does not, so live there is nothing to
  // take out.
  bool feed_shows_own_orders = true;

  // Reads the [strategy] section.
  void read(Settings& settings) {
    obi_levels = static_cast<std::size_t>(settings.integer("strategy.obi_levels", static_cast<std::int64_t>(obi_levels)));
    flow_window = static_cast<std::size_t>(settings.integer("strategy.flow_window", static_cast<std::int64_t>(flow_window)));
    trend_halflife_ms = settings.number("strategy.trend_halflife_ms", trend_halflife_ms);
    w_obi = settings.number("strategy.w_obi", w_obi);
    w_micro = settings.number("strategy.w_micro", w_micro);
    w_flow = settings.number("strategy.w_flow", w_flow);
    w_trend = settings.number("strategy.w_trend", w_trend);
    vol_halflife_ms = settings.number("strategy.vol_halflife_ms", vol_halflife_ms);
    enter_score = settings.number("strategy.enter_score", enter_score);
    cancel_score = settings.number("strategy.cancel_score", cancel_score);
    max_toxicity = settings.number("strategy.max_toxicity", max_toxicity);
    max_volatility = settings.number("strategy.max_volatility", max_volatility);
    const std::string side = settings.text("strategy.entry_side", queue_side ? "queue" : "score");
    if (side == "queue" || side == "score") {
      queue_side = side == "queue";
    } else {
      settings.complain("strategy.entry_side = " + side + " is not score or queue");
    }
    min_spread_ticks = settings.integer("strategy.min_spread_ticks", min_spread_ticks);
    max_spread_ticks = settings.integer("strategy.max_spread_ticks", max_spread_ticks);
    order_size = static_cast<std::uint32_t>(settings.integer("strategy.order_size", order_size));
    entry_timeout_ms = settings.integer("strategy.entry_timeout_ms", entry_timeout_ms);
    target_ticks = settings.integer("strategy.target_ticks", target_ticks);
    time_stop_ms = settings.integer("strategy.time_stop_ms", time_stop_ms);
    scratch_score = settings.number("strategy.scratch_score", scratch_score);
    breakeven_ticks = settings.integer("strategy.breakeven_ticks", breakeven_ticks);
    stop_ticks = settings.integer("strategy.stop_ticks", stop_ticks);
    max_position = settings.integer("strategy.max_position", max_position);
    max_total_position = settings.integer("strategy.max_total_position", max_total_position);
    max_session_loss = settings.number("strategy.max_session_loss", max_session_loss);
    max_run_loss = settings.number("strategy.max_run_loss", max_run_loss);
    max_orders_per_second = settings.integer("strategy.max_orders_per_second", max_orders_per_second);
    random_entries = settings.flag("strategy.random_entries", random_entries);
    random_filters = settings.flag("strategy.random_filters", random_filters);
    random_seed = static_cast<std::uint32_t>(settings.integer("strategy.random_seed", random_seed));
  }
};

// What the strategy did, beyond the trades themselves.
struct StrategyCounters {
  std::uint64_t entries = 0;          // entry orders sent
  std::uint64_t entries_withdrawn = 0;
  std::uint64_t entries_filled = 0;   // entry orders that filled at least in part
  std::uint64_t target_exits = 0;     // positions closed at the target
  std::uint64_t time_stops = 0;
  std::uint64_t scratches = 0;        // closed early because the signal turned
  std::uint64_t breakevens = 0;       // exits brought back to the entry price
  std::uint64_t price_stops = 0;      // closed early because the price moved against it
  std::uint64_t kill_switches = 0;
  std::uint64_t refused_by_limits = 0;  // entries a risk limit stopped
  std::uint64_t too_toxic = 0;        // entries the toxicity filter stopped
  std::uint64_t too_volatile = 0;     // entries the volatility filter stopped
};

// Does the book say anything about where the price goes next? For every
// quote, each signal is paired with how far the mid then moves over the next
// second and the next five, and the correlation is kept. Near zero means the
// signal is noise: no rule built on it can have an edge.
class SignalCheck {
 public:
  static constexpr std::size_t kSignals = 5;  // imbalance, micro-price, trade flow, trend, the score
  static constexpr std::size_t kScore = kSignals - 1;
  static constexpr std::array<const char*, kSignals> kNames{"order-book imbalance", "micro-price", "trade flow",
                                                            "trend", "combined score"};
  static constexpr std::array<std::int64_t, 2> kHorizonsUs{1'000'000, 5'000'000};

  struct Sums {
    double n = 0, sx = 0, sy = 0, sxy = 0, sxx = 0, syy = 0;
    void add(double x, double y) noexcept {
      n += 1;
      sx += x;
      sy += y;
      sxy += x * y;
      sxx += x * x;
      syy += y * y;
    }
    [[nodiscard]] double correlation() const noexcept {
      const double cov = n * sxy - sx * sy;
      const double vx = n * sxx - sx * sx;
      const double vy = n * syy - sy * sy;
      return vx > 0 && vy > 0 ? cov / std::sqrt(vx * vy) : 0;
    }
    // The move, in cents, that one unit of the signal goes with (least squares).
    [[nodiscard]] double slope() const noexcept {
      const double vx = n * sxx - sx * sx;
      return vx > 0 ? (n * sxy - sx * sy) / vx : 0;
    }
  };

  void observe(std::uint16_t symbol, std::int64_t time_us, const std::array<double, kSignals>& signals, double mid) {
    if (pending_.size() <= symbol) {
      pending_.resize(symbol + 1u);
    }
    for (std::size_t horizon = 0; horizon < kHorizonsUs.size(); ++horizon) {
      auto& waiting = pending_[symbol][horizon];
      while (!waiting.empty() && time_us - waiting.front().time_us >= kHorizonsUs[horizon]) {
        for (std::size_t signal = 0; signal < kSignals; ++signal) {
          sums_[horizon][signal].add(waiting.front().signals[signal], mid - waiting.front().mid);
        }
        waiting.pop_front();
      }
      waiting.push_back(Pending{time_us, signals, mid});
    }
  }

  // The day is over: what was waiting for a later price can never have one.
  void session_over(std::uint16_t symbol) {
    if (symbol < pending_.size()) {
      for (auto& waiting : pending_[symbol]) {
        waiting.clear();
      }
    }
  }

  [[nodiscard]] const Sums& sums(std::size_t horizon, std::size_t signal) const { return sums_[horizon][signal]; }

 private:
  struct Pending {
    std::int64_t time_us;
    std::array<double, kSignals> signals;
    double mid;
  };
  std::vector<std::array<std::deque<Pending>, kHorizonsUs.size()>> pending_;
  std::array<std::array<Sums, kSignals>, kHorizonsUs.size()> sums_{};
};

class HftStrategy final : public Strategy {
 public:
  explicit HftStrategy(StrategyConfig config) : config_(config), random_(config.random_seed) {}

  [[nodiscard]] const StrategyCounters& counters() const noexcept { return counters_; }

  // Measures how well the signals predict the price, as the run goes.
  void check_signals(SignalCheck* check) noexcept { check_ = check; }

  // Stops trading for good: no new entries, every entry withdrawn, every
  // position closed at the market. For the end of a live session, a kill
  // switch from outside, or the program being asked to stop.
  void halt(Context& context) {
    run_killed_ = true;
    everything_out(context);
  }

  // Whether it has stopped trading: halted, or a kill switch tripped.
  [[nodiscard]] bool stopped() const noexcept { return run_killed_; }
  [[nodiscard]] bool session_stopped() const noexcept { return session_killed_; }

  // Nothing held and nothing working, in any symbol.
  [[nodiscard]] bool idle() const noexcept {
    return std::all_of(symbols_.begin(), symbols_.end(), [](const SymbolState& state) {
      return state.position == 0 && state.entry_id == 0 && state.exit_id == 0 && state.flatten_id == 0;
    });
  }

  struct Holding {
    std::uint16_t symbol;
    std::int64_t shares;       // + long, - short
    std::int64_t entry_price;  // cents
    std::int64_t mid2;         // twice the mid it is marked at, in cents
  };

  // What it holds now, as it sees it.
  [[nodiscard]] std::vector<Holding> holdings() const {
    std::vector<Holding> held;
    for (std::size_t symbol = 0; symbol < symbols_.size(); ++symbol) {
      const SymbolState& state = symbols_[symbol];
      if (state.position != 0) {
        held.push_back(Holding{static_cast<std::uint16_t>(symbol), state.position, state.entry_price,
                               state.market_bid + state.market_ask});
      }
    }
    return held;
  }

  // Profit or loss as the strategy counts it (for its kill switches), what is
  // held marked at the mid: this session, and the whole run.
  [[nodiscard]] double session_pnl() const { return marked(session_cash_); }
  [[nodiscard]] double run_pnl() const { return marked(run_cash_); }

  void on_start(Context& context) override {
    symbols_.assign(context.symbols().size(), SymbolState{});
  }

  void on_session(Context&, std::uint16_t symbol, bool open) override {
    SymbolState& state = symbols_[symbol];
    if (open) {
      const std::uint64_t generation = state.generation;
      state = SymbolState{};
      state.generation = generation + 1;
      state.session_open = true;
      if (symbol == 0) {
        session_killed_ = false;
        session_cash_ = 0;
      }
    } else {
      state.session_open = false;
      // The exchange cancels what is left and flattens; the reports follow.
      if (check_ != nullptr) {
        check_->session_over(symbol);
      }
    }
  }

  void on_market(Context& context, const MarketUpdate& update) override {
    SymbolState& state = symbols_[update.symbol];
    if (update.kind == TickKind::Trade) {
      record_print(state, update);
      return;
    }
    if (update.kind != TickKind::Quote) {
      return;
    }
    read_book(state, update);
    if (check_ != nullptr && state.market_bid > 0 && state.market_ask > 0) {
      check_->observe(update.symbol, update.time_us, {state.obi, state.micro, state.flow, state.trend, state.score},
                      static_cast<double>(state.market_bid + state.market_ask) / 2.0);
    }
    if (state.position != 0) {
      check_losses(context);  // a loss can grow without a fill: what is held is marked anew
    }
    decide(context, update.symbol);
  }

  void on_fill(Context& context, const FillReport& fill) override {
    SymbolState& state = symbols_[fill.symbol];
    const std::int64_t signed_shares =
        fill.side == lob::Side::Buy ? static_cast<std::int64_t>(fill.quantity) : -static_cast<std::int64_t>(fill.quantity);
    const double value = static_cast<double>(fill.price) / 100.0 * static_cast<double>(fill.quantity);
    const double cash = (fill.side == lob::Side::Buy ? -value : value) -
                        config_.commission_per_share * static_cast<double>(fill.quantity);
    session_cash_ += cash;
    run_cash_ += cash;
    const std::int64_t before = state.position;
    state.position += signed_shares;
    total_position_ += std::abs(state.position) - std::abs(before);
    if (before == 0 && state.position != 0) {
      state.opened_us = fill.time_us;
      state.entry_price = fill.price;
      if (fill.order_id != 0) {
        // The clock on the position starts now.
        context.wake_at(fill.time_us + config_.time_stop_ms * 1'000, token(fill.symbol, Wake::TimeStop));
      }
    }

    if (fill.order_id != 0 && fill.order_id == state.entry_id) {
      if (!state.entry_filled) {
        state.entry_filled = true;
        ++counters_.entries_filled;
      }
      state.entry_resting = std::min(state.entry_resting, fill.remaining);
      if (fill.remaining > 0 && !state.entry_cancel_sent) {
        // Some of it is enough: no more exposure on this side.
        context.cancel(state.entry_id);
        state.entry_cancel_sent = true;
      }
      if (fill.remaining == 0) {
        state.entry_id = 0;
        place_exit(context, fill.symbol);
      }
    } else if (fill.order_id != 0 && fill.order_id == state.exit_id) {
      state.exit_resting = std::min(state.exit_resting, fill.remaining);
      if (state.position == 0) {
        ++counters_.target_exits;
        state.exit_id = 0;
        flat(state);
      }
    } else if (fill.order_id != 0 && fill.order_id == state.flatten_id) {
      if (state.position == 0) {
        state.flatten_id = 0;
        flat(state);
      }
    }
    check_losses(context);
  }

  void on_order(Context& context, const OrderReport& report) override {
    SymbolState& state = symbols_[report.symbol];
    if (report.order_id == state.entry_id) {
      switch (report.event) {
        case OrderEvent::Rejected:
        case OrderEvent::Cancelled:
        case OrderEvent::CancelRejected:
          // Gone, one way or another. Whatever filled is held, and goes out next.
          state.entry_id = 0;
          if (state.position != 0) {
            place_exit(context, report.symbol);
          } else {
            flat(state);
          }
          break;
        case OrderEvent::Accepted:
          state.entry_resting = report.resting;
          break;
        case OrderEvent::Done:
          break;
      }
    } else if (report.order_id == state.exit_id) {
      if (report.event == OrderEvent::Accepted) {
        state.exit_resting = report.resting;
      } else if (report.event == OrderEvent::Cancelled || report.event == OrderEvent::CancelRejected ||
          report.event == OrderEvent::Rejected) {
        state.exit_id = 0;
        if (state.flatten_wanted && state.position != 0) {
          flatten_now(context, report.symbol);
        } else if (state.position != 0 && state.session_open) {
          place_exit(context, report.symbol);
        }
      }
    } else if (report.order_id == state.flatten_id && report.event == OrderEvent::Done) {
      state.flatten_id = 0;
      if (state.position != 0 && state.session_open) {
        flatten_now(context, report.symbol);  // the market was thin; again
      }
    }
  }

  void on_timer(Context& context, std::uint64_t value) override {
    const auto symbol = static_cast<std::uint16_t>(value >> 40);
    const auto kind = static_cast<Wake>((value >> 32) & 0xFF);
    const auto generation = static_cast<std::uint32_t>(value & 0xFFFF'FFFF);
    if (symbol >= symbols_.size()) {
      return;
    }
    SymbolState& state = symbols_[symbol];
    if (generation != static_cast<std::uint32_t>(state.generation)) {
      return;  // from an earlier session
    }
    if (kind == Wake::TimeStop) {
      if (state.position != 0 && context.now() - state.opened_us >= config_.time_stop_ms * 1'000 &&
          !state.flatten_wanted) {
        ++counters_.time_stops;
        get_out(context, symbol);
      }
    } else if (kind == Wake::EntryTimeout) {
      if (state.entry_id != 0 && !state.entry_cancel_sent && state.position == 0 &&
          context.now() - state.entry_sent_us >= config_.entry_timeout_ms * 1'000) {
        withdraw(context, state);
      }
    }
  }

 private:
  enum class Wake : std::uint8_t { TimeStop = 1, EntryTimeout = 2 };

  struct SymbolState {
    bool session_open = false;
    std::uint64_t generation = 0;
    // The book, as last seen.
    std::int64_t bid = 0;
    std::int64_t ask = 0;
    std::uint64_t market_bid_size = 0;  // without our own orders
    std::uint64_t market_ask_size = 0;
    std::int64_t market_bid = 0;        // the best price that is not ours alone
    std::int64_t market_ask = 0;
    // Signals.
    double obi = 0;
    double micro = 0;
    double flow = 0;
    double toxicity = 0;
    double trend = 0;
    // Ticks a second the mid has been moving: an average that forgets, and how
    // much of its weight it has gathered so far. It starts from nothing, so
    // until it has gathered all of it, the average alone reads too low; it is
    // divided by its weight to be fair from the first quote.
    double volatility = 0;
    double volatility_weight = 0;
    std::uint32_t volatility_samples = 0;
    double score = 0;
    std::int64_t last_mid2 = 0;
    std::int64_t last_quote_us = 0;
    std::deque<std::pair<lob::Side, std::uint32_t>> prints;
    std::uint64_t buy_volume = 0;
    std::uint64_t sell_volume = 0;
    // Orders and position.
    std::uint64_t entry_id = 0;
    lob::Side entry_side = lob::Side::Buy;
    std::int64_t entry_order_price = 0;
    std::uint32_t entry_resting = 0;    // in the book, as far as we know: 0 until it is accepted
    std::int64_t entry_sent_us = 0;
    bool entry_cancel_sent = false;
    bool entry_filled = false;
    std::uint64_t exit_id = 0;
    lob::Side exit_side = lob::Side::Sell;
    std::int64_t exit_price = 0;
    std::uint32_t exit_resting = 0;     // in the book, as far as we know
    std::uint64_t flatten_id = 0;
    bool flatten_wanted = false;
    bool at_breakeven = false;          // the exit is, or is to be, at the entry price
    std::int64_t position = 0;
    std::int64_t entry_price = 0;
    std::int64_t opened_us = 0;
  };

  [[nodiscard]] std::uint64_t token(std::uint16_t symbol, Wake kind) const {
    return (static_cast<std::uint64_t>(symbol) << 40) | (static_cast<std::uint64_t>(kind) << 32) |
           static_cast<std::uint32_t>(symbols_[symbol].generation);
  }

  void record_print(SymbolState& state, const MarketUpdate& update) {
    state.prints.emplace_back(update.aggressor, update.trade_size);
    (update.aggressor == lob::Side::Buy ? state.buy_volume : state.sell_volume) += update.trade_size;
    while (state.prints.size() > config_.flow_window) {
      const auto [side, size] = state.prints.front();
      (side == lob::Side::Buy ? state.buy_volume : state.sell_volume) -= size;
      state.prints.pop_front();
    }
    const double total = static_cast<double>(state.buy_volume + state.sell_volume);
    state.flow = total > 0 ? (static_cast<double>(state.buy_volume) - static_cast<double>(state.sell_volume)) / total : 0;
    state.toxicity = std::abs(state.flow);
  }

  // The signals from a fresh look at the book, with our own orders taken out.
  void read_book(SymbolState& state, const MarketUpdate& update) {
    const auto ours_at = [&](lob::Side side, std::int64_t price) -> std::uint64_t {
      if (!config_.feed_shows_own_orders) {
        return 0;
      }
      std::uint64_t ours = 0;
      if (state.entry_id != 0 && state.entry_side == side && state.entry_order_price == price) {
        ours += state.entry_resting;
      }
      if (state.exit_id != 0 && state.exit_side == side && state.exit_price == price) {
        ours += state.exit_resting;
      }
      return ours;
    };
    std::uint64_t bids = 0;
    std::uint64_t asks = 0;
    state.market_bid = 0;
    state.market_ask = 0;
    for (std::size_t level = 0; level < update.bid_levels && level < config_.obi_levels; ++level) {
      const std::uint64_t theirs = update.bids[level].quantity - std::min(update.bids[level].quantity, ours_at(lob::Side::Buy, update.bids[level].price));
      if (theirs > 0 && state.market_bid == 0) {
        state.market_bid = update.bids[level].price;
        state.market_bid_size = theirs;
      }
      bids += theirs;
    }
    for (std::size_t level = 0; level < update.ask_levels && level < config_.obi_levels; ++level) {
      const std::uint64_t theirs = update.asks[level].quantity - std::min(update.asks[level].quantity, ours_at(lob::Side::Sell, update.asks[level].price));
      if (theirs > 0 && state.market_ask == 0) {
        state.market_ask = update.asks[level].price;
        state.market_ask_size = theirs;
      }
      asks += theirs;
    }
    state.bid = update.bid_levels > 0 ? update.bids[0].price : 0;
    state.ask = update.ask_levels > 0 ? update.asks[0].price : 0;
    state.obi = bids + asks > 0 ? (static_cast<double>(bids) - static_cast<double>(asks)) / static_cast<double>(bids + asks) : 0;

    if (state.market_bid > 0 && state.market_ask > 0) {
      const double bid = static_cast<double>(state.market_bid);
      const double ask = static_cast<double>(state.market_ask);
      const double bid_size = static_cast<double>(state.market_bid_size);
      const double ask_size = static_cast<double>(state.market_ask_size);
      const double micro_price = (bid * ask_size + ask * bid_size) / (bid_size + ask_size);
      state.micro = micro_price - (bid + ask) / 2;  // ticks, a tick being a cent

      const std::int64_t mid2 = state.market_bid + state.market_ask;
      if (state.last_mid2 != 0) {
        const double elapsed_ms = static_cast<double>(update.time_us - state.last_quote_us) / 1'000.0;
        const double keep = config_.trend_halflife_ms > 0 ? std::exp2(-elapsed_ms / config_.trend_halflife_ms) : 0;
        state.trend = keep * state.trend + (1 - keep) * static_cast<double>(mid2 - state.last_mid2) / 2.0;
        const double forget = config_.vol_halflife_ms > 0 ? std::exp2(-elapsed_ms / config_.vol_halflife_ms) : 0;
        const double rate = static_cast<double>(std::abs(mid2 - state.last_mid2)) / 2.0 / std::max(elapsed_ms / 1'000.0, 1e-3);
        state.volatility = forget * state.volatility + (1 - forget) * rate;
        state.volatility_weight = forget * state.volatility_weight + (1 - forget);
        ++state.volatility_samples;
      }
      state.last_mid2 = mid2;
      state.last_quote_us = update.time_us;
    }
    state.score = config_.w_obi * state.obi + config_.w_micro * state.micro + config_.w_flow * state.flow +
                  config_.w_trend * state.trend;
  }

  void decide(Context& context, std::uint16_t symbol) {
    SymbolState& state = symbols_[symbol];
    if (!state.session_open || state.market_bid == 0 || state.market_ask == 0) {
      return;
    }
    if (state.position != 0) {
      if (state.flatten_wanted) {
        return;
      }
      // Holding. How far the market has gone against the position, in ticks:
      // for a long, how far the bid is below where it was bought.
      const bool long_position = state.position > 0;
      const std::int64_t adverse =
          long_position ? state.entry_price - state.market_bid : state.market_ask - state.entry_price;
      if (config_.stop_ticks > 0 && adverse >= config_.stop_ticks) {
        ++counters_.price_stops;
        get_out(context, symbol);
        return;
      }
      if (config_.breakeven_ticks > 0 && adverse >= config_.breakeven_ticks && !state.at_breakeven) {
        // Offer it out where it was bought instead: the exit is cancelled and
        // comes back at the entry price when the cancel is confirmed.
        state.at_breakeven = true;
        ++counters_.breakevens;
        if (state.exit_id != 0) {
          context.cancel(state.exit_id);
        }
      }
      // Out at once if the signal has turned against us.
      const double against = long_position ? -state.score : state.score;
      if (config_.scratch_score > 0 && against >= config_.scratch_score) {
        ++counters_.scratches;
        get_out(context, symbol);
      }
      return;
    }
    if (state.entry_id != 0) {
      // Entering: withdraw if the case for it has gone, or the touch has moved off our price.
      if (state.entry_cancel_sent) {
        return;
      }
      const bool buying = state.entry_side == lob::Side::Buy;
      const double support = buying ? state.score : -state.score;
      const std::int64_t touch = buying ? state.market_bid : state.market_ask;
      const bool moved = touch != state.entry_order_price;
      if ((!config_.random_entries && support < config_.cancel_score) || moved) {
        withdraw(context, state);
      }
      return;
    }
    if (state.flatten_id != 0 || state.exit_id != 0 || killed()) {
      return;
    }

    // Flat: is there a case for joining one side?
    const std::int64_t spread = state.market_ask - state.market_bid;
    if (spread < config_.min_spread_ticks || spread > config_.max_spread_ticks) {
      return;
    }
    int side = 0;  // +1 buy, -1 sell
    if (config_.random_entries) {
      side = random_() < 0.5 ? 1 : -1;
    } else if (config_.queue_side) {
      // The shorter queue, unless the score is clearly against that side.
      side = state.market_bid_size <= state.market_ask_size ? 1 : -1;
      if ((side > 0 ? -state.score : state.score) >= config_.enter_score) {
        side = 0;
      }
    } else if (state.score >= config_.enter_score) {
      side = 1;
    } else if (state.score <= -config_.enter_score) {
      side = -1;
    }
    if (side == 0) {
      return;
    }
    const bool filtered = !config_.random_entries || config_.random_filters;
    if (filtered && config_.max_volatility > 0 &&
        (state.volatility_samples < 20 || state.volatility_weight <= 0 ||
         state.volatility / state.volatility_weight > config_.max_volatility)) {
      ++counters_.too_volatile;
      return;
    }
    if (filtered && state.toxicity > config_.max_toxicity) {
      ++counters_.too_toxic;
      return;
    }
    if (static_cast<std::int64_t>(config_.order_size) > config_.max_position ||
        total_position_ + static_cast<std::int64_t>(config_.order_size) > config_.max_total_position ||
        !rate_allows(context)) {
      ++counters_.refused_by_limits;
      return;
    }
    state.entry_side = side > 0 ? lob::Side::Buy : lob::Side::Sell;
    state.entry_order_price = side > 0 ? state.market_bid : state.market_ask;
    state.entry_resting = 0;
    state.entry_cancel_sent = false;
    state.entry_filled = false;
    state.entry_sent_us = context.now();
    state.entry_id = send(context, symbol, state.entry_side, state.entry_order_price, config_.order_size, OrderKind::Limit);
    ++counters_.entries;
    context.wake_at(context.now() + config_.entry_timeout_ms * 1'000, token(symbol, Wake::EntryTimeout));
  }

  void withdraw(Context& context, SymbolState& state) {
    context.cancel(state.entry_id);
    state.entry_cancel_sent = true;
    ++counters_.entries_withdrawn;
  }

  // Offers the position out `target_ticks` from where it was entered.
  void place_exit(Context& context, std::uint16_t symbol) {
    SymbolState& state = symbols_[symbol];
    if (state.position == 0 || state.exit_id != 0 || state.flatten_id != 0 || !state.session_open) {
      return;
    }
    if (state.flatten_wanted || killed()) {
      flatten_now(context, symbol);
      return;
    }
    const bool long_position = state.position > 0;
    state.exit_side = long_position ? lob::Side::Sell : lob::Side::Buy;
    state.exit_price = state.at_breakeven ? state.entry_price
                                          : state.entry_price + (long_position ? config_.target_ticks : -config_.target_ticks);
    state.exit_resting = 0;
    state.exit_id = send(context, symbol, state.exit_side, state.exit_price,
                         static_cast<std::uint32_t>(std::abs(state.position)), OrderKind::Limit);
  }

  // Out of the position now, at the market: the exit order is cancelled
  // first, and the market order goes when the cancel is confirmed, so the two
  // can never both fill.
  void get_out(Context& context, std::uint16_t symbol) {
    SymbolState& state = symbols_[symbol];
    state.flatten_wanted = true;
    if (state.entry_id != 0 && !state.entry_cancel_sent) {
      context.cancel(state.entry_id);
      state.entry_cancel_sent = true;
    }
    if (state.exit_id != 0) {
      context.cancel(state.exit_id);
    } else if (state.position != 0 && state.flatten_id == 0) {
      flatten_now(context, symbol);
    }
  }

  void flatten_now(Context& context, std::uint16_t symbol) {
    SymbolState& state = symbols_[symbol];
    if (state.position == 0 || state.flatten_id != 0) {
      return;
    }
    state.flatten_id = send(context, symbol, state.position > 0 ? lob::Side::Sell : lob::Side::Buy, 0,
                            static_cast<std::uint32_t>(std::abs(state.position)), OrderKind::Market);
  }

  void flat(SymbolState& state) {
    state.flatten_wanted = false;
    state.at_breakeven = false;
    state.entry_filled = false;
    state.entry_cancel_sent = false;
    ++state.generation;  // any timer still set for the old position is stale now
  }

  std::uint64_t send(Context& context, std::uint16_t symbol, lob::Side side, std::int64_t price, std::uint32_t quantity,
                     OrderKind kind) {
    sent_.push_back(context.now());
    return context.submit(symbol, side, price, quantity, kind);
  }

  [[nodiscard]] bool rate_allows(Context& context) {
    while (!sent_.empty() && sent_.front() <= context.now() - 1'000'000) {
      sent_.pop_front();
    }
    return static_cast<std::int64_t>(sent_.size()) < config_.max_orders_per_second;
  }

  // Profit or loss so far, marking what is held at the mid.
  [[nodiscard]] double marked(double cash) const {
    double total = cash;
    for (const SymbolState& state : symbols_) {
      if (state.position != 0 && state.market_bid > 0 && state.market_ask > 0) {
        total += static_cast<double>(state.position) * static_cast<double>(state.market_bid + state.market_ask) / 200.0;
      }
    }
    return total;
  }

  [[nodiscard]] bool killed() const noexcept { return session_killed_ || run_killed_; }

  void check_losses(Context& context) {
    if (!session_killed_ && marked(session_cash_) <= -config_.max_session_loss) {
      session_killed_ = true;
      ++counters_.kill_switches;
      everything_out(context);
    }
    if (!run_killed_ && marked(run_cash_) <= -config_.max_run_loss) {
      run_killed_ = true;
      ++counters_.kill_switches;
      everything_out(context);
    }
  }

  void everything_out(Context& context) {
    for (std::size_t symbol = 0; symbol < symbols_.size(); ++symbol) {
      SymbolState& state = symbols_[symbol];
      if (state.entry_id != 0 && !state.entry_cancel_sent) {
        withdraw(context, state);
      }
      if (state.position != 0 && !state.flatten_wanted) {
        get_out(context, static_cast<std::uint16_t>(symbol));
      }
    }
  }

  StrategyConfig config_;
  Mulberry32 random_;
  std::vector<SymbolState> symbols_;
  std::deque<std::int64_t> sent_;  // when recent orders were sent, for the rate limit
  std::int64_t total_position_ = 0;
  double session_cash_ = 0;
  double run_cash_ = 0;
  bool session_killed_ = false;
  bool run_killed_ = false;
  StrategyCounters counters_;
  SignalCheck* check_ = nullptr;
};

}  // namespace bt
