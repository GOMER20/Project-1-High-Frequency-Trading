#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backtest/practice_market.hpp"
#include "backtest/report.hpp"
#include "backtest/simulator.hpp"
#include "backtest/strategy.hpp"
#include "backtest/ticks.hpp"

// Live paper trading: HFT Strategy as a program trading in Gomer Trading,
// through its binary bot gateway (docs/gateway in Project 2), in a sandbox
// competition of its own. Nothing here touches real money: the gateway is
// Gomer Trading's paper exchange.
//
// The strategy is the same object the backtester runs. LiveContext is its
// handle on the gateway instead of on the simulator: orders become NEW_ORDER
// and CANCEL frames, execution reports become fills and order reports, and
// timers run on the wall clock.
//
// While it runs, the program
//   - records the market it is sent (best bid and offer, and trades) as a tick
//     file, so that the session can be replayed through the backtester;
//   - writes status.json once a second, which Gomer Trading's Admin console
//     shows (live profit and loss, holdings, success rate);
//   - stops, gets out of everything and finishes up if a file named `kill`
//     appears in its folder (the Admin console's kill switch), when its stop
//     time comes, or when it is asked to (Ctrl-C).
// At the end it writes the session's report, success rate first, and the
// same session replayed through the backtester beside it.

namespace bt::live {

// --- The gateway's wire format (gomer_gateway.h) ------------------------------------

namespace wire {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::int64_t kPriceScale = 10'000;  // prices are 1/10000 of a dollar

enum Type : std::uint8_t {
  Logon = 0x01,
  LogonAck = 0x02,
  Heartbeat = 0x03,
  HeartbeatAck = 0x04,
  Reject = 0x05,
  Subscribe = 0x10,
  NewOrder = 0x20,
  Cancel = 0x21,
  ExecReport = 0x30,
  Bbo = 0x40,
  Trade = 0x41,
  Depth = 0x42,
};

enum : std::uint8_t { SideBuy = 1, SideSell = 2 };
enum : std::uint8_t { OrdMarket = 1, OrdLimit = 2 };
enum : std::uint8_t { TifGtc = 1, TifIoc = 3 };
enum : std::uint8_t { ChannelBbo = 1, ChannelTrades = 2, ChannelDepth = 4 };
enum : std::uint8_t { ExecNew = 0, ExecPartialFill = 1, ExecFill = 2, ExecCanceled = 3, ExecRejected = 4 };

#pragma pack(push, 1)
struct Header {
  std::uint16_t length;
  std::uint8_t type;
  std::uint8_t version;
  std::uint32_t seq;
};
struct LogonMsg {
  Header h;
  char api_key[64];
  std::uint32_t heartbeat_ms;
  std::uint32_t reserved;
};
struct HeartbeatMsg {
  Header h;
  std::uint64_t client_time_ns;
};
struct SubscribeMsg {
  Header h;
  char symbol[8];
  std::uint8_t channels;
  std::uint8_t pad[7];
};
struct NewOrderMsg {
  Header h;
  std::uint64_t cl_ord_id;
  char symbol[8];
  std::uint8_t side;
  std::uint8_t ord_type;
  std::uint8_t tif;
  std::uint8_t pad;
  std::uint32_t qty;
  std::int64_t price;
  std::int64_t stop_price;
};
struct OrderRefMsg {
  Header h;
  std::uint64_t cl_ord_id;
};
struct LogonAckMsg {
  Header h;
  std::uint8_t status;
  std::uint8_t pad[3];
  std::uint32_t orders_per_second;
  std::uint64_t session_id;
  char trader_id[24];
  std::uint8_t competition_id[16];
  std::int64_t starting_balance;
};
struct HeartbeatAckMsg {
  Header h;
  std::uint64_t client_time_ns;
  std::uint64_t server_time_ns;
};
struct RejectMsg {
  Header h;
  std::uint32_t ref_seq;
  std::uint8_t ref_type;
  std::uint8_t pad;
  std::uint16_t code;
  std::uint64_t cl_ord_id;
  char text[48];
};
struct ExecReportMsg {
  Header h;
  std::uint64_t cl_ord_id;
  std::uint8_t order_id[16];
  char symbol[8];
  std::uint8_t exec_type;
  std::uint8_t ord_status;
  std::uint8_t side;
  std::uint8_t ord_type;
  std::uint32_t order_qty;
  std::uint32_t cum_qty;
  std::uint32_t leaves_qty;
  std::uint32_t last_qty;
  std::uint32_t reserved;
  std::int64_t last_px;
  std::int64_t avg_px;
  std::int64_t commission;
  std::int64_t slippage;
  std::uint64_t transact_time_ns;
};
struct BboMsg {
  Header h;
  char symbol[8];
  std::int64_t bid;
  std::int64_t ask;
  std::uint32_t bid_size;
  std::uint32_t ask_size;
  std::uint64_t ts_ns;
};
struct TradeMsg {
  Header h;
  char symbol[8];
  std::int64_t price;
  std::uint32_t size;
  std::uint32_t pad;
  std::uint64_t ts_ns;
};
struct LevelMsg {
  std::int64_t price;
  std::uint32_t size;
  std::uint32_t pad;
};
struct DepthMsg {
  Header h;
  char symbol[8];
  std::uint64_t ts_ns;
  std::uint8_t n_bids;
  std::uint8_t n_asks;
  std::uint8_t pad[6];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 8 && sizeof(LogonMsg) == 80 && sizeof(HeartbeatMsg) == 16 && sizeof(SubscribeMsg) == 24);
static_assert(sizeof(NewOrderMsg) == 48 && sizeof(OrderRefMsg) == 16 && sizeof(LogonAckMsg) == 72);
static_assert(sizeof(HeartbeatAckMsg) == 24 && sizeof(RejectMsg) == 72 && sizeof(ExecReportMsg) == 104);
static_assert(sizeof(BboMsg) == 48 && sizeof(TradeMsg) == 40 && sizeof(LevelMsg) == 16 && sizeof(DepthMsg) == 32);

[[nodiscard]] inline std::string symbol_of(const char (&field)[8]) {
  return std::string(field, static_cast<std::size_t>(std::find(field, field + 8, '\0') - field));
}

[[nodiscard]] inline std::int64_t to_cents(std::int64_t scaled) { return (scaled + (scaled >= 0 ? 50 : -50)) / 100; }

}  // namespace wire

// A connection to the gateway: one Unix or TCP socket, whole frames in and out.
class GatewayLink {
 public:
  GatewayLink() = default;
  GatewayLink(const GatewayLink&) = delete;
  GatewayLink& operator=(const GatewayLink&) = delete;
  ~GatewayLink() { close(); }

