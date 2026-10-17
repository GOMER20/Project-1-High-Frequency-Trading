#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "backtest/practice_market.hpp"
#include "lob/types.hpp"

// Tick data: every quote and every trade, with the time it happened.
//
// A tick stream is a sequence of Ticks in time order, across all its symbols.
// It comes from a TickSource: the practice market generating it on the spot
// (PracticeMarketSource, which needs no storage at all), or a file written
// earlier (TickFileReader). Prices are whole cents.
//
// The file format is compact: each event is stored as small differences from
// the one before, a few bytes each, so a whole session of one symbol is a few
// hundred kilobytes.

namespace bt {

enum class TickKind : std::uint8_t {
  Quote = 0,  // the best bid and offer and the size at each
  Trade = 1,  // a trade: price, size, and which side was the aggressor
  Open = 2,   // a session begins; prices may have jumped since the last one
  Close = 3,  // a session ends: nothing more until the next Open
};

struct Tick {
  std::int64_t time_us = 0;       // since the stream's epoch
  std::uint16_t symbol = 0;
  TickKind kind = TickKind::Quote;
  lob::Side aggressor = lob::Side::Buy;  // Trade: Buy lifted the offer, Sell hit the bid
  std::int64_t bid = 0;           // Quote: best bid. Trade: the price
  std::int64_t ask = 0;           // Quote: best offer
  std::uint32_t bid_size = 0;     // Quote: shares at the bid. Trade: shares traded
  std::uint32_t ask_size = 0;     // Quote: shares at the offer
};

struct SymbolInfo {
  std::string name;
  std::int64_t reference_cents = 0;  // a typical price, for sizing books
};

class TickSource {
 public:
  virtual ~TickSource() = default;
  // The next event, or false when the stream is over.
  virtual bool next(Tick& out) = 0;
  [[nodiscard]] virtual std::span<const SymbolInfo> symbols() const = 0;
  // Wall-clock time of the stream's time zero, in epoch milliseconds: the
  // depth model redraws its levels on wall-clock twenty-second boundaries.
  [[nodiscard]] virtual std::int64_t epoch_ms() const = 0;
  // An order of ours traded: a source that can react (the practice market,
  // whose prices respond to order flow) does. Shares are signed: + bought.
  virtual void apply_flow(std::uint16_t /*symbol*/, std::int64_t /*signed_shares*/) {}
};

// A stream given in full, for tests and for data built some other way.
class TickVectorSource final : public TickSource {
 public:
  TickVectorSource(std::vector<SymbolInfo> symbols, std::vector<Tick> ticks, std::int64_t epoch_ms = 0)
      : symbols_(std::move(symbols)), ticks_(std::move(ticks)), epoch_ms_(epoch_ms) {}

  bool next(Tick& out) override {
    if (at_ >= ticks_.size()) {
      return false;
    }
    out = ticks_[at_++];
    return true;
  }
  [[nodiscard]] std::span<const SymbolInfo> symbols() const override { return symbols_; }
  [[nodiscard]] std::int64_t epoch_ms() const override { return epoch_ms_; }
  void apply_flow(std::uint16_t symbol, std::int64_t signed_shares) override {
    flows.emplace_back(symbol, signed_shares);
  }

  // Every apply_flow() call, for tests.
  std::vector<std::pair<std::uint16_t, std::int64_t>> flows;

 private:
  std::vector<SymbolInfo> symbols_;
  std::vector<Tick> ticks_;
  std::int64_t epoch_ms_;
  std::size_t at_ = 0;
};

// --- Generating: the practice market ---------------------------------------------

// The symbols Gomer Trading streams from the start (its DEFAULT_SYMBOLS).
inline const std::vector<std::string> kDefaultSymbols = {"AAPL", "MSFT", "NVDA", "TSLA", "AMZN",
                                                         "GOOGL", "META", "AMD", "SPY", "QQQ"};

struct PracticeMarketOptions {
  std::uint32_t seed = 1;
  std::uint32_t sessions = 1;
  std::vector<std::string> symbols = kDefaultSymbols;  // which listings to emit; empty: all
  // Our own trades move prices, as they do in the app. Off reproduces a stream
  // that was recorded without us in it.
  bool impact = true;
  // Ticks per session: US exchange hours, 9:30 to 16:00 New York, two a second.
  std::uint32_t ticks_per_session = 46'800;
};

inline constexpr std::int64_t kTickUs = 500'000;
// 2026-10-12, 9:30 am New York: a Monday opening bell.
inline constexpr std::int64_t kPracticeEpochMs = 1'791'811'800'000;

// Gomer Trading's practice market, session after session. Every listing moves
// (the market factors join them all); only the chosen ones are emitted. Each
// tick gives each symbol a Quote and then a Trade at that quote.
class PracticeMarketSource final : public TickSource {
 public:
  explicit PracticeMarketSource(PracticeMarketOptions options)
      : options_(std::move(options)),
        model_random_(options_.seed),
        quote_random_(options_.seed ^ 0x9E3779B9u),
        model_(kListings, model_random_),
        activity_(kListings.size(), 1.0) {
    for (std::size_t index = 0; index < kListings.size(); ++index) {
      const bool wanted = options_.symbols.empty() ||
                          std::find(options_.symbols.begin(), options_.symbols.end(),
                                    kListings[index].symbol) != options_.symbols.end();
      if (wanted) {
        emitted_.push_back(index);
        symbols_.push_back(SymbolInfo{kListings[index].symbol,
                                      static_cast<std::int64_t>(js_round(kListings[index].price * 100))});
      }
    }
    pending_.reserve(2 * emitted_.size() + 2);
  }

