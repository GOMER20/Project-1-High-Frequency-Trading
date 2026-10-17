#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "backtest/practice_market.hpp"
#include "backtest/ticks.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

// The tick-level backtester.
//
// It replays a tick stream through a real order book (lob::OrderBook) for
// each symbol and lets a strategy trade in it, as it would on an exchange:
//
//   - The market's own liquidity, "the house", rests in the book as orders:
//     the quote's best bid and offer at the sizes it shows, and the levels the
//     depth model puts behind them. When the quote changes, the house's orders
//     change with it.
//   - The strategy's orders join the same book, at the back of the queue for
//     their price. Ahead of them is whatever the house had there already.
//   - Queue position is kept honestly. When the size shown at a price shrinks
//     without a trade, some of the shrinking was ahead of the strategy's order
//     and some behind; QueueModel says how much of each. When it grows, the
//     new size joins behind.
//   - When the market moves through a resting order (the offer falls to a
//     resting bid, say) the order is filled at its own price: traded through,
//     which is usually bad news for whoever was resting.
//   - Trade prints in the data can be taken as real orders that eat into the
//     queue (print_fills), or as information only. In Gomer Trading's
//     practice market they are random decoration that the app never fills
//     anything from, so they are information only unless asked otherwise.
//   - Everything takes time: market data reaches the strategy after
//     feed_latency, its orders reach the book after order_latency, cancels
//     after cancel_latency. An order can fill while its cancel is on the way.
//   - Fills can be partial, and commission is charged per share.
//
// Same input, same result: there are no clocks and no unordered iteration;
// events are taken strictly by time and then by the order they were made.

namespace bt {

enum class QueueModel : std::uint8_t {
  ProRata,  // shrinking comes from ahead and behind in proportion to how much is in each
  Back,     // shrinking comes from behind first: cancellations never move us up (pessimistic)
  Front,    // shrinking comes from ahead first (optimistic)
};

struct SimConfig {
  std::int64_t feed_latency_us = 1'000;    // exchange to strategy: market data, fills, acknowledgements
  std::int64_t order_latency_us = 1'000;   // strategy to exchange: a new order
  std::int64_t cancel_latency_us = 1'000;  // strategy to exchange: a cancel
  double commission_per_share = 0.0;       // dollars a share, on every share traded
  QueueModel queue = QueueModel::ProRata;
  bool print_fills = false;                // trade prints are real orders that trade with resting ones
  std::size_t depth_levels = kDepthLevels; // levels of house liquidity a side
  std::uint32_t book_capacity = 4'096;     // orders that can rest in one symbol's book
};

// --- What the strategy sees and does ----------------------------------------------

enum class OrderKind : std::uint8_t { Limit, IOC, Market };

struct BookLevel {
  std::int64_t price = 0;
  std::uint64_t quantity = 0;
};

inline constexpr std::size_t kViewLevels = 5;

// The market as the strategy sees it, feed_latency after it happened.
struct MarketUpdate {
  std::int64_t time_us = 0;      // when it happened at the exchange
  std::uint16_t symbol = 0;
  TickKind kind = TickKind::Quote;
  // Trade: the print.
  std::int64_t trade_price = 0;
  std::uint32_t trade_size = 0;
  lob::Side aggressor = lob::Side::Buy;
  // The top of the book after the event, the strategy's own orders included.
  std::array<BookLevel, kViewLevels> bids{};
  std::array<BookLevel, kViewLevels> asks{};
  std::uint8_t bid_levels = 0;
  std::uint8_t ask_levels = 0;
};

struct FillReport {
  std::int64_t time_us = 0;      // when it happened at the exchange
  std::uint64_t order_id = 0;
  std::uint16_t symbol = 0;
  lob::Side side = lob::Side::Buy;
  std::int64_t price = 0;
  std::uint32_t quantity = 0;
  std::uint32_t remaining = 0;   // of the order, after this fill
  bool passive = false;          // the order was resting; false if it took liquidity arriving
};

enum class OrderEvent : std::uint8_t {
  Accepted,        // in the book (or traded in full on arrival)
  Rejected,        // refused: price outside the book, book full, nothing to trade
  Done,            // an IOC or market order finished: whatever did not trade is gone
  Cancelled,
  CancelRejected,  // too late: already filled, or never there
};

struct OrderReport {
  std::int64_t time_us = 0;
  std::uint64_t order_id = 0;
  std::uint16_t symbol = 0;
  OrderEvent event = OrderEvent::Accepted;
  std::uint32_t resting = 0;     // still in the book after this
};

class Context;

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual void on_start(Context&) {}
  virtual void on_market(Context&, const MarketUpdate&) = 0;
  virtual void on_fill(Context&, const FillReport&) {}
  virtual void on_order(Context&, const OrderReport&) {}
  virtual void on_timer(Context&, std::uint64_t /*token*/) {}
  // A session opens or closes. At a close the simulator cancels the
  // strategy's orders and flattens what it still holds, at the touch.
  virtual void on_session(Context&, std::uint16_t /*symbol*/, bool /*open*/) {}
};