  [[nodiscard]] bool connect_unix(const std::string& path, std::string& error) {
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) {
      error = "the socket path is too long";
      return false;
    }
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      error = "cannot connect to " + path + ": " + std::strerror(errno);
      close();
      return false;
    }
    return true;
  }

  [[nodiscard]] bool connect_tcp(const std::string& host, std::uint16_t port, std::string& error) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
      error = "not an IPv4 address: " + host;
      return false;
    }
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      error = "cannot connect to " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
      close();
      return false;
    }
    int on = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    return true;
  }

  // Takes over a socket that is already connected: for tests, with a socketpair.
  void adopt(int fd) {
    close();
    fd_ = fd;
  }

  [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

  // Sends one whole frame, filling in its header. False if the connection has gone.
  template <typename Message>
  bool send(Message& message, std::uint8_t type) {
    message.h.length = sizeof(Message);
    message.h.type = type;
    message.h.version = wire::kVersion;
    message.h.seq = ++seq_;
    const auto* bytes = reinterpret_cast<const char*>(&message);
    std::size_t sent = 0;
    while (fd_ >= 0 && sent < sizeof(Message)) {
      const ssize_t wrote = ::send(fd_, bytes + sent, sizeof(Message) - sent, kNoSignal);
      if (wrote < 0 && errno == EINTR) {
        continue;
      }
      if (wrote <= 0) {
        close();
        return false;
      }
      sent += static_cast<std::size_t>(wrote);
    }
    return fd_ >= 0;
  }

  [[nodiscard]] std::uint32_t last_seq() const noexcept { return seq_; }

  // Waits up to `timeout_ms` for data and hands over every whole frame that
  // has arrived. False once the connection has closed.
  bool poll(int timeout_ms, const std::function<void(std::uint8_t, std::span<const std::byte>)>& on_frame) {
    if (fd_ < 0) {
      return false;
    }
    pollfd waiting{fd_, POLLIN, 0};
    const int ready = ::poll(&waiting, 1, timeout_ms);
    if (ready < 0 && errno != EINTR) {
      close();
      return false;
    }
    if (ready > 0) {
      std::array<std::byte, 1 << 16> chunk{};
      const ssize_t got = ::recv(fd_, chunk.data(), chunk.size(), 0);
      if (got <= 0 && !(got < 0 && (errno == EINTR || errno == EAGAIN))) {
        close();
        return false;
      }
      if (got > 0) {
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + got);
      }
    }
    std::size_t at = 0;
    while (buffer_.size() - at >= sizeof(wire::Header)) {
      wire::Header header{};
      std::memcpy(&header, buffer_.data() + at, sizeof(header));
      if (header.length < sizeof(wire::Header)) {
        close();  // the stream has lost its framing
        return false;
      }
      if (buffer_.size() - at < header.length) {
        break;
      }
      on_frame(header.type, std::span<const std::byte>(buffer_.data() + at, header.length));
      at += header.length;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(at));
    return fd_ >= 0;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
#ifdef MSG_NOSIGNAL
  static constexpr int kNoSignal = MSG_NOSIGNAL;
#else
  static constexpr int kNoSignal = 0;
#endif
  int fd_ = -1;
  std::uint32_t seq_ = 0;
  std::vector<std::byte> buffer_;
};

