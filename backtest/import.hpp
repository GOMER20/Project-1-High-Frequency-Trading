#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backtest/ticks.hpp"
#include "lob/order_book.hpp"
#include "lob/recording.hpp"

// Tick data from a recording of an HFT session (lob/recording.hpp), as the
// HFT program inside Gomer Trading writes one a market day.
//
// A recording holds the commands the engine was sent and nothing else, so it
// is replayed here through a book per symbol, exactly as the engine did, and
// the market is read off the books as it goes:
//
//   - a Quote whenever a symbol's best bid or offer, or the size at either,
//     has changed by the end of a run of commands for it;
//   - a Trade for every fill, at its price and size, the side that arrived
//     being the aggressor.
//
// A recording has no clock. Gomer Trading requotes every symbol each half
// second, one after another, so a new tick begins when a symbol comes round
// again; time is that count of ticks times half a second. Order IDs are the
// books' own, as they were in the engine, so cancels find their orders.

namespace bt {

struct ImportResult {
  std::vector<SymbolInfo> symbols;
  std::vector<Tick> ticks;
  std::uint64_t commands = 0;
  std::uint64_t submits = 0;
  std::uint64_t cancels = 0;
  std::uint64_t trades = 0;
  std::uint64_t quotes = 0;
  bool damaged = false;  // the recording was cut short; this is as far as it went
};

[[nodiscard]] inline ImportResult import_recording(const lob::Recording& recording) {
  ImportResult out;
  out.damaged = recording.damaged();
  const std::vector<lob::BookConfig>& configs = recording.books();
  std::vector<std::unique_ptr<lob::OrderBook>> books;
  for (const lob::BookConfig& config : configs) {
    books.push_back(std::make_unique<lob::OrderBook>(config));
    out.symbols.push_back(SymbolInfo{std::string(config.symbol), 0});
  }

  struct Touch {
    std::int64_t bid = 0, ask = 0;
    std::uint64_t bid_size = 0, ask_size = 0;
    bool operator==(const Touch&) const = default;
  };
  std::vector<Touch> last(books.size());
  std::vector<std::uint64_t> seen_in_tick(books.size(), 0);  // the tick a symbol was last worked on in
  std::uint64_t tick = 1;
  std::int64_t current = -1;  // the symbol being worked on

  const auto touch_of = [&](std::size_t symbol) {
    Touch touch;
    lob::OrderBook& book = *books[symbol];
    if (const auto bid = book.best_bid()) {
      touch.bid = *bid;
      touch.bid_size = book.quantity_at(lob::Side::Buy, *bid);
    }
    if (const auto ask = book.best_ask()) {
      touch.ask = *ask;
      touch.ask_size = book.quantity_at(lob::Side::Sell, *ask);
    }
    return touch;
  };
  const auto time_now = [&] { return static_cast<std::int64_t>(tick) * kTickUs; };
  // At the end of a run of commands for a symbol: a quote, if it changed.
  const auto settle = [&](std::int64_t symbol) {
    if (symbol < 0) {
      return;
    }
    const Touch touch = touch_of(static_cast<std::size_t>(symbol));
    if (touch == last[static_cast<std::size_t>(symbol)] || touch.bid == 0 || touch.ask == 0) {
      return;
    }
    last[static_cast<std::size_t>(symbol)] = touch;
    Tick quote;
    quote.time_us = time_now();
    quote.symbol = static_cast<std::uint16_t>(symbol);
    quote.kind = TickKind::Quote;
    quote.bid = touch.bid;
    quote.ask = touch.ask;
    quote.bid_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(touch.bid_size, UINT32_MAX));
    quote.ask_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(touch.ask_size, UINT32_MAX));
    out.ticks.push_back(quote);
    ++out.quotes;
    if (out.symbols[static_cast<std::size_t>(symbol)].reference_cents == 0) {
      out.symbols[static_cast<std::size_t>(symbol)].reference_cents = (touch.bid + touch.ask) / 2;
    }
  };

  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    Tick open;
    open.symbol = static_cast<std::uint16_t>(symbol);
    open.kind = TickKind::Open;
    out.ticks.push_back(open);
  }

  for (const lob::Command& command : recording.commands()) {
    ++out.commands;
    if (command.symbol >= books.size() ||
        (command.type != lob::CommandType::Submit && command.type != lob::CommandType::Cancel)) {
      continue;  // refused by the engine, or not about the book
    }
    if (command.symbol != current) {
      settle(current);
      if (seen_in_tick[command.symbol] == tick) {
        ++tick;  // this symbol again: the next requote has begun
      }
      seen_in_tick[command.symbol] = tick;
      current = command.symbol;
    }
    lob::OrderBook& book = *books[command.symbol];
    if (command.type == lob::CommandType::Cancel) {
      ++out.cancels;
      static_cast<void>(book.cancel(command.order_id));
      continue;
    }
    ++out.submits;
    static_cast<void>(book.submit(command.side, command.price, command.quantity, command.order_type,
                                  [&](const lob::Trade& trade) {
                                    Tick print;
                                    print.time_us = time_now();
                                    print.symbol = command.symbol;
                                    print.kind = TickKind::Trade;
                                    print.bid = trade.price;
                                    print.bid_size = trade.quantity;
                                    print.aggressor = command.side;
                                    out.ticks.push_back(print);
                                    ++out.trades;
                                  }));
  }
  settle(current);

  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    Tick close;
    close.time_us = time_now() + kTickUs;
    close.symbol = static_cast<std::uint16_t>(symbol);
    close.kind = TickKind::Close;
    out.ticks.push_back(close);
    if (out.symbols[symbol].reference_cents == 0) {
      // Never quoted: the middle of the book's range will do.
      out.symbols[symbol].reference_cents =
          configs[symbol].min_price + static_cast<std::int64_t>(configs[symbol].num_levels / 2);
    }
  }
  return out;
}

}  // namespace bt