// --- Results ------------------------------------------------------------------------

// One round trip: from flat, through a position, back to flat, in one symbol.
struct RoundTrip {
  std::uint16_t symbol = 0;
  lob::Side side = lob::Side::Buy;  // Buy: it was a long position
  std::int64_t opened_us = 0;
  std::int64_t closed_us = 0;
  std::uint64_t shares = 0;          // most held at once
  double pnl = 0;                    // dollars, after commission
  bool forced = false;               // closed by the simulator at a session's end
  std::uint32_t session = 0;
};

// Counts kept per session as well as in all, so that a stretch of sessions
// can be judged by itself.
struct SessionCounts {
  std::uint64_t orders_sent = 0;
  std::uint64_t passive_submitted = 0;
  std::uint64_t passive_filled = 0;
  double adverse_1s_sum = 0;
  double adverse_5s_sum = 0;
  std::uint64_t adverse_1s_count = 0;
  std::uint64_t adverse_5s_count = 0;
};

struct RunResult {
  std::vector<RoundTrip> trips;
  double net_pnl = 0;                // dollars, after commission
  double commission = 0;
  double max_drawdown = 0;           // dollars, from a peak of marked-to-market equity
  double peak_equity = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t passive_submitted = 0;  // shares offered in resting orders
  std::uint64_t passive_filled = 0;     // of those, shares that filled while resting
  std::uint64_t aggressive_filled = 0;  // shares that traded on arrival
  std::uint64_t traded_through = 0;     // passive fills from the market moving through the order
  std::uint64_t print_filled = 0;       // passive fills from trade prints
  // Adverse selection: after a passive fill, how far the mid moved against
  // the position, in cents a share, one and five seconds on. Positive is bad.
  double adverse_1s_sum = 0;
  double adverse_5s_sum = 0;
  std::uint64_t adverse_1s_count = 0;
  std::uint64_t adverse_5s_count = 0;
  std::uint64_t ticks = 0;
  std::uint32_t sessions = 0;
  std::uint64_t house_rejected = 0;     // house orders a book could not take (price out of range)
  std::vector<double> session_pnl;      // net, by session
  std::vector<SessionCounts> by_session;
};

// --- The simulator -----------------------------------------------------------------

class Simulator;

// The strategy's handle on the exchange. Everything it does takes effect
// after the configured delays.
class Context {
 public:
  explicit Context(Simulator& sim) : sim_(sim) {}

  [[nodiscard]] std::int64_t now() const noexcept;
  [[nodiscard]] std::span<const SymbolInfo> symbols() const noexcept;

  // Returns the order's ID, which every report about it carries.
  std::uint64_t submit(std::uint16_t symbol, lob::Side side, std::int64_t price, std::uint32_t quantity,
                       OrderKind kind = OrderKind::Limit);
  void cancel(std::uint64_t order_id);
  // Calls on_timer(token) at `time_us`.
  void wake_at(std::int64_t time_us, std::uint64_t token);

 private:
  Simulator& sim_;
};