template <typename Message>
[[nodiscard]] bool read_as(std::span<const std::byte> frame, Message& out) {
  if (frame.size() < sizeof(Message)) {
    return false;
  }
  std::memcpy(&out, frame.data(), sizeof(Message));
  return true;
}

// --- Clocks -------------------------------------------------------------------------

[[nodiscard]] inline std::int64_t wall_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Minutes since midnight in New York, and the date there, for a moment.
struct NewYorkTime {
  int minutes = 0;
  std::string date;  // YYYY-MM-DD
};

[[nodiscard]] inline NewYorkTime new_york(std::int64_t epoch_us) {
  ::setenv("TZ", "America/New_York", 1);
  ::tzset();
  const std::time_t seconds = static_cast<std::time_t>(epoch_us / 1'000'000);
  std::tm parts{};
  ::localtime_r(&seconds, &parts);
  char date[16];
  std::strftime(date, sizeof(date), "%Y-%m-%d", &parts);
  return NewYorkTime{parts.tm_hour * 60 + parts.tm_min, date};
}

// --- Round trips, as the backtester counts them ------------------------------------

// Positions and round trips from fills, so that a live session is judged by
// exactly the numbers a backtest is.
class TripBook {
 public:
  explicit TripBook(std::size_t symbols) : positions_(symbols) {}