  bool next(Tick& out) override {
    while (cursor_ == pending_.size()) {
      if (!advance()) {
        return false;
      }
    }
    out = pending_[cursor_++];
    return true;
  }

  [[nodiscard]] std::span<const SymbolInfo> symbols() const override { return symbols_; }
  [[nodiscard]] std::int64_t epoch_ms() const override { return kPracticeEpochMs; }

  void apply_flow(std::uint16_t symbol, std::int64_t signed_shares) override {
    if (options_.impact && symbol < emitted_.size()) {
      model_.apply_flow(emitted_[symbol], static_cast<double>(signed_shares));
    }
  }

  // The listing index of an emitted symbol: for tests.
  [[nodiscard]] std::size_t listing_of(std::uint16_t symbol) const { return emitted_[symbol]; }
  [[nodiscard]] const MarketModel& model() const noexcept { return model_; }

 private:
  // Sessions are a day apart, and a weekend after every fifth.
  [[nodiscard]] std::int64_t session_start_us(std::uint32_t session) const {
    const std::int64_t days = session + 2 * (session / 5);
    return days * 86'400'000'000LL;
  }

  // Fills `pending_` with the next tick's events. False when every session is done.
  bool advance() {
    pending_.clear();
    cursor_ = 0;
    if (session_ >= options_.sessions) {
      return false;
    }
    const std::int64_t start = session_start_us(session_);
    if (tick_ == 0) {
      if (session_ > 0) {
        // A weekend's news moves prices more than one night's.
        model_.reopen(session_ % 5 == 0 ? 1.3 : 1.0);
      }
      for (std::size_t symbol = 0; symbol < emitted_.size(); ++symbol) {
        Tick open;
        open.time_us = start;
        open.symbol = static_cast<std::uint16_t>(symbol);
        open.kind = TickKind::Open;
        pending_.push_back(open);
      }
    }
    if (tick_ == options_.ticks_per_session) {
      for (std::size_t symbol = 0; symbol < emitted_.size(); ++symbol) {
        Tick close;
        close.time_us = start + static_cast<std::int64_t>(tick_) * kTickUs;
        close.symbol = static_cast<std::uint16_t>(symbol);
        close.kind = TickKind::Close;
        pending_.push_back(close);
      }
      ++session_;
      tick_ = 0;
      return true;
    }

    // As the app's clock does: the model steps, then every symbol is requoted.
    model_.step(activity_);
    const std::int64_t now = start + static_cast<std::int64_t>(tick_ + 1) * kTickUs;
    std::size_t emitted = 0;
    for (std::size_t index = 0; index < kListings.size(); ++index) {
      // Every listing draws its quote's random numbers, emitted or not, so
      // that the choice of symbols does not change the market.
      const Level1 quote = requote(model_.price_of(index), activity_[index], quote_random_);
      if (emitted == emitted_.size() || emitted_[emitted] != index) {
        continue;
      }
      const auto symbol = static_cast<std::uint16_t>(emitted++);
      Tick q;
      q.time_us = now;
      q.symbol = symbol;
      q.kind = TickKind::Quote;
      q.bid = static_cast<std::int64_t>(js_round(quote.bid * 100));
      q.ask = static_cast<std::int64_t>(js_round(quote.ask * 100));
      q.bid_size = quote.bid_size;
      q.ask_size = quote.ask_size;
      pending_.push_back(q);

      Tick t;
      t.time_us = now;
      t.symbol = symbol;
      t.kind = TickKind::Trade;
      t.bid = static_cast<std::int64_t>(js_round(quote.last * 100));
      t.bid_size = quote.last_size;
      // A trade at the offer was a buyer lifting it; at the bid, a seller hitting it.
      t.aggressor = t.bid == q.ask ? lob::Side::Buy : lob::Side::Sell;
      pending_.push_back(t);
    }
    ++tick_;
    return true;
  }

