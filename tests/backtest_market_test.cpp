// Phase 1: the practice market port and the tick data it produces.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "backtest/practice_market.hpp"
#include "backtest/ticks.hpp"

namespace {

using bt::Tick;
using bt::TickKind;

const std::string kGolden = std::string(LOB_SOURCE_DIR) + "/tests/data/practice_market_golden.txt";

std::string temp_path(const std::string& name) {
  return ::testing::TempDir() + "lob_" + std::to_string(::getpid()) + "_" + name;
}

// --- Against Gomer Trading's own code -------------------------------------------------
// tests/data/practice_market_golden.txt was printed by the app's TypeScript
// model (backtest/tools/practice_market_golden.ts). The port must agree.

bool close_enough(double expected, double actual) {
  return std::abs(expected - actual) <= 1e-9 * std::max(1.0, std::abs(expected));
}

TEST(PracticeMarket, DrawsTheSameRandomNumbersAsTheApp) {
  std::ifstream in(kGolden);
  ASSERT_TRUE(in) << kGolden;
  std::string line;
  std::vector<double> expected;
  while (std::getline(in, line)) {
    if (line.starts_with("random ")) {
      std::istringstream words(line.substr(7));
      double value = 0;
      while (words >> value) {
        expected.push_back(value);
      }
    }
  }
  ASSERT_EQ(expected.size(), 8u);
  bt::Mulberry32 random(12345);
  for (const double value : expected) {
    EXPECT_EQ(random(), value);  // exactly: the arithmetic is all integers
  }
}

TEST(PracticeMarket, MovesPricesExactlyAsTheAppsModelDoes) {
  std::ifstream in(kGolden);
  ASSERT_TRUE(in) << kGolden;
  // step -> lines of (listing, price, variance, activity)
  struct Row {
    std::size_t listing;
    double price;
    double variance;
    double activity;
  };
  std::map<int, std::vector<Row>> expected;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line.starts_with("random") || line.starts_with("depth")) {
      continue;
    }
    std::istringstream words(line);
    int step = 0;
    Row row{};
    words >> step >> row.listing >> row.price >> row.variance >> row.activity;
    expected[step].push_back(row);
  }
  ASSERT_EQ(expected.size(), 10u);

  bt::Mulberry32 random(7);
  bt::MarketModel model(bt::kListings, random);
  std::vector<double> activity(bt::kListings.size(), 0.0);
  const auto check = [&](int step, bool with_activity) {
    const std::vector<Row>& rows = expected.at(step);
    ASSERT_EQ(rows.size(), bt::kListings.size());
    for (const Row& row : rows) {
      ASSERT_TRUE(close_enough(row.price, model.price_of(row.listing)))
          << "step " << step << " " << bt::kListings[row.listing].symbol << ": " << row.price
          << " vs " << model.price_of(row.listing);
      ASSERT_TRUE(close_enough(row.variance, model.variance_of(row.listing))) << "step " << step;
      if (with_activity) {
        ASSERT_TRUE(close_enough(row.activity, activity[row.listing])) << "step " << step;
      }
    }
  };

  check(0, false);
  for (int step = 1; step <= 6000; ++step) {
    model.step(activity);
    if (step == 2000) {
      model.apply_flow(0, 25'000);  // AAPL
    }
    if (step == 2500) {
      model.apply_flow(7, -4'000);  // NVDA
    }
    if (expected.contains(step)) {
      check(step, true);
    }
  }
  model.reopen(1.3);
  check(6001, false);
  for (int step = 6002; step <= 6500; ++step) {
    model.step(activity);
  }
  check(6500, true);
}

TEST(PracticeMarket, BuildsTheSameDepthAsTheApp) {
  std::ifstream in(kGolden);
  ASSERT_TRUE(in) << kGolden;
  std::string line;
  int checked = 0;
  while (std::getline(in, line)) {
    if (!line.starts_with("depth ")) {
      continue;
    }
    std::istringstream words(line.substr(6));
    std::string symbol;
    std::int64_t now = 0;
    std::string word;
    std::string bids;
    std::string asks;
    words >> symbol >> now >> word >> bids >> word >> asks;
    const auto parse = [](const std::string& text) {
      std::vector<bt::DepthLevel> levels;
      std::istringstream items(text);
      std::string item;
      while (std::getline(items, item, ',')) {
        const std::size_t colon = item.find(':');
        levels.push_back({std::stoll(item.substr(0, colon)),
                          static_cast<std::uint32_t>(std::stoul(item.substr(colon + 1)))});
      }
      return levels;
    };
    const std::vector<bt::DepthLevel> want_bids = parse(bids);
    const std::vector<bt::DepthLevel> want_asks = parse(asks);
    ASSERT_FALSE(want_bids.empty());
    ASSERT_FALSE(want_asks.empty());

    // The same quote the tool gave buildBook: touch prices and sizes from the first levels.
    const std::uint32_t bid_size = want_bids[0].size;
    const std::uint32_t ask_size = want_asks[0].size;
    const std::uint32_t typical = bt::round_lot((bt::top_size(bid_size) + bt::top_size(ask_size)) / 2.0);
    std::array<bt::DepthLevel, bt::kDepthLevels> got{};
    const std::size_t bid_count = bt::depth_side(symbol, true, want_bids[0].cents, bid_size, typical, now, got);
    ASSERT_EQ(bid_count, want_bids.size()) << symbol;
    for (std::size_t index = 0; index < bid_count; ++index) {
      EXPECT_EQ(got[index].cents, want_bids[index].cents) << symbol << " bid " << index;
      EXPECT_EQ(got[index].size, want_bids[index].size) << symbol << " bid " << index;
    }
    const std::size_t ask_count = bt::depth_side(symbol, false, want_asks[0].cents, ask_size, typical, now, got);
    ASSERT_EQ(ask_count, want_asks.size()) << symbol;
    for (std::size_t index = 0; index < ask_count; ++index) {
      EXPECT_EQ(got[index].cents, want_asks[index].cents) << symbol << " ask " << index;
      EXPECT_EQ(got[index].size, want_asks[index].size) << symbol << " ask " << index;
    }
    ++checked;
  }
  EXPECT_EQ(checked, 3);
}

TEST(PracticeMarket, RoundsHalvesUpAsJavaScriptDoes) {
  EXPECT_EQ(bt::js_round(2.5), 3.0);
  EXPECT_EQ(bt::js_round(2.4999999999999996), 2.0);
  EXPECT_EQ(bt::js_round(0.49999999999999994), 0.0);
  EXPECT_EQ(bt::js_round(-2.5), -2.0);
  EXPECT_EQ(bt::round2(228.505), 228.51);
}

TEST(PracticeMarket, QuotesAroundThePriceTheWayTheAppDoes) {
  bt::Mulberry32 random(3);
  for (int draw = 0; draw < 10'000; ++draw) {
    const double price = 1 + 700 * random();
    const double busy = 0.3 + 4 * random();
    const bt::Level1 quote = bt::requote(price, busy, random);
    ASSERT_LT(quote.bid, quote.ask);
    ASSERT_GE(quote.ask - quote.bid, 0.01 - 1e-9);
    // The spread is under a basis point a side at most, three times wider when it is busy.
    ASSERT_LE(quote.ask - quote.bid, std::max(0.01, price * 0.00008 * 3) + 0.011);
    ASSERT_EQ(quote.bid_size % 100, 0u);
    ASSERT_EQ(quote.ask_size % 100, 0u);
    ASSERT_GE(quote.bid_size, 100u);
    ASSERT_LE(quote.bid_size, 4'000u);
    ASSERT_TRUE(quote.last == quote.bid || quote.last == quote.ask);
    ASSERT_GE(quote.last_size, 1u);
  }
}

// --- The generated stream ---------------------------------------------------------------

std::vector<Tick> drain(bt::TickSource& source, std::size_t limit = SIZE_MAX) {
  std::vector<Tick> ticks;
  Tick tick;
  while (ticks.size() < limit && source.next(tick)) {
    ticks.push_back(tick);
  }
  return ticks;
}

bool same(const Tick& a, const Tick& b) {
  return a.time_us == b.time_us && a.symbol == b.symbol && a.kind == b.kind && a.aggressor == b.aggressor &&
         a.bid == b.bid && a.ask == b.ask && a.bid_size == b.bid_size && a.ask_size == b.ask_size;
}

TEST(PracticeMarketSource, GivesSessionsOfQuotesAndTradesTwiceASecond) {
  bt::PracticeMarketSource source({.seed = 1, .sessions = 2, .symbols = {"AAPL", "F"}, .ticks_per_session = 100});
  ASSERT_EQ(source.symbols().size(), 2u);
  EXPECT_EQ(source.symbols()[0].name, "AAPL");
  EXPECT_EQ(source.symbols()[0].reference_cents, 22'850);
  EXPECT_EQ(source.symbols()[1].name, "F");

  const std::vector<Tick> ticks = drain(source);
  // Per session: an Open per symbol, 100 ticks of a Quote and a Trade per symbol, a Close per symbol.
  ASSERT_EQ(ticks.size(), 2u * (2 + 100 * 4 + 2));
  EXPECT_EQ(ticks[0].kind, TickKind::Open);
  EXPECT_EQ(ticks[2].kind, TickKind::Quote);
  EXPECT_EQ(ticks[2].time_us, bt::kTickUs);
  EXPECT_EQ(ticks[3].kind, TickKind::Trade);
  EXPECT_EQ(ticks[403].kind, TickKind::Close);
  EXPECT_EQ(ticks[404].kind, TickKind::Open);
  EXPECT_EQ(ticks[404].time_us, 86'400'000'000LL);  // the next day

  std::int64_t time = 0;
  for (const Tick& tick : ticks) {
    ASSERT_GE(tick.time_us, time);  // in time order
    time = tick.time_us;
    if (tick.kind == TickKind::Quote) {
      ASSERT_GT(tick.ask, tick.bid);
      ASSERT_GT(tick.bid, 0);
    }
  }
  // Each trade is at the quote just before it, and says which side was the aggressor.
  for (std::size_t index = 1; index < ticks.size(); ++index) {
    if (ticks[index].kind == TickKind::Trade) {
      const Tick& quote = ticks[index - 1];
      ASSERT_EQ(quote.kind, TickKind::Quote);
      ASSERT_TRUE(ticks[index].bid == quote.bid || ticks[index].bid == quote.ask);
      ASSERT_EQ(ticks[index].aggressor, ticks[index].bid == quote.ask ? lob::Side::Buy : lob::Side::Sell);
    }
  }
}

TEST(PracticeMarketSource, IsTheSameMarketForTheSameSeedWhicheverSymbolsAreTaken) {
  bt::PracticeMarketSource one({.seed = 9, .sessions = 1, .symbols = {"NVDA"}, .ticks_per_session = 300});
  bt::PracticeMarketSource again({.seed = 9, .sessions = 1, .symbols = {"NVDA"}, .ticks_per_session = 300});
  bt::PracticeMarketSource with_others({.seed = 9, .sessions = 1, .symbols = {"AAPL", "NVDA", "SPY"}, .ticks_per_session = 300});
  bt::PracticeMarketSource other_seed({.seed = 10, .sessions = 1, .symbols = {"NVDA"}, .ticks_per_session = 300});

  const std::vector<Tick> a = drain(one);
  const std::vector<Tick> b = drain(again);
  std::vector<Tick> c;
  for (Tick tick : drain(with_others)) {
    if (tick.symbol == 1) {
      tick.symbol = 0;
      c.push_back(tick);
    }
  }
  const std::vector<Tick> d = drain(other_seed);
  ASSERT_EQ(a.size(), b.size());
  ASSERT_EQ(a.size(), c.size());
  bool differs = false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    ASSERT_TRUE(same(a[index], b[index])) << index;
    ASSERT_TRUE(same(a[index], c[index])) << index;
    differs = differs || !same(a[index], d[index]);
  }
  EXPECT_TRUE(differs);
}

TEST(PracticeMarketSource, MovesWithOurOwnTradesOnlyWhenImpactIsOn) {
  const auto mid_after = [](bool impact, std::int64_t shares) {
    bt::PracticeMarketSource source({.seed = 4, .sessions = 1, .symbols = {"AAPL"}, .impact = impact, .ticks_per_session = 50});
    Tick tick;
    for (int i = 0; i < 6; ++i) {
      source.next(tick);  // the Open and two ticks
    }
    source.apply_flow(0, shares);
    std::int64_t mid = 0;
    while (source.next(tick)) {
      if (tick.kind == TickKind::Quote) {
        mid = tick.bid + tick.ask;
        break;
      }
    }
    return mid;
  };
  EXPECT_EQ(mid_after(false, 50'000), mid_after(false, 0));
  EXPECT_GT(mid_after(true, 50'000), mid_after(true, 0));   // buying lifts the price
  EXPECT_LT(mid_after(true, -50'000), mid_after(true, 0));  // selling presses it
}

// --- The tick file --------------------------------------------------------------------

TEST(TickFile, GivesBackExactlyWhatWasWrittenInAFewBytesAnEvent) {
  const std::string path = temp_path("ticks.tick");
  bt::PracticeMarketSource source({.seed = 2, .sessions = 2, .symbols = {"AAPL", "F", "NFLX"}, .ticks_per_session = 2'000});
  std::vector<Tick> written;
  {
    bt::TickFileWriter writer;
    ASSERT_TRUE(writer.open(path, source.symbols(), source.epoch_ms()));
    Tick tick;
    while (source.next(tick)) {
      writer.write(tick);
      written.push_back(tick);
    }
    ASSERT_TRUE(writer.close());
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  ASSERT_NE(file, nullptr);
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fclose(file);
  // Quotes and trades together: well under ten bytes an event.
  EXPECT_LT(static_cast<double>(size) / static_cast<double>(written.size()), 10.0);

  auto reader = bt::TickFileReader::open(path);
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(reader->symbols().size(), 3u);
  EXPECT_EQ(reader->symbols()[2].name, "NFLX");
  EXPECT_EQ(reader->symbols()[2].reference_cents, 71'240);
  EXPECT_EQ(reader->epoch_ms(), bt::kPracticeEpochMs);
  const std::vector<Tick> read = drain(*reader);
  ASSERT_EQ(read.size(), written.size());
  for (std::size_t index = 0; index < read.size(); ++index) {
    ASSERT_TRUE(same(read[index], written[index])) << index;
  }
  EXPECT_FALSE(reader->damaged());
  std::remove(path.c_str());
}

TEST(TickFile, RefusesWhatIsNotATickFileAndNoticesOneCutShort) {
  const std::string path = temp_path("bad.tick");
  {
    std::ofstream(path, std::ios::binary) << "not a tick file at all";
  }
  EXPECT_EQ(bt::TickFileReader::open(path), nullptr);
  EXPECT_EQ(bt::TickFileReader::open(path + ".missing"), nullptr);

  bt::PracticeMarketSource source({.seed = 2, .sessions = 1, .symbols = {"AAPL"}, .ticks_per_session = 100});
  {
    bt::TickFileWriter writer;
    ASSERT_TRUE(writer.open(path, source.symbols(), source.epoch_ms()));
    Tick tick;
    while (source.next(tick)) {
      writer.write(tick);
    }
    ASSERT_TRUE(writer.close());
  }
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  {
    // The last event (a Close, three bytes) and the end of the trade before it.
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes.substr(0, bytes.size() - 5);
  }
  auto reader = bt::TickFileReader::open(path);
  ASSERT_NE(reader, nullptr);
  const std::vector<Tick> read = drain(*reader);
  EXPECT_LT(read.size(), 202u);
  EXPECT_GT(read.size(), 190u);
  EXPECT_TRUE(reader->damaged());
  std::remove(path.c_str());
}

}  // namespace