  void fill(std::int64_t now_us, std::uint16_t symbol, lob::Side side, std::int64_t price_cents, std::uint32_t quantity,
            double commission, bool passive) {
    Position& position = positions_[symbol];
    const double value = static_cast<double>(price_cents) / 100.0 * quantity;
    const std::int64_t signed_shares = side == lob::Side::Buy ? quantity : -static_cast<std::int64_t>(quantity);
    const double flow = (side == lob::Side::Buy ? -value : value) - commission;
    result_.commission += commission;
    const std::int64_t before = position.shares;
    const std::int64_t after = before + signed_shares;
    if (before != 0 && after != 0 && (before > 0) != (after > 0)) {
      const double closing = static_cast<double>(std::abs(before)) / quantity;
      position.trip_cash += flow * closing;
      position.cash += flow * closing;
      close_trip(now_us, symbol, false);
      start_trip(now_us, symbol, after > 0 ? lob::Side::Buy : lob::Side::Sell);
      position.trip_cash += flow * (1 - closing);
      position.cash += flow * (1 - closing);
      position.most = static_cast<std::uint64_t>(std::abs(after));
    } else {
      if (before == 0) {
        start_trip(now_us, symbol, signed_shares > 0 ? lob::Side::Buy : lob::Side::Sell);
      }
      position.trip_cash += flow;
      position.cash += flow;
      position.most = std::max<std::uint64_t>(position.most, static_cast<std::uint64_t>(std::abs(after)));
    }
    position.shares = after;
    if (after == 0) {
      close_trip(now_us, symbol, false);
    }
    if (passive) {
      result_.passive_filled += quantity;
      counts().passive_filled += quantity;
      adverse_.push_back(Check{now_us + 1'000'000, symbol, side, price_cents, quantity, false});
      adverse_.push_back(Check{now_us + 5'000'000, symbol, side, price_cents, quantity, true});
    } else {
      result_.aggressive_filled += quantity;
    }
  }

  void offered(std::uint32_t quantity) {
    result_.passive_submitted += quantity;
    counts().passive_submitted += quantity;
  }

  void sent() {
    ++result_.orders_sent;
    ++counts().orders_sent;
  }

  // A new mid for a symbol: marks equity and settles adverse-selection checks that are due.
  void mid(std::int64_t now_us, std::uint16_t symbol, std::int64_t mid2) {
    mids_[symbol] = mid2;
    const double mid = static_cast<double>(mid2) / 2.0;
    for (std::size_t index = 0; index < adverse_.size();) {
      const Check& check = adverse_[index];
      if (check.symbol != symbol || check.due_us > now_us) {
        ++index;
        continue;
      }
      const double move = check.side == lob::Side::Buy ? static_cast<double>(check.price) - mid : mid - static_cast<double>(check.price);
      SessionCounts& session = counts();
      (check.five ? result_.adverse_5s_sum : result_.adverse_1s_sum) += move * check.quantity;
      (check.five ? result_.adverse_5s_count : result_.adverse_1s_count) += check.quantity;
      (check.five ? session.adverse_5s_sum : session.adverse_1s_sum) += move * check.quantity;
      (check.five ? session.adverse_5s_count : session.adverse_1s_count) += check.quantity;
      adverse_[index] = adverse_.back();
      adverse_.pop_back();
    }
    const double now = equity();
    result_.peak_equity = std::max(result_.peak_equity, now);
    result_.max_drawdown = std::max(result_.max_drawdown, result_.peak_equity - now);
  }

  [[nodiscard]] double equity() const {
    double total = 0;
    for (std::size_t symbol = 0; symbol < positions_.size(); ++symbol) {
      total += positions_[symbol].cash;
      const auto mid = mids_.find(static_cast<std::uint16_t>(symbol));
      if (positions_[symbol].shares != 0 && mid != mids_.end()) {
        total += static_cast<double>(positions_[symbol].shares) * static_cast<double>(mid->second) / 200.0;
      }
    }
    return total;
  }

  [[nodiscard]] std::int64_t shares(std::uint16_t symbol) const { return positions_[symbol].shares; }