  PracticeMarketOptions options_;
  Mulberry32 model_random_;
  Mulberry32 quote_random_;
  MarketModel model_;
  std::vector<double> activity_;
  std::vector<std::size_t> emitted_;  // listing index of each emitted symbol
  std::vector<SymbolInfo> symbols_;
  std::vector<Tick> pending_;
  std::size_t cursor_ = 0;
  std::uint32_t session_ = 0;
  std::uint32_t tick_ = 0;
};

// --- Storing: the tick file --------------------------------------------------------
//
//   header:  "LOB-TICK" | u32 version | i64 epoch_ms | u32 symbol count
//            | per symbol: char[8] name, i64 reference cents
//   events:  one after another to the end of the file, each
//            u8 kind (low 2 bits) and aggressor (bit 2), then varints:
//            symbol, time since the previous event (us),
//            Quote: bid - symbol's previous bid (zigzag), ask - bid, bid size, ask size
//            Trade: price - symbol's previous bid (zigzag), size
//
// Numbers are written as varints, seven bits a byte, so the common small ones
// take one or two bytes. The format is the same on every machine.

inline constexpr char kTickMagic[8] = {'L', 'O', 'B', '-', 'T', 'I', 'C', 'K'};
inline constexpr std::uint32_t kTickVersion = 1;

namespace detail {

inline void put_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<std::uint8_t>(value | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<std::uint8_t>(value));
}

[[nodiscard]] inline std::uint64_t zigzag(std::int64_t value) {
  return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}

[[nodiscard]] inline std::int64_t unzigzag(std::uint64_t value) {
  return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
}

inline void put_fixed(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

}  // namespace detail

class TickFileWriter {
 public:
  // Returns false if the file cannot be created.
  [[nodiscard]] bool open(const std::string& path, std::span<const SymbolInfo> symbols, std::int64_t epoch_ms) {
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) {
      return false;
    }
    std::vector<std::uint8_t> header;
    detail::put_fixed(header, kTickMagic, sizeof(kTickMagic));
    const std::uint32_t version = kTickVersion;
    detail::put_fixed(header, &version, sizeof(version));
    detail::put_fixed(header, &epoch_ms, sizeof(epoch_ms));
    const auto count = static_cast<std::uint32_t>(symbols.size());
    detail::put_fixed(header, &count, sizeof(count));
    for (const SymbolInfo& symbol : symbols) {
      char name[8] = {};
      std::memcpy(name, symbol.name.data(), std::min<std::size_t>(symbol.name.size(), 8));
      detail::put_fixed(header, name, sizeof(name));
      detail::put_fixed(header, &symbol.reference_cents, sizeof(symbol.reference_cents));
    }
    previous_bid_.assign(symbols.size(), 0);
    return std::fwrite(header.data(), 1, header.size(), file_) == header.size();
  }

  void write(const Tick& tick) {
    buffer_.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(tick.kind) |
                                                (tick.aggressor == lob::Side::Sell ? 4 : 0)));
    detail::put_varint(buffer_, tick.symbol);
    detail::put_varint(buffer_, static_cast<std::uint64_t>(tick.time_us - previous_time_));
    previous_time_ = tick.time_us;
    std::int64_t& previous = previous_bid_[tick.symbol];
    if (tick.kind == TickKind::Quote) {
      detail::put_varint(buffer_, detail::zigzag(tick.bid - previous));
      detail::put_varint(buffer_, detail::zigzag(tick.ask - tick.bid));
      detail::put_varint(buffer_, tick.bid_size);
      detail::put_varint(buffer_, tick.ask_size);
      previous = tick.bid;
    } else if (tick.kind == TickKind::Trade) {
      detail::put_varint(buffer_, detail::zigzag(tick.bid - previous));
      detail::put_varint(buffer_, tick.bid_size);
    }
    ++written_;
    if (buffer_.size() >= (1u << 16)) {
      flush();
    }
  }

  // Writes what is buffered and closes the file. Returns false on any write error.
  bool close() {
    flush();
    bool good = ok_;
    if (file_ != nullptr) {
      good = std::fclose(file_) == 0 && good;
      file_ = nullptr;
    }
    return good;
  }

  ~TickFileWriter() { close(); }

  [[nodiscard]] std::uint64_t written() const noexcept { return written_; }

 private:
  void flush() {
    if (file_ != nullptr && !buffer_.empty() &&
        std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) {
      ok_ = false;
    }
    buffer_.clear();
  }

  std::FILE* file_ = nullptr;
  std::vector<std::uint8_t> buffer_;
  std::vector<std::int64_t> previous_bid_;
  std::int64_t previous_time_ = 0;
  std::uint64_t written_ = 0;
  bool ok_ = true;
};