class Simulator {
 public:
  Simulator(TickSource& source, Strategy& strategy, SimConfig config = {})
      : source_(source), strategy_(strategy), config_(config), context_(*this) {
    const auto symbols = source_.symbols();
    books_.reserve(symbols.size());
    for (const SymbolInfo& symbol : symbols) {
      const std::int64_t reference = std::max<std::int64_t>(symbol.reference_cents, 100);
      const std::int64_t low = std::max<std::int64_t>(1, reference / 4);
      const std::int64_t high = reference * 4;
      Book book;
      book.book = std::make_unique<lob::OrderBook>(lob::BookConfig{
          .symbol = symbol.name,
          .min_price = low,
          .num_levels = static_cast<std::size_t>(std::min<std::int64_t>(high - low, 262'144)),
          .max_orders = config_.book_capacity,
      });
      books_.push_back(std::move(book));
    }
    positions_.resize(symbols.size());
  }

  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  // Runs the whole stream and returns what happened.
  RunResult run() {
    strategy_.on_start(context_);
    Tick tick;
    bool have_tick = source_.next(tick);
    while (have_tick || !events_.empty()) {
      // At the same moment, the exchange moves before anyone hears about it.
      if (have_tick && (events_.empty() || tick.time_us <= events_.top().time_us)) {
        now_ = tick.time_us;
        on_tick(tick);
        have_tick = source_.next(tick);
      } else {
        const Event event = events_.top();
        events_.pop();
        now_ = event.time_us;
        dispatch(event);
      }
    }
    finish();
    return std::move(result_);
  }

  // --- For Context ---
  [[nodiscard]] std::int64_t now() const noexcept { return now_; }
  [[nodiscard]] std::span<const SymbolInfo> symbols() const noexcept { return source_.symbols(); }

  std::uint64_t submit(std::uint16_t symbol, lob::Side side, std::int64_t price, std::uint32_t quantity,
                       OrderKind kind) {
    const std::uint64_t id = ++next_order_id_;
    ++result_.orders_sent;
    ++counts(symbol).orders_sent;
    Pending pending{id, symbol, side, price, quantity, kind};
    schedule(now_ + config_.order_latency_us, EventType::OrderArrives, pending_orders_.size());
    pending_orders_.push_back(pending);
    return id;
  }

  void cancel(std::uint64_t order_id) {
    ++result_.cancels_sent;
    schedule(now_ + config_.cancel_latency_us, EventType::CancelArrives, order_id);
  }

  void wake_at(std::int64_t time_us, std::uint64_t token) {
    schedule(std::max(time_us, now_), EventType::Timer, token);
  }

 private:
  enum class EventType : std::uint8_t { OrderArrives, CancelArrives, Timer, DeliverMarket, DeliverFill, DeliverOrder };

  struct Event {
    std::int64_t time_us;
    std::uint64_t sequence;
    EventType type;
    std::uint64_t value;  // an index into the matching store, or an ID or token
  };

  struct Later {
    bool operator()(const Event& a, const Event& b) const noexcept {
      return a.time_us != b.time_us ? a.time_us > b.time_us : a.sequence > b.sequence;
    }
  };

  struct Pending {
    std::uint64_t id;
    std::uint16_t symbol;
    lob::Side side;
    std::int64_t price;
    std::uint32_t quantity;
    OrderKind kind;
  };

  // One of the strategy's orders while it rests in a book.
  struct Live {
    std::uint64_t id;
    lob::OrderId book_id;
    lob::Side side;
    std::int64_t price;
    std::uint32_t remaining;
  };

  struct Book {
    std::unique_ptr<lob::OrderBook> book;
    std::vector<Live> live;            // the strategy's resting orders in this book
    std::int64_t bid = 0;              // the latest quote
    std::int64_t ask = 0;
    bool quoted = false;
    std::uint32_t session = 0;
  };

  struct Position {
    std::int64_t shares = 0;       // + long, - short
    double cash = 0;               // dollars: what buying cost and selling brought, commission out
    // The round trip in progress.
    bool open = false;
    lob::Side side = lob::Side::Buy;
    std::int64_t opened_us = 0;
    std::uint64_t most = 0;
    double trip_cash = 0;
  };

  struct AdverseCheck {
    std::int64_t due_us;
    lob::Side side;
    std::int64_t price;
    std::uint32_t quantity;
    bool five;  // the five-second check; otherwise the one-second one
    std::uint32_t session;
  };

  SessionCounts& counts_for(std::uint32_t session) {
    if (result_.by_session.size() <= session) {
      result_.by_session.resize(session + 1);
    }
    return result_.by_session[session];
  }
  SessionCounts& counts(std::uint16_t symbol) { return counts_for(books_[symbol].session); }

  void schedule(std::int64_t time_us, EventType type, std::uint64_t value) {
    events_.push(Event{time_us, ++sequence_, type, value});
  }

  // --- The exchange side ---

  void on_tick(const Tick& tick) {
    ++result_.ticks;
    Book& book = books_[tick.symbol];
    switch (tick.kind) {
      case TickKind::Open:
        book.quoted = false;
        if (tick.symbol == 0) {
          result_.session_pnl.push_back(0.0);
          ++result_.sessions;
          static_cast<void>(counts_for(result_.sessions - 1));
          session_start_equity_ = equity();
        }
        book.session = result_.sessions == 0 ? 0 : result_.sessions - 1;
        strategy_.on_session(context_, tick.symbol, true);
        return;
      case TickKind::Close:
        close_session(tick.symbol);
        strategy_.on_session(context_, tick.symbol, false);
        return;
      case TickKind::Quote:
        book.bid = tick.bid;
        book.ask = tick.ask;
        book.quoted = true;
        requote(tick.symbol, tick);
        check_adverse(tick.symbol);
        mark();
        deliver_market(tick);
        return;
      case TickKind::Trade:
        if (config_.print_fills) {
          print(tick.symbol, tick);
        }
        deliver_market(tick);
        return;
    }
  }

  // The house's ladder for a quote: the touch at the quote's sizes, and the
  // depth model's levels behind it.
  void desired_ladder(std::uint16_t symbol, const Tick& quote, std::array<DepthLevel, kDepthLevels>& bids,
                      std::size_t& bid_count, std::array<DepthLevel, kDepthLevels>& asks, std::size_t& ask_count) const {
    const std::string& name = source_.symbols()[symbol].name;
    const std::int64_t now_ms = source_.epoch_ms() + quote.time_us / 1'000;
    const std::uint32_t bid_size = top_size(quote.bid_size);
    const std::uint32_t ask_size = top_size(quote.ask_size);
    const std::uint32_t typical = round_lot((bid_size + ask_size) / 2.0 > 0 ? (bid_size + ask_size) / 2.0 : 100);
    const std::size_t levels = std::min(config_.depth_levels, kDepthLevels);
    bid_count = depth_side(name, true, quote.bid, bid_size, typical, now_ms, std::span(bids.data(), levels));
    ask_count = depth_side(name, false, quote.ask, ask_size, typical, now_ms, std::span(asks.data(), levels));
  }

  [[nodiscard]] static std::uint64_t wanted_at(const std::array<DepthLevel, kDepthLevels>& ladder, std::size_t count,
                                               std::int64_t price) {
    for (std::size_t index = 0; index < count; ++index) {
      if (ladder[index].cents == price) {
        return ladder[index].size;
      }
    }
    return 0;
  }

  [[nodiscard]] bool is_ours(const Book& book, lob::OrderId book_id) const {
    return std::any_of(book.live.begin(), book.live.end(), [&](const Live& live) { return live.book_id == book_id; });
  }

  // Makes the house's orders match a new quote. Everything that has to go
  // goes first, on both sides, and only then is anything added: a new bid
  // must not meet a stale house offer on its way in.
  void requote(std::uint16_t symbol, const Tick& quote) {
    Book& book = books_[symbol];
    std::array<DepthLevel, kDepthLevels> bids{};
    std::array<DepthLevel, kDepthLevels> asks{};
    std::size_t bid_count = 0;
    std::size_t ask_count = 0;
    desired_ladder(symbol, quote, bids, bid_count, asks, ask_count);

    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
      const auto& ladder = side == lob::Side::Buy ? bids : asks;
      const std::size_t count = side == lob::Side::Buy ? bid_count : ask_count;
      prices_.clear();
      book.book->for_each_level(side, [&](lob::Price price, std::uint64_t) { prices_.push_back(price); });
      for (const std::int64_t price : prices_) {
        shrink(book, side, price, wanted_at(ladder, count, price));
      }
    }
    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
      const auto& ladder = side == lob::Side::Buy ? bids : asks;
      const std::size_t count = side == lob::Side::Buy ? bid_count : ask_count;
      for (std::size_t index = 0; index < count; ++index) {
        grow(symbol, side, ladder[index].cents, ladder[index].size);
      }
    }
  }