  // The session as a run of the backtester would report it.
  [[nodiscard]] RunResult result(std::int64_t now_us) const {
    RunResult copy = result_;
    copy.net_pnl = equity();
    copy.sessions = 1;
    copy.session_pnl = {copy.net_pnl};
    for (std::size_t symbol = 0; symbol < positions_.size(); ++symbol) {
      if (positions_[symbol].open) {
        RoundTrip trip;  // still open: counted at its mark
        trip.symbol = static_cast<std::uint16_t>(symbol);
        trip.side = positions_[symbol].side;
        trip.opened_us = positions_[symbol].opened_us;
        trip.closed_us = now_us;
        trip.shares = positions_[symbol].most;
        const auto mid = mids_.find(trip.symbol);
        trip.pnl = positions_[symbol].trip_cash +
                   (mid == mids_.end() ? 0 : static_cast<double>(positions_[symbol].shares) * static_cast<double>(mid->second) / 200.0);
        trip.forced = true;
        copy.trips.push_back(trip);
      }
    }
    return copy;
  }

 private:
  struct Position {
    std::int64_t shares = 0;
    double cash = 0;
    bool open = false;
    lob::Side side = lob::Side::Buy;
    std::int64_t opened_us = 0;
    std::uint64_t most = 0;
    double trip_cash = 0;
  };
  struct Check {
    std::int64_t due_us;
    std::uint16_t symbol;
    lob::Side side;
    std::int64_t price;
    std::uint32_t quantity;
    bool five;
  };

  SessionCounts& counts() {
    if (result_.by_session.empty()) {
      result_.by_session.resize(1);
    }
    return result_.by_session[0];
  }

  void start_trip(std::int64_t now_us, std::uint16_t symbol, lob::Side side) {
    Position& position = positions_[symbol];
    position.open = true;
    position.side = side;
    position.opened_us = now_us;
    position.most = 0;
    position.trip_cash = 0;
  }

  void close_trip(std::int64_t now_us, std::uint16_t symbol, bool forced) {
    Position& position = positions_[symbol];
    if (!position.open) {
      return;
    }
    RoundTrip trip;
    trip.symbol = symbol;
    trip.side = position.side;
    trip.opened_us = position.opened_us;
    trip.closed_us = now_us;
    trip.shares = position.most;
    trip.pnl = position.trip_cash;
    trip.forced = forced;
    result_.trips.push_back(trip);
    position.open = false;
    position.trip_cash = 0;
  }

  std::vector<Position> positions_;
  std::map<std::uint16_t, std::int64_t> mids_;
  std::vector<Check> adverse_;
  RunResult result_;
};

// --- The strategy's handle on the gateway --------------------------------------------

struct LiveOptions {
  std::string api_key;
  std::string unix_socket;  // used if set
  std::string tcp_host = "127.0.0.1";
  std::uint16_t tcp_port = 47'835;
  std::string dir;           // status, kill file, reports and tick data
  int stop_minutes = 15 * 60 + 58;  // New York time to stop and get out: two minutes before the close
  int run_minutes = 0;       // stop after this many minutes instead (0: at stop_minutes)
  std::uint32_t keep_tick_files = 3;
};

class LiveContext final : public Context {
 public:
  LiveContext(GatewayLink& link, std::vector<SymbolInfo> symbols, TripBook& trips)
      : link_(link), symbols_(std::move(symbols)), trips_(trips) {}

  [[nodiscard]] std::int64_t now() const noexcept override { return wall_us(); }
  [[nodiscard]] std::span<const SymbolInfo> symbols() const noexcept override { return symbols_; }

  std::uint64_t submit(std::uint16_t symbol, lob::Side side, std::int64_t price, std::uint32_t quantity,
                       OrderKind kind) override {
    const std::uint64_t id = ++next_id_;
    wire::NewOrderMsg message{};
    message.cl_ord_id = id;
    std::memcpy(message.symbol, symbols_[symbol].name.data(), std::min<std::size_t>(8, symbols_[symbol].name.size()));
    message.side = side == lob::Side::Buy ? wire::SideBuy : wire::SideSell;
    message.ord_type = kind == OrderKind::Market ? wire::OrdMarket : wire::OrdLimit;
    message.tif = kind == OrderKind::Limit ? wire::TifGtc : wire::TifIoc;
    message.qty = quantity;
    message.price = kind == OrderKind::Market ? 0 : price * (wire::kPriceScale / 100);
    orders_[id] = Sent{symbol, side, kind, quantity, now(), false, link_.last_seq() + 1, 0};
    trips_.sent();
    if (!link_.send(message, wire::NewOrder)) {
      lost_.push_back(id);
    }
    return id;
  }

