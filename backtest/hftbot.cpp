// hftbot: HFT Strategy trading live, on paper, in Gomer Trading.
//
//   hftbot [--config FILE] [--set section.name=value]... [--dir DIR]
//          [--socket PATH | --tcp HOST:PORT] [--key-file FILE]
//          [--until HH:MM] [--minutes N] [--keep-tick-files N]
//
// It is started by hand, never by itself. It connects to Gomer Trading's
// binary bot gateway with the API key of a program admitted to a sandbox
// competition, trades HFT Strategy there until its stop time (two minutes
// before the US close, New York time) or until it is stopped, gets out of
// everything, and writes the session's report beside a backtest of the same
// session. Gomer Trading is a paper exchange: no real money is involved.
//
//   --dir        its folder: status.json, the kill file, runs/ (reports and
//                tick data). Default: Gomer Trading's data folder, hft-strategy/
//   --socket     the gateway's Unix socket (default: the desktop app's)
//   --tcp        or its TCP listener instead, e.g. 127.0.0.1:47835
//   --key-file   a file holding the API key (default: DIR/api-key, or the
//                GOMER_API_KEY environment variable). The key is never printed.
//   --until      New York time to stop and get out (default 15:58)
//   --minutes    stop after this many minutes instead
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "backtest/config.hpp"
#include "backtest/live.hpp"
#include "backtest/report.hpp"
#include "backtest/setup.hpp"
#include "backtest/simulator.hpp"
#include "backtest/strategy.hpp"
#include "backtest/ticks.hpp"

namespace {

std::atomic<bool> g_asked_to_stop{false};

void on_signal(int) { g_asked_to_stop = true; }

int usage() {
  std::fputs(
      "usage: hftbot [--config FILE] [--set section.name=value]... [--dir DIR]\n"
      "              [--socket PATH | --tcp HOST:PORT] [--key-file FILE]\n"
      "              [--until HH:MM] [--minutes N] [--keep-tick-files N]\n",
      stderr);
  return 2;
}

std::string data_folder() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/Library/Application Support/Gomer Trading";
}

std::string iso_time(std::int64_t epoch_us) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_us / 1'000'000);
  std::tm parts{};
  ::gmtime_r(&seconds, &parts);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return text;
}

std::string clock_text(int minutes) {
  char text[8];
  std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
  return text;
}

std::string minutes_text(int minutes) { return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes"); }

std::int64_t percentile(std::vector<std::int64_t> values, double share) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  return values[std::min(values.size() - 1, static_cast<std::size_t>(share * static_cast<double>(values.size())))];
}

struct Book {
  std::int64_t bid = 0, ask = 0;
  std::uint32_t bid_size = 0, ask_size = 0;
  std::vector<bt::BookLevel> bids, asks;  // from the depth channel, best first, cents
  bool open = false;
};

class Bot {
 public:
  Bot(bt::RunSetup setup, bt::live::LiveOptions options)
      : setup_(std::move(setup)),
        options_(std::move(options)),
        symbols_(choose(setup_.market.symbols)),
        books_(symbols_.size()),
        strategy_config_(live_config(setup_.strategy)),
        trips_(symbols_.size()),
        context_(link_, symbols_, trips_),
        strategy_(strategy_config_) {}