class TickFileReader final : public TickSource {
 public:
  // Reads the whole file into memory. Nullopt if it is not a tick file.
  [[nodiscard]] static std::unique_ptr<TickFileReader> open(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      return nullptr;
    }
    std::vector<std::uint8_t> bytes;
    std::uint8_t chunk[1 << 16];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);

    auto reader = std::unique_ptr<TickFileReader>(new TickFileReader());
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t size) {
      if (bytes.size() - at < size) {
        return false;
      }
      std::memcpy(out, bytes.data() + at, size);
      at += size;
      return true;
    };
    char magic[8];
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!take(magic, 8) || std::memcmp(magic, kTickMagic, 8) != 0 || !take(&version, 4) ||
        version != kTickVersion || !take(&reader->epoch_ms_, 8) || !take(&count, 4) ||
        count == 0 || count > 65'535) {
      return nullptr;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
      char name[8];
      SymbolInfo symbol;
      if (!take(name, 8) || !take(&symbol.reference_cents, 8)) {
        return nullptr;
      }
      symbol.name.assign(name, static_cast<std::size_t>(std::find(name, name + 8, '\0') - name));
      reader->symbols_.push_back(std::move(symbol));
    }
    reader->bytes_ = std::move(bytes);
    reader->at_ = at;
    reader->previous_bid_.assign(count, 0);
    return reader;
  }

  bool next(Tick& out) override {
    if (at_ >= bytes_.size()) {
      return false;
    }
    const std::size_t start = at_;
    const std::uint8_t head = bytes_[at_++];
    std::uint64_t symbol = 0;
    std::uint64_t delta = 0;
    if (!varint(symbol) || !varint(delta) || symbol >= symbols_.size() || (head & 3) > 3) {
      return fail(start);
    }
    out = Tick{};
    out.kind = static_cast<TickKind>(head & 3);
    out.aggressor = (head & 4) != 0 ? lob::Side::Sell : lob::Side::Buy;
    out.symbol = static_cast<std::uint16_t>(symbol);
    time_ += static_cast<std::int64_t>(delta);
    out.time_us = time_;
    std::int64_t& previous = previous_bid_[symbol];
    if (out.kind == TickKind::Quote) {
      std::uint64_t bid = 0;
      std::uint64_t spread = 0;
      std::uint64_t bid_size = 0;
      std::uint64_t ask_size = 0;
      if (!varint(bid) || !varint(spread) || !varint(bid_size) || !varint(ask_size)) {
        return fail(start);
      }
      out.bid = previous + detail::unzigzag(bid);
      out.ask = out.bid + detail::unzigzag(spread);
      out.bid_size = static_cast<std::uint32_t>(bid_size);
      out.ask_size = static_cast<std::uint32_t>(ask_size);
      previous = out.bid;
    } else if (out.kind == TickKind::Trade) {
      std::uint64_t price = 0;
      std::uint64_t size = 0;
      if (!varint(price) || !varint(size)) {
        return fail(start);
      }
      out.bid = previous + detail::unzigzag(price);
      out.bid_size = static_cast<std::uint32_t>(size);
    }
    return true;
  }

  [[nodiscard]] std::span<const SymbolInfo> symbols() const override { return symbols_; }
  [[nodiscard]] std::int64_t epoch_ms() const override { return epoch_ms_; }
  // True if the file ended part-way through an event.
  [[nodiscard]] bool damaged() const noexcept { return damaged_; }

 private:
  TickFileReader() = default;

  bool varint(std::uint64_t& out) {
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at_ >= bytes_.size()) {
        return false;
      }
      const std::uint8_t byte = bytes_[at_++];
      out |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        return true;
      }
    }
    return false;
  }

  bool fail(std::size_t start) {
    damaged_ = true;
    at_ = bytes_.size();
    static_cast<void>(start);
    return false;
  }

  std::vector<std::uint8_t> bytes_;
  std::size_t at_ = 0;
  std::int64_t epoch_ms_ = 0;
  std::vector<SymbolInfo> symbols_;
  std::vector<std::int64_t> previous_bid_;
  std::int64_t time_ = 0;
  bool damaged_ = false;
};

}  // namespace bt