  void cancel(std::uint64_t order_id) override {
    wire::OrderRefMsg message{};
    message.cl_ord_id = order_id;
    const auto it = orders_.find(order_id);
    if (it != orders_.end()) {
      it->second.cancel_seq = link_.last_seq() + 1;
    }
    link_.send(message, wire::Cancel);
  }

  void wake_at(std::int64_t time_us, std::uint64_t token) override { timers_.push(Timer{time_us, ++timer_seq_, token}); }

  // What is known about an order the strategy sent.
  struct Sent {
    std::uint16_t symbol;
    lob::Side side;
    OrderKind kind;
    std::uint32_t quantity;
    std::int64_t sent_us;
    bool answered;            // its first report has come back
    std::uint32_t seq;        // the frame that carried it
    std::uint32_t cancel_seq; // the frame that carried its cancel, if any
    std::int64_t commission = 0;  // charged so far, 1/10000 dollars
    bool rested = false;      // it was resting in the book (its fills are passive)
  };

  [[nodiscard]] Sent* order(std::uint64_t id) {
    const auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::optional<std::uint64_t> order_by_seq(std::uint32_t seq, bool cancel) const {
    for (const auto& [id, sent] : orders_) {
      if ((cancel ? sent.cancel_seq : sent.seq) == seq) {
        return id;
      }
    }
    return std::nullopt;
  }

  void finished(std::uint64_t id) { orders_.erase(id); }

  // Timers that are due, in order.
  template <typename Fire>
  void fire_due(Fire&& fire) {
    while (!timers_.empty() && timers_.top().time_us <= now()) {
      const Timer timer = timers_.top();
      timers_.pop();
      fire(timer.token);
    }
  }

  [[nodiscard]] std::int64_t next_timer_us() const { return timers_.empty() ? INT64_MAX : timers_.top().time_us; }

  // Round trips from sending an order to hearing its first report, microseconds.
  std::vector<std::int64_t> round_trips_us;
  // Orders that could not be sent because the connection had gone.
  std::vector<std::uint64_t> lost_;

 private:
  struct Timer {
    std::int64_t time_us;
    std::uint64_t seq;
    std::uint64_t token;
    bool operator>(const Timer& other) const {
      return time_us != other.time_us ? time_us > other.time_us : seq > other.seq;
    }
  };

  GatewayLink& link_;
  std::vector<SymbolInfo> symbols_;
  TripBook& trips_;
  std::map<std::uint64_t, Sent> orders_;
  std::uint64_t next_id_ = static_cast<std::uint64_t>(wall_us());  // never reused across runs: clOrdIds are unique per account
  std::priority_queue<Timer, std::vector<Timer>, std::greater<>> timers_;
  std::uint64_t timer_seq_ = 0;
};

// --- Files ---------------------------------------------------------------------------

inline bool write_atomically(const std::filesystem::path& path, const std::string& text) {
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
}

// Keeps the newest `keep` tick files under runs/, deleting older ones. Reports are kept: they are small.
inline void prune_tick_files(const std::filesystem::path& runs, std::uint32_t keep) {
  std::vector<std::filesystem::path> found;
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(runs, error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".lobt") {
      found.push_back(entry.path());
    }
  }
  std::sort(found.begin(), found.end());
  while (found.size() > keep) {
    std::filesystem::remove(found.front(), error);
    found.erase(found.begin());
  }
}

[[nodiscard]] inline std::uint64_t folder_bytes(const std::filesystem::path& dir) {
  std::uint64_t total = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, error)) {
    if (entry.is_regular_file(error)) {
      total += entry.file_size(error);
    }
  }
  return total;
}

}  // namespace bt::live