  int run() {
    std::filesystem::create_directories(options_.dir);
    std::string error;
    const bool connected = options_.unix_socket.empty() ? link_.connect_tcp(options_.tcp_host, options_.tcp_port, error)
                                                        : link_.connect_unix(options_.unix_socket, error);
    if (!connected) {
      std::fprintf(stderr, "hftbot: %s. Is Gomer Trading running?\n", error.c_str());
      return 1;
    }
    if (!logon()) {
      return 1;
    }
    for (const bt::SymbolInfo& symbol : symbols_) {
      bt::live::wire::SubscribeMsg message{};
      std::memcpy(message.symbol, symbol.name.data(), std::min<std::size_t>(8, symbol.name.size()));
      message.channels = bt::live::wire::ChannelBbo | bt::live::wire::ChannelTrades | bt::live::wire::ChannelDepth;
      link_.send(message, bt::live::wire::Subscribe);
    }

    started_us_ = bt::live::wall_us();
    const bt::live::NewYorkTime start = bt::live::new_york(started_us_);
    run_dir_ = std::filesystem::path(options_.dir) / "runs" / start.date;
    std::filesystem::create_directories(run_dir_);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%02d%02d", start.minutes / 60, start.minutes % 60);
    stamp_ = stamp;
    tick_path_ = run_dir_ / ("ticks-" + stamp_ + ".lobt");
    epoch_ms_ = started_us_ / 1'000;
    if (!ticks_.open(tick_path_.string(), symbols_, epoch_ms_)) {
      std::fprintf(stderr, "hftbot: cannot create %s; carrying on without recording the market\n", tick_path_.c_str());
      recording_ = false;
    }
    stop_us_ = options_.run_minutes > 0 ? started_us_ + static_cast<std::int64_t>(options_.run_minutes) * 60'000'000 : 0;
    std::fprintf(stderr, "hftbot: trading %zu symbols as %s; stopping %s. Status in %s\n", symbols_.size(), trader_.c_str(),
                 options_.run_minutes > 0 ? ("in " + minutes_text(options_.run_minutes)).c_str()
                                          : ("at " + clock_text(options_.stop_minutes) + " New York time").c_str(),
                 (std::filesystem::path(options_.dir) / "status.json").c_str());

    strategy_.on_start(context_);
    std::int64_t next_status = 0;
    std::int64_t halted_at = 0;
    for (;;) {
      const std::int64_t now = bt::live::wall_us();
      const std::int64_t wait_us = std::clamp<std::int64_t>(context_.next_timer_us() - now, 0, 200'000);
      const bool alive = link_.poll(static_cast<int>(wait_us / 1'000), [&](std::uint8_t type, std::span<const std::byte> frame) {
        on_frame(type, frame);
      });
      context_.fire_due([&](std::uint64_t token) { strategy_.on_timer(context_, token); });
      if (!alive) {
        stop_reason_ = stop_reason_.empty() ? "the gateway closed the connection (the program may have been switched off)" : stop_reason_;
        state_ = "stopped";
        break;
      }
      const std::int64_t later = bt::live::wall_us();
      if (state_ != "stopping") {
        const std::string why = why_stop(later);
        if (!why.empty()) {
          stop_reason_ = why;
          state_ = "stopping";
          halted_at = later;
          strategy_.halt(context_);
          std::fprintf(stderr, "hftbot: stopping: %s\n", why.c_str());
        }
      } else if (strategy_.idle() || later - halted_at > 30'000'000) {
        if (!strategy_.idle()) {
          stop_reason_ += "; still not flat after 30 s, left to the exchange";
        }
        state_ = "stopped";
        break;
      }
      if (later >= next_status) {
        write_status(later);
        next_status = later + 1'000'000;
      }
    }
    finish();
    return link_.connected() || state_ == "stopped" ? 0 : 1;
  }

 private:
  // The practice market's listings that were asked for ("all": every one), then any other names.
  static std::vector<bt::SymbolInfo> choose(const std::vector<std::string>& wanted) {
    std::vector<bt::SymbolInfo> chosen;
    for (const bt::Listing& listing : bt::kListings) {
      if (wanted.empty() || std::find(wanted.begin(), wanted.end(), listing.symbol) != wanted.end()) {
        chosen.push_back(bt::SymbolInfo{listing.symbol, static_cast<std::int64_t>(bt::js_round(listing.price * 100))});
      }
    }
    for (const std::string& name : wanted) {
      if (std::none_of(chosen.begin(), chosen.end(), [&](const bt::SymbolInfo& symbol) { return symbol.name == name; })) {
        chosen.push_back(bt::SymbolInfo{name, 10'000});  // not a practice-market listing: traded all the same
      }
    }
    return chosen;
  }

  static bt::StrategyConfig live_config(bt::StrategyConfig config) {
    config.feed_shows_own_orders = false;  // Gomer Trading's depth is the market's, without our orders
    return config;
  }

  bool logon() {
    bt::live::wire::LogonMsg message{};
    std::memcpy(message.api_key, options_.api_key.data(), std::min<std::size_t>(64, options_.api_key.size()));
    link_.send(message, bt::live::wire::Logon);
    std::fill(std::begin(message.api_key), std::end(message.api_key), '\0');
    bool answered = false;
    bool ok = false;
    const std::int64_t give_up = bt::live::wall_us() + 5'000'000;
    while (!answered && bt::live::wall_us() < give_up) {
      if (!link_.poll(100, [&](std::uint8_t type, std::span<const std::byte> frame) {
            if (type == bt::live::wire::LogonAck) {
              bt::live::wire::LogonAckMsg ack{};
              answered = bt::live::read_as(frame, ack);
              ok = answered && ack.status == 0;
              trader_ = std::string(ack.trader_id, static_cast<std::size_t>(std::find(ack.trader_id, ack.trader_id + 24, '\0') - ack.trader_id));
              char hex[33];
              for (int index = 0; index < 16; ++index) {
                std::snprintf(hex + 2 * index, 3, "%02x", ack.competition_id[index]);
              }
              competition_ = hex;
              orders_per_second_ = ack.orders_per_second;
            } else if (type == bt::live::wire::Reject) {
              bt::live::wire::RejectMsg reject{};
              if (bt::live::read_as(frame, reject)) {
                std::fprintf(stderr, "hftbot: logon refused (code %u): %.48s\n", reject.code, reject.text);
              }
              answered = true;
            }
          })) {
        break;
      }
    }
    if (!ok) {
      std::fprintf(stderr, "hftbot: could not log on to the gateway. Check the API key and that the program is switched on.\n");
    }
    return ok;
  }

  [[nodiscard]] std::optional<std::uint16_t> index_of(const char (&name)[8]) const {
    const std::string symbol = bt::live::wire::symbol_of(name);
    for (std::size_t index = 0; index < symbols_.size(); ++index) {
      if (symbols_[index].name == symbol) {
        return static_cast<std::uint16_t>(index);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::int64_t tick_time(std::int64_t ts_us) {
    const std::int64_t at = std::max(last_tick_us_, ts_us - epoch_ms_ * 1'000);
    last_tick_us_ = at;
    return at;
  }

  void record(const bt::Tick& tick) {
    if (recording_) {
      ticks_.write(tick);
    }
  }

  void on_frame(std::uint8_t type, std::span<const std::byte> frame) {
    using namespace bt::live::wire;
    switch (type) {
      case Bbo: {
        BboMsg message{};
        if (!bt::live::read_as(frame, message)) return;
        const auto symbol = index_of(message.symbol);
        if (!symbol) return;
        on_bbo(*symbol, message);
        return;
      }
      case Trade: {
        TradeMsg message{};
        if (!bt::live::read_as(frame, message)) return;
        const auto symbol = index_of(message.symbol);
        if (!symbol || !books_[*symbol].open) return;
        Book& book = books_[*symbol];
        bt::MarketUpdate update = view(*symbol, static_cast<std::int64_t>(message.ts_ns / 1'000));
        update.kind = bt::TickKind::Trade;
        update.trade_price = to_cents(message.price);
        update.trade_size = message.size;
        update.aggressor = update.trade_price >= book.ask ? lob::Side::Buy : update.trade_price <= book.bid ? lob::Side::Sell : lob::Side::Buy;
        bt::Tick tick;
        tick.time_us = tick_time(update.time_us);
        tick.symbol = *symbol;
        tick.kind = bt::TickKind::Trade;
        tick.bid = update.trade_price;
        tick.bid_size = update.trade_size;
        tick.aggressor = update.aggressor;
        record(tick);
        strategy_.on_market(context_, update);
        return;
      }
      case Depth: {
        DepthMsg message{};
        if (!bt::live::read_as(frame, message)) return;
        const auto symbol = index_of(message.symbol);
        if (!symbol || frame.size() < sizeof(DepthMsg) + (std::size_t{message.n_bids} + message.n_asks) * sizeof(LevelMsg)) return;
        Book& book = books_[*symbol];
        book.bids.clear();
        book.asks.clear();
        for (std::size_t index = 0; index < std::size_t{message.n_bids} + message.n_asks; ++index) {
          LevelMsg level{};
          std::memcpy(&level, frame.data() + sizeof(DepthMsg) + index * sizeof(LevelMsg), sizeof(level));
          (index < message.n_bids ? book.bids : book.asks).push_back(bt::BookLevel{to_cents(level.price), level.size});
        }
        return;
      }
      case ExecReport: {
        ExecReportMsg message{};
        if (bt::live::read_as(frame, message)) on_exec(message);
        return;
      }
      case Reject: {
        RejectMsg message{};
        if (bt::live::read_as(frame, message)) on_reject(message);
        return;
      }
      default:
        return;
    }
  }

  // The book as the strategy sees it: the best bid and offer, and the depth behind them.
  [[nodiscard]] bt::MarketUpdate view(std::uint16_t symbol, std::int64_t time_us) const {
    const Book& book = books_[symbol];
    bt::MarketUpdate update;
    update.time_us = time_us;
    update.symbol = symbol;
    update.kind = bt::TickKind::Quote;
    update.bids[update.bid_levels++] = bt::BookLevel{book.bid, book.bid_size};
    update.asks[update.ask_levels++] = bt::BookLevel{book.ask, book.ask_size};
    for (const bt::BookLevel& level : book.bids) {
      if (level.price < book.bid && update.bid_levels < bt::kViewLevels) update.bids[update.bid_levels++] = level;
    }
    for (const bt::BookLevel& level : book.asks) {
      if (level.price > book.ask && update.ask_levels < bt::kViewLevels) update.asks[update.ask_levels++] = level;
    }
    return update;
  }

  void on_bbo(std::uint16_t symbol, const bt::live::wire::BboMsg& message) {
    const std::int64_t bid = bt::live::wire::to_cents(message.bid);
    const std::int64_t ask = bt::live::wire::to_cents(message.ask);
    if (bid <= 0 || ask <= bid) return;
    Book& book = books_[symbol];
    book.bid = bid;
    book.ask = ask;
    book.bid_size = message.bid_size;
    book.ask_size = message.ask_size;
    const auto ts_us = static_cast<std::int64_t>(message.ts_ns / 1'000);
    const std::int64_t now = bt::live::wall_us();
    // The market is trading only if its quotes are fresh: a subscription is
    // answered with the last quote even while the exchange is shut.
    if (!book.open && std::abs(now - ts_us) < 5'000'000 && state_ == "waiting for the market") {
      state_ = "trading";
    }
    if (!book.open && std::abs(now - ts_us) < 5'000'000 && state_ == "trading") {
      book.open = true;
      bt::Tick open;
      open.time_us = tick_time(ts_us);
      open.symbol = symbol;
      open.kind = bt::TickKind::Open;
      record(open);
      strategy_.on_session(context_, symbol, true);
    }
    if (!book.open) return;
    bt::Tick tick;
    tick.time_us = tick_time(ts_us);
    tick.symbol = symbol;
    tick.kind = bt::TickKind::Quote;
    tick.bid = bid;
    tick.ask = ask;
    tick.bid_size = message.bid_size;
    tick.ask_size = message.ask_size;
    record(tick);
    trips_.mid(now, symbol, bid + ask);
    strategy_.on_market(context_, view(symbol, ts_us));
  }

  void on_exec(const bt::live::wire::ExecReportMsg& message) {
    using namespace bt::live::wire;
    bt::live::LiveContext::Sent* sent = context_.order(message.cl_ord_id);
    if (sent == nullptr) return;  // not one of this run's
    const std::int64_t now = bt::live::wall_us();
    if (!sent->answered) {
      sent->answered = true;
      context_.round_trips_us.push_back(now - sent->sent_us);
    }
    const double commission = static_cast<double>(message.commission - sent->commission) / static_cast<double>(kPriceScale);
    sent->commission = message.commission;
    const std::uint64_t id = message.cl_ord_id;
    const std::uint16_t symbol = sent->symbol;
    const bool limit = sent->kind == bt::OrderKind::Limit;
    switch (message.exec_type) {
      case ExecNew:
        if (limit) {
          sent->rested = message.leaves_qty > 0;
          trips_.offered(message.leaves_qty);
          strategy_.on_order(context_, bt::OrderReport{now, id, symbol, bt::OrderEvent::Accepted, message.leaves_qty});
        }
        return;
      case ExecPartialFill:
      case ExecFill: {
        const bool passive = limit && sent->rested;
        const std::int64_t price = to_cents(message.last_px);
        trips_.fill(now, symbol, sent->side, price, message.last_qty, commission, passive);
        if (commission > 0 && message.last_qty > 0) commission_per_share_ = commission / message.last_qty;
        const bt::OrderKind kind = sent->kind;
        const lob::Side side = sent->side;
        if (message.leaves_qty == 0) context_.finished(id);
        strategy_.on_fill(context_, bt::FillReport{now, id, symbol, side, price, message.last_qty, message.leaves_qty, passive});
        if (message.leaves_qty == 0 && kind != bt::OrderKind::Limit) {
          strategy_.on_order(context_, bt::OrderReport{now, id, symbol, bt::OrderEvent::Done, 0});
        }
        return;
      }
      case ExecCanceled: {
        const bt::OrderKind kind = sent->kind;
        context_.finished(id);
        strategy_.on_order(context_, bt::OrderReport{now, id, symbol, kind == bt::OrderKind::Limit ? bt::OrderEvent::Cancelled : bt::OrderEvent::Done, 0});
        return;
      }
      case ExecRejected:
        context_.finished(id);
        strategy_.on_order(context_, bt::OrderReport{now, id, symbol, bt::OrderEvent::Rejected, 0});
        return;
      default:
        return;
    }
  }

  void on_reject(const bt::live::wire::RejectMsg& message) {
    using namespace bt::live::wire;
    const bool cancel = message.ref_type == Cancel;
    std::uint64_t id = message.cl_ord_id;
    if (id == 0) {
      id = context_.order_by_seq(message.ref_seq, cancel).value_or(0);
    }
    bt::live::LiveContext::Sent* sent = context_.order(id);
    // A cancel that arrives after its order filled is part of trading, not a problem.
    constexpr std::uint16_t kTooLateToCancel = 8;
    if (cancel && message.code == kTooLateToCancel) {
      ++late_cancels_;
    } else if (++rejects_ <= 20) {
      std::fprintf(stderr, "hftbot: the gateway refused %s %llu (code %u): %.48s\n", cancel ? "the cancel of" : "order",
                   static_cast<unsigned long long>(id), message.code, message.text);
    }
    if (sent == nullptr) return;
    const std::uint16_t symbol = sent->symbol;
    const std::int64_t now = bt::live::wall_us();
    if (cancel) {
      strategy_.on_order(context_, bt::OrderReport{now, id, symbol, bt::OrderEvent::CancelRejected, 0});
    } else {
      context_.finished(id);
      strategy_.on_order(context_, bt::OrderReport{now, id, symbol, bt::OrderEvent::Rejected, 0});
    }
  }

  [[nodiscard]] std::string why_stop(std::int64_t now) {
    if (g_asked_to_stop) return "asked to stop";
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::path(options_.dir) / "kill", error)) {
      std::filesystem::remove(std::filesystem::path(options_.dir) / "kill", error);
      killed_from_outside_ = true;
      return "the kill switch in Gomer Trading's Admin console";
    }
    if (strategy_.stopped()) return "its own kill switch: the run's loss limit";
    if (stop_us_ > 0) {
      if (now >= stop_us_) return "its time was up (" + minutes_text(options_.run_minutes) + ")";
    } else if (bt::live::new_york(now).minutes >= options_.stop_minutes) {
      return "its stop time, " + clock_text(options_.stop_minutes) + " New York time";
    }
    return {};
  }

  [[nodiscard]] std::string holdings_json() const {
    std::string json = "[";
    for (const auto& held : strategy_.holdings()) {
      json += std::string(json.size() > 1 ? "," : "") + "{\"symbol\":\"" + symbols_[held.symbol].name +
              "\",\"shares\":" + std::to_string(held.shares) + ",\"entry\":" + bt::format("%.2f", static_cast<double>(held.entry_price) / 100) +
              ",\"mark\":" + bt::format("%.3f", static_cast<double>(held.mid2) / 200) + "}";
    }
    return json + "]";
  }

  void write_status(std::int64_t now) {
    const bt::Summary summary = bt::summarize(trips_.result(now));
    const bt::StrategyCounters& counts = strategy_.counters();
    std::size_t trading = 0;
    for (const Book& book : books_) trading += book.open ? 1 : 0;
    std::string json = "{\"program\":\"HFT Strategy\",\"pid\":" + std::to_string(::getpid()) + ",\"state\":\"" + state_ +
                       "\",\"reason\":\"" + bt::json_escape(stop_reason_) + "\",\"started\":\"" + iso_time(started_us_) +
                       "\",\"updated\":\"" + iso_time(now) + "\",\"trader\":\"" + bt::json_escape(trader_) +
                       "\",\"competition\":\"" + competition_ + "\",\"symbols\":" + std::to_string(symbols_.size()) +
                       ",\"symbols_trading\":" + std::to_string(trading) + ",\"pnl\":" + bt::format("%.2f", trips_.equity()) +
                       ",\"summary\":" + bt::summary_json(summary) + ",\"holdings\":" + holdings_json() +
                       ",\"counters\":{\"entries\":" + std::to_string(counts.entries) + ",\"entries_filled\":" + std::to_string(counts.entries_filled) +
                       ",\"entries_withdrawn\":" + std::to_string(counts.entries_withdrawn) + ",\"target_exits\":" + std::to_string(counts.target_exits) +
                       ",\"time_stops\":" + std::to_string(counts.time_stops) + ",\"kill_switches\":" + std::to_string(counts.kill_switches) +
                       ",\"rejects\":" + std::to_string(rejects_) + "}" +
                       ",\"latency_us\":{\"median\":" + std::to_string(percentile(context_.round_trips_us, 0.5)) +
                       ",\"p99\":" + std::to_string(percentile(context_.round_trips_us, 0.99)) + "}" +
                       ",\"session_kill_switch\":" + (strategy_.session_stopped() ? "true" : "false") +
                       ",\"tick_file\":\"" + bt::json_escape(tick_path_.string()) + "\",\"disk_bytes\":" +
                       std::to_string(bt::live::folder_bytes(options_.dir)) + "}\n";
    bt::live::write_atomically(std::filesystem::path(options_.dir) / "status.json", json);
  }

  void finish() {
    const std::int64_t now = bt::live::wall_us();
    for (std::size_t symbol = 0; symbol < books_.size(); ++symbol) {
      if (books_[symbol].open) {
        bt::Tick close;
        close.time_us = tick_time(now);
        close.symbol = static_cast<std::uint16_t>(symbol);
        close.kind = bt::TickKind::Close;
        record(close);
        strategy_.on_session(context_, static_cast<std::uint16_t>(symbol), false);
      }
    }
    if (recording_ && !ticks_.close()) {
      std::fprintf(stderr, "hftbot: writing the tick file failed (disk full?)\n");
      recording_ = false;
    }
    write_status(now);
    const bt::RunResult live = trips_.result(now);
    const bt::Summary summary = bt::summarize(live);

    // The same session through the backtester: the market it was sent, the same settings.
    std::optional<bt::Summary> replay;
    if (recording_) {
      if (std::unique_ptr<bt::TickFileReader> reader = bt::TickFileReader::open(tick_path_.string())) {
        bt::StrategyConfig config = setup_.strategy;
        config.feed_shows_own_orders = true;  // the backtester's book has our orders in it
        config.commission_per_share = commission_per_share_ > 0 ? commission_per_share_ : setup_.exchange.commission_per_share;
        bt::SimConfig exchange = setup_.exchange;
        const std::int64_t one_way = std::max<std::int64_t>(1, percentile(context_.round_trips_us, 0.5) / 2);
        exchange.feed_latency_us = exchange.order_latency_us = exchange.cancel_latency_us = one_way;
        exchange.commission_per_share = config.commission_per_share;
        bt::HftStrategy strategy(config);
        bt::Simulator simulator(*reader, strategy, exchange);
        replay = bt::summarize(simulator.run());
      }
    }

    const bt::live::NewYorkTime ended = bt::live::new_york(now);
    const bt::live::NewYorkTime began = bt::live::new_york(started_us_);
    std::string md = "# HFT Strategy: live session " + began.date + "\n\n";
    md += "**Success rate: " + bt::success_text(summary) + "**\n\n";
    md += "```\n" + bt::summary_lines(summary) + "```\n\n";
    md += "- Traded " + clock_text(began.minutes) + " to " + clock_text(ended.minutes) + " New York time, as " + trader_ +
          ", in Gomer Trading's sandbox competition " + competition_ + ". Paper trading: no real money.\n";
    md += "- Stopped by " + (stop_reason_.empty() ? std::string("the end of the run") : stop_reason_) + ".\n";
    md += "- Orders sent: " + std::to_string(live.orders_sent) + "; refused by the gateway: " + std::to_string(rejects_) +
          "; cancels that arrived after their order had filled: " + std::to_string(late_cancels_) +
          ". Order to first report: " + bt::format("%.2f", static_cast<double>(percentile(context_.round_trips_us, 0.5)) / 1000) +
          " ms typical, " + bt::format("%.2f", static_cast<double>(percentile(context_.round_trips_us, 0.99)) / 1000) + " ms at the 99th percentile.\n";
    md += "- Market recorded to " + tick_path_.filename().string() + bt::format(" (%.1f MB)", static_cast<double>(std::filesystem::exists(tick_path_) ? std::filesystem::file_size(tick_path_) : 0) / 1e6) +
          "; the newest " + std::to_string(options_.keep_tick_files) + " recordings are kept.\n\n";
    md += "## Live against the backtest\n\n";
    if (replay) {
      md += "The same session's market, as it was recorded here, through the backtester with the same settings, the delay measured live, and the realistic sandbox's rules (trade prints trade with the queue; the market's own orders queue and cancel as HFT's house does):\n\n";
      md += bt::summary_header("");
      md += bt::summary_row("Live", summary);
      md += bt::summary_row("Backtest of the same session", *replay);
      md += "\nWhere they differ, the usual reasons are: the live prices already include the strategy's own effect on the market, which the replay cannot take back out; the delays live varied from order to order, where the replay uses one typical delay; and the live feed reports the depth behind the touch at most every 50 ms.\n";
    } else {
      md += "No backtest: the market could not be recorded this session.\n";
    }
    std::string json = "{\"date\":\"" + began.date + "\",\"live\":" + bt::summary_json(summary) +
                       ",\"backtest\":" + (replay ? bt::summary_json(*replay) : std::string("null")) + ",\"reason\":\"" +
                       bt::json_escape(stop_reason_) + "\",\"orders\":" + std::to_string(live.orders_sent) + "}\n";
    const std::filesystem::path md_path = run_dir_ / ("report-" + stamp_ + ".md");
    bt::live::write_atomically(md_path, md);
    bt::live::write_atomically(run_dir_ / ("report-" + stamp_ + ".json"), json);
    bt::live::write_atomically(std::filesystem::path(options_.dir) / "latest.md", md);
    bt::live::prune_tick_files(std::filesystem::path(options_.dir) / "runs", options_.keep_tick_files);
    std::fputs(md.c_str(), stdout);
    std::fprintf(stderr, "hftbot: report in %s; its folder holds %.1f MB\n", md_path.c_str(),
                 static_cast<double>(bt::live::folder_bytes(options_.dir)) / 1e6);
  }

  bt::RunSetup setup_;
  bt::live::LiveOptions options_;
  std::vector<bt::SymbolInfo> symbols_;
  std::vector<Book> books_;
  bt::StrategyConfig strategy_config_;
  bt::live::GatewayLink link_;
  bt::live::TripBook trips_;
  bt::live::LiveContext context_;
  bt::HftStrategy strategy_;
  bt::TickFileWriter ticks_;
  bool recording_ = true;
  std::filesystem::path run_dir_;
  std::filesystem::path tick_path_;
  std::string stamp_;
  std::int64_t started_us_ = 0;
  std::int64_t stop_us_ = 0;
  std::int64_t epoch_ms_ = 0;
  std::int64_t last_tick_us_ = 0;
  std::string state_ = "waiting for the market";
  std::string stop_reason_;
  std::string trader_;
  std::string competition_;
  std::uint32_t orders_per_second_ = 0;
  std::uint64_t rejects_ = 0;
  std::uint64_t late_cancels_ = 0;  // cancels that found their order already filled
  double commission_per_share_ = 0;
  bool killed_from_outside_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::vector<std::pair<std::string, std::string>> sets;
  bt::live::LiveOptions options;
  options.dir = data_folder() + "/hft-strategy";
  options.unix_socket = data_folder() + "/gateway.sock";
  std::string key_file;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&]() -> std::string { return index + 1 < argc ? argv[++index] : std::string(); };
    if (argument == "--config") {
      config_path = value();
    } else if (argument == "--set") {
      const std::string given = value();
      const std::size_t equals = given.find('=');
      if (equals == std::string::npos) return usage();
      sets.emplace_back(given.substr(0, equals), given.substr(equals + 1));
    } else if (argument == "--dir") {
      options.dir = value();
    } else if (argument == "--socket") {
      options.unix_socket = value();
    } else if (argument == "--tcp") {
      const std::string given = value();
      const std::size_t colon = given.rfind(':');
      if (colon == std::string::npos) return usage();
      options.unix_socket.clear();
      options.tcp_host = given.substr(0, colon);
      options.tcp_port = static_cast<std::uint16_t>(std::atoi(given.substr(colon + 1).c_str()));
    } else if (argument == "--key-file") {
      key_file = value();
    } else if (argument == "--until") {
      const std::string given = value();
      int hours = 0, minutes = 0;
      if (std::sscanf(given.c_str(), "%d:%d", &hours, &minutes) != 2) return usage();
      options.stop_minutes = hours * 60 + minutes;
    } else if (argument == "--minutes") {
      options.run_minutes = std::atoi(value().c_str());
    } else if (argument == "--keep-tick-files") {
      options.keep_tick_files = static_cast<std::uint32_t>(std::max(1, std::atoi(value().c_str())));
    } else {
      return usage();
    }
  }

  std::string error;
  std::optional<bt::Settings> settings = config_path.empty() ? bt::Settings::parse("", error) : bt::Settings::load(config_path, error);
  if (!settings) {
    std::fprintf(stderr, "hftbot: %s\n", error.c_str());
    return 1;
  }
  for (const auto& [key, value] : sets) settings->set(key, value);
  bt::RunSetup setup = bt::read_setup(*settings);
  for (const std::string& problem : settings->problems()) std::fprintf(stderr, "hftbot: %s\n", problem.c_str());
  if (!settings->problems().empty()) return 1;

  if (key_file.empty() && std::getenv("GOMER_API_KEY") != nullptr) {
    options.api_key = std::getenv("GOMER_API_KEY");
  } else {
    std::ifstream file(key_file.empty() ? options.dir + "/api-key" : key_file);
    std::getline(file, options.api_key);
  }
  while (!options.api_key.empty() && std::isspace(static_cast<unsigned char>(options.api_key.back()))) options.api_key.pop_back();
  if (options.api_key.empty()) {
    std::fprintf(stderr, "hftbot: no API key. Put it in %s/api-key, or pass --key-file or GOMER_API_KEY.\n", options.dir.c_str());
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  Bot bot(std::move(setup), std::move(options));
  return bot.run();
}