  // Takes the house at one price down to `wanted`, deciding by the queue
  // model how much of the reduction comes from ahead of the strategy's order.
  void shrink(Book& book, lob::Side side, std::int64_t price, std::uint64_t wanted) {
    queue_.clear();
    book.book->for_each_order(side, price, [&](const lob::Order& order) {
      queue_.push_back({order.id, order.quantity, false});
    });
    std::uint64_t house = 0;
    std::uint64_t ahead = 0;
    bool seen_ours = false;
    for (auto& entry : queue_) {
      entry.ours = is_ours(book, entry.id);
      if (entry.ours) {
        seen_ours = true;
      } else {
        house += entry.quantity;
        if (!seen_ours) {
          ahead += entry.quantity;
        }
      }
    }
    if (house <= wanted) {
      return;
    }
    const std::uint64_t cut = house - wanted;
    const std::uint64_t behind = house - ahead;
    std::uint64_t from_ahead = 0;
    if (!seen_ours) {
      from_ahead = 0;  // nothing of ours here: where it comes from does not matter
    } else if (config_.queue == QueueModel::ProRata) {
      from_ahead = static_cast<std::uint64_t>(std::llround(static_cast<double>(cut) * static_cast<double>(ahead) /
                                                           static_cast<double>(house)));
    } else if (config_.queue == QueueModel::Back) {
      from_ahead = cut > behind ? cut - behind : 0;
    } else {
      from_ahead = std::min(cut, ahead);
    }
    from_ahead = std::min(from_ahead, ahead);
    std::uint64_t from_behind = std::min(cut - from_ahead, behind);
    from_ahead = cut - from_behind;  // whatever behind could not give, ahead gives

    // Ahead: take from the house orders just in front of ours, nearest first.
    // Behind: from the back of the queue.
    std::size_t first_ours = queue_.size();
    for (std::size_t index = 0; index < queue_.size(); ++index) {
      if (queue_[index].ours) {
        first_ours = index;
        break;
      }
    }
    for (std::size_t index = first_ours; index-- > 0 && from_ahead > 0;) {
      const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(from_ahead, queue_[index].quantity));
      book.book->execute(queue_[index].id, take);
      from_ahead -= take;
    }
    // (With no order of ours here, everything counts as behind.)
    const std::size_t behind_from = first_ours == queue_.size() ? 0 : first_ours;
    for (std::size_t index = queue_.size(); index-- > behind_from && from_behind > 0;) {
      if (queue_[index].ours) {
        continue;
      }
      const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(from_behind, queue_[index].quantity));
      book.book->execute(queue_[index].id, take);
      from_behind -= take;
    }
  }

  // Brings the house at one price up to `wanted`. New house orders join at
  // the back; if the price now crosses a resting order of the strategy's (the
  // market has moved through it), they trade with it, at its price.
  void grow(std::uint16_t symbol, lob::Side side, std::int64_t price, std::uint64_t wanted) {
    Book& book = books_[symbol];
    std::uint64_t house = book.book->quantity_at(side, price);
    for (const Live& live : book.live) {
      if (live.side == side && live.price == price) {
        house -= std::min<std::uint64_t>(house, live.remaining);
      }
    }
    if (house >= wanted) {
      return;
    }
    const auto more = static_cast<lob::Quantity>(wanted - house);
    const lob::SubmitResult added = book.book->submit(side, price, more, lob::OrderType::Limit, [&](const lob::Trade& trade) {
      on_passive_fill(symbol, trade, FillCause::Through);
    });
    if (added.id == lob::kInvalidOrderId) {
      ++result_.house_rejected;
    }
  }

  // A trade print taken as a real order: it trades with the queue at its
  // price, house and strategy alike, in time priority.
  void print(std::uint16_t symbol, const Tick& trade) {
    Book& book = books_[symbol];
    static_cast<void>(book.book->submit(trade.aggressor, trade.bid, trade.bid_size, lob::OrderType::IOC,
                                        [&](const lob::Trade& fill) { on_passive_fill(symbol, fill, FillCause::Print); }));
  }

  enum class FillCause : std::uint8_t { Through, Print, Self };

  // A resting order traded. Only the strategy's matter here.
  void on_passive_fill(std::uint16_t symbol, const lob::Trade& trade, FillCause cause) {
    Book& book = books_[symbol];
    for (std::size_t index = 0; index < book.live.size(); ++index) {
      Live& live = book.live[index];
      if (live.book_id != trade.maker_id) {
        continue;
      }
      live.remaining -= std::min(live.remaining, trade.quantity);
      result_.passive_filled += trade.quantity;
      counts(symbol).passive_filled += trade.quantity;
      if (cause == FillCause::Through) {
        result_.traded_through += trade.quantity;
      } else if (cause == FillCause::Print) {
        result_.print_filled += trade.quantity;
      }
      const FillReport fill{now_, live.id, symbol, live.side, trade.price, trade.quantity, live.remaining, true};
      book_fill(fill);
      if (live.remaining == 0) {
        book.live[index] = book.live.back();
        book.live.pop_back();
      }
      return;
    }
  }

  void arrive(const Pending& order) {
    Book& book = books_[order.symbol];
    const lob::OrderType type = order.kind == OrderKind::Market ? lob::OrderType::Market
                                : order.kind == OrderKind::IOC  ? lob::OrderType::IOC
                                                                : lob::OrderType::Limit;
    std::uint32_t filled = 0;
    const lob::SubmitResult result = book.book->submit(order.side, order.price, order.quantity, type, [&](const lob::Trade& trade) {
      // Ours took liquidity. The other side may be the house, or one of our
      // own resting orders on the other side: that is a trade with ourselves,
      // and both halves count.
      on_passive_fill(order.symbol, trade, FillCause::Self);
      filled += trade.quantity;
      result_.aggressive_filled += trade.quantity;
      book_fill(FillReport{now_, order.id, order.symbol, order.side, trade.price, trade.quantity,
                           order.quantity - filled, false});
    });
    if (result.id == lob::kInvalidOrderId) {
      deliver_order(OrderReport{now_, order.id, order.symbol, OrderEvent::Rejected, 0});
      return;
    }
    if (result.resting > 0) {
      book.live.push_back(Live{order.id, result.id, order.side, order.price, result.resting});
      result_.passive_submitted += result.resting;
      counts(order.symbol).passive_submitted += result.resting;
      deliver_order(OrderReport{now_, order.id, order.symbol, OrderEvent::Accepted, result.resting});
    } else {
      deliver_order(OrderReport{now_, order.id, order.symbol,
                                type == lob::OrderType::Limit ? OrderEvent::Accepted : OrderEvent::Done, 0});
    }
  }

  void cancel_arrives(std::uint64_t order_id) {
    for (std::size_t symbol = 0; symbol < books_.size(); ++symbol) {
      Book& book = books_[symbol];
      for (std::size_t index = 0; index < book.live.size(); ++index) {
        if (book.live[index].id == order_id) {
          book.book->cancel(book.live[index].book_id);
          book.live[index] = book.live.back();
          book.live.pop_back();
          deliver_order(OrderReport{now_, order_id, static_cast<std::uint16_t>(symbol), OrderEvent::Cancelled, 0});
          return;
        }
      }
    }
    deliver_order(OrderReport{now_, order_id, 0, OrderEvent::CancelRejected, 0});
  }

  // --- Accounting ---

  void book_fill(const FillReport& fill) {
    Position& position = positions_[fill.symbol];
    const double shares = static_cast<double>(fill.quantity);
    const double value = static_cast<double>(fill.price) / 100.0 * shares;
    const double commission = config_.commission_per_share * shares;
    result_.commission += commission;
    const std::int64_t signed_shares = fill.side == lob::Side::Buy ? static_cast<std::int64_t>(fill.quantity)
                                                                    : -static_cast<std::int64_t>(fill.quantity);
    const double flow = (fill.side == lob::Side::Buy ? -value : value) - commission;

    // A fill that takes the position through zero closes one round trip and opens the next.
    const std::int64_t before = position.shares;
    const std::int64_t after = before + signed_shares;
    if (before != 0 && after != 0 && (before > 0) != (after > 0)) {
      const double closing_share = static_cast<double>(std::abs(before)) / static_cast<double>(fill.quantity);
      position.trip_cash += flow * closing_share;
      position.cash += flow * closing_share;
      position.shares = 0;
      close_trip(fill.symbol, false);
      start_trip(fill.symbol, after > 0 ? lob::Side::Buy : lob::Side::Sell);
      position.trip_cash += flow * (1 - closing_share);
      position.cash += flow * (1 - closing_share);
      position.shares = after;
      position.most = static_cast<std::uint64_t>(std::abs(after));
    } else {
      if (before == 0) {
        start_trip(fill.symbol, signed_shares > 0 ? lob::Side::Buy : lob::Side::Sell);
      }
      position.trip_cash += flow;
      position.cash += flow;
      position.shares = after;
      position.most = std::max<std::uint64_t>(position.most, static_cast<std::uint64_t>(std::abs(after)));
      if (after == 0) {
        close_trip(fill.symbol, false);
      }
    }

    source_.apply_flow(fill.symbol, signed_shares);
    if (fill.passive) {
      const std::uint32_t session = books_[fill.symbol].session;
      adverse_.push_back(AdverseCheck{now_ + 1'000'000, fill.side, fill.price, fill.quantity, false, session});
      adverse_.push_back(AdverseCheck{now_ + 5'000'000, fill.side, fill.price, fill.quantity, true, session});
      adverse_symbol_.push_back(fill.symbol);
      adverse_symbol_.push_back(fill.symbol);
    }
    deliver_fill(fill);
  }

  void start_trip(std::uint16_t symbol, lob::Side side) {
    Position& position = positions_[symbol];
    position.open = true;
    position.side = side;
    position.opened_us = now_;
    position.most = 0;
    position.trip_cash = 0;
  }

  void close_trip(std::uint16_t symbol, bool forced) {
    Position& position = positions_[symbol];
    if (!position.open) {
      return;
    }
    RoundTrip trip;
    trip.symbol = symbol;
    trip.side = position.side;
    trip.opened_us = position.opened_us;
    trip.closed_us = now_;
    trip.shares = position.most;
    trip.pnl = position.trip_cash;
    trip.forced = forced;
    trip.session = books_[symbol].session;
    result_.trips.push_back(trip);
    position.open = false;
    position.trip_cash = 0;
  }

  // At a session's close: the strategy's orders are cancelled and whatever it
  // still holds is closed at the touch, which is what it would cost to get out.
  void close_session(std::uint16_t symbol) {
    Book& book = books_[symbol];
    for (const Live& live : book.live) {
      book.book->cancel(live.book_id);
      deliver_order(OrderReport{now_, live.id, symbol, OrderEvent::Cancelled, 0});
    }
    book.live.clear();
    Position& position = positions_[symbol];
    if (position.shares != 0 && book.quoted) {
      const bool long_position = position.shares > 0;
      const std::int64_t price = long_position ? book.bid : book.ask;
      const auto shares = static_cast<std::uint32_t>(std::abs(position.shares));
      const double value = static_cast<double>(price) / 100.0 * shares;
      const double commission = config_.commission_per_share * shares;
      result_.commission += commission;
      const double flow = (long_position ? value : -value) - commission;
      position.cash += flow;
      position.trip_cash += flow;
      position.shares = 0;
      close_trip(symbol, true);
      deliver_fill(FillReport{now_, 0, symbol, long_position ? lob::Side::Sell : lob::Side::Buy, price, shares, 0, false});
    }
    // Clear the house out too: the next session starts from its own quote.
    prices_.clear();
    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
      prices_.clear();
      book.book->for_each_level(side, [&](lob::Price price, std::uint64_t) { prices_.push_back(price); });
      for (const std::int64_t price : prices_) {
        shrink(book, side, price, 0);
      }
    }
    book.quoted = false;
    if (symbol + 1u == books_.size() && !result_.session_pnl.empty()) {
      result_.session_pnl.back() = equity() - session_start_equity_;
    }
  }

  [[nodiscard]] double equity() const {
    double total = 0;
    for (std::size_t symbol = 0; symbol < positions_.size(); ++symbol) {
      const Position& position = positions_[symbol];
      const Book& book = books_[symbol];
      total += position.cash;
      if (position.shares != 0 && book.quoted) {
        total += static_cast<double>(position.shares) * static_cast<double>(book.bid + book.ask) / 200.0;
      }
    }
    return total;
  }

  void mark() {
    const double now = equity();
    result_.peak_equity = std::max(result_.peak_equity, now);
    result_.max_drawdown = std::max(result_.max_drawdown, result_.peak_equity - now);
  }

  void check_adverse(std::uint16_t symbol) {
    const Book& book = books_[symbol];
    const double mid = static_cast<double>(book.bid + book.ask) / 2.0;
    for (std::size_t index = 0; index < adverse_.size();) {
      const AdverseCheck& check = adverse_[index];
      if (adverse_symbol_[index] != symbol || check.due_us > now_) {
        ++index;
        continue;
      }
      // Bought at `price`: adverse if the mid is now lower. Sold: if higher.
      const double move = check.side == lob::Side::Buy ? static_cast<double>(check.price) - mid
                                                       : mid - static_cast<double>(check.price);
      SessionCounts& session = counts_for(check.session);
      if (check.five) {
        result_.adverse_5s_sum += move * check.quantity;
        result_.adverse_5s_count += check.quantity;
        session.adverse_5s_sum += move * check.quantity;
        session.adverse_5s_count += check.quantity;
      } else {
        result_.adverse_1s_sum += move * check.quantity;
        result_.adverse_1s_count += check.quantity;
        session.adverse_1s_sum += move * check.quantity;
        session.adverse_1s_count += check.quantity;
      }
      adverse_[index] = adverse_.back();
      adverse_.pop_back();
      adverse_symbol_[index] = adverse_symbol_.back();
      adverse_symbol_.pop_back();
    }
  }

  void finish() {
    result_.net_pnl = equity();
    for (std::size_t symbol = 0; symbol < positions_.size(); ++symbol) {
      if (positions_[symbol].open) {
        close_trip(static_cast<std::uint16_t>(symbol), true);
      }
    }
  }

  // --- Telling the strategy ---

  void deliver_market(const Tick& tick) {
    const Book& book = books_[tick.symbol];
    MarketUpdate update;
    update.time_us = tick.time_us;
    update.symbol = tick.symbol;
    update.kind = tick.kind;
    if (tick.kind == TickKind::Trade) {
      update.trade_price = tick.bid;
      update.trade_size = tick.bid_size;
      update.aggressor = tick.aggressor;
    }
    book.book->for_each_level(lob::Side::Buy, [&](lob::Price price, std::uint64_t quantity) {
      if (update.bid_levels < kViewLevels) {
        update.bids[update.bid_levels++] = BookLevel{price, quantity};
      }
    });
    book.book->for_each_level(lob::Side::Sell, [&](lob::Price price, std::uint64_t quantity) {
      if (update.ask_levels < kViewLevels) {
        update.asks[update.ask_levels++] = BookLevel{price, quantity};
      }
    });
    schedule(now_ + config_.feed_latency_us, EventType::DeliverMarket, market_store_.size());
    market_store_.push_back(update);
  }

  void deliver_fill(const FillReport& fill) {
    schedule(now_ + config_.feed_latency_us, EventType::DeliverFill, fill_store_.size());
    fill_store_.push_back(fill);
  }

  void deliver_order(const OrderReport& report) {
    schedule(now_ + config_.feed_latency_us, EventType::DeliverOrder, order_store_.size());
    order_store_.push_back(report);
  }

  void dispatch(const Event& event) {
    switch (event.type) {
      case EventType::OrderArrives:
        arrive(pending_orders_[event.value]);
        break;
      case EventType::CancelArrives:
        cancel_arrives(event.value);
        break;
      case EventType::Timer:
        strategy_.on_timer(context_, event.value);
        break;
      case EventType::DeliverMarket:
        strategy_.on_market(context_, market_store_[event.value]);
        break;
      case EventType::DeliverFill:
        strategy_.on_fill(context_, fill_store_[event.value]);
        break;
      case EventType::DeliverOrder:
        strategy_.on_order(context_, order_store_[event.value]);
        break;
    }
    compact();
  }

  // The stores grow with every event; once nothing waits on them they are emptied.
  void compact() {
    if (events_.empty()) {
      pending_orders_.clear();
      market_store_.clear();
      fill_store_.clear();
      order_store_.clear();
    }
  }

  TickSource& source_;
  Strategy& strategy_;
  SimConfig config_;
  Context context_;
  std::vector<Book> books_;
  std::vector<Position> positions_;
  std::priority_queue<Event, std::vector<Event>, Later> events_;
  std::uint64_t sequence_ = 0;
  std::uint64_t next_order_id_ = 0;
  std::int64_t now_ = 0;
  std::vector<Pending> pending_orders_;
  std::vector<MarketUpdate> market_store_;
  std::vector<FillReport> fill_store_;
  std::vector<OrderReport> order_store_;
  std::vector<AdverseCheck> adverse_;
  std::vector<std::uint16_t> adverse_symbol_;
  std::vector<std::int64_t> prices_;
  struct QueueEntry {
    lob::OrderId id;
    std::uint32_t quantity;
    bool ours;
  };
  std::vector<QueueEntry> queue_;
  double session_start_equity_ = 0;
  RunResult result_;
};

inline std::int64_t Context::now() const noexcept { return sim_.now(); }
inline std::span<const SymbolInfo> Context::symbols() const noexcept { return sim_.symbols(); }
inline std::uint64_t Context::submit(std::uint16_t symbol, lob::Side side, std::int64_t price, std::uint32_t quantity,
                                     OrderKind kind) {
  return sim_.submit(symbol, side, price, quantity, kind);
}
inline void Context::cancel(std::uint64_t order_id) { sim_.cancel(order_id); }
inline void Context::wake_at(std::int64_t time_us, std::uint64_t token) { sim_.wake_at(time_us, token); }

}  // namespace bt
