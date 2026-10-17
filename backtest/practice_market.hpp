#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Gomer Trading's practice market, in C++.
//
// Gomer Trading (the simulated exchange this project's strategies trade on)
// makes its prices with a market model and turns them into quotes and trades
// twice a second. This is a line-for-line port of that code, so that tick
// data with exactly the same behaviour can be generated here, from a seed, in
// any amount, without running the app and without storing gigabytes:
//
//   backend/src/market/model.ts              MarketModel, seededRandom
//   backend/src/market/simulatedProvider.ts  requote(): the quote and trade of each tick
//   backend/src/market/universe.ts           the listings
//   backend/src/market/depth.ts              buildBook(): the levels behind the quote
//
// Every random number comes from a seeded generator (the app uses
// Math.random for the quote's sizes and its trade; here that is a second
// seeded stream), so the same seed gives the same market every time.
//
// Being a port, it should be kept in step with those files. The tests compare
// it against prices produced by the app's own model.

namespace bt {

// --- Randomness ------------------------------------------------------------------

// mulberry32, as seededRandom() in model.ts: uniform in [0, 1).
class Mulberry32 {
 public:
  explicit Mulberry32(std::uint32_t seed) noexcept : state_(seed) {}

  double operator()() noexcept {
    state_ += 0x6D2B79F5u;
    std::uint32_t t = state_;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
  }

 private:
  std::uint32_t state_;
};

// Math.round, which rounds halves up (towards +infinity) and, unlike
// floor(x + 0.5), is exact for every double.
[[nodiscard]] inline double js_round(double x) noexcept {
  const double down = std::floor(x);
  return x - down >= 0.5 ? down + 1.0 : down;
}

[[nodiscard]] inline double round2(double x) noexcept { return js_round(x * 100.0) / 100.0; }

// --- The listings ----------------------------------------------------------------

enum Sector : std::uint8_t { TECH, SEMIS, CONSUMER, FINANCE, ENERGY, INDUSTRIAL, MEDIA, FINTECH };
inline constexpr std::size_t kSectors = 8;

struct Listing {
  const char* symbol;
  double price;  // reference price: where the stock starts, and a very weak long-run anchor
  double vol;    // volatility of its own moves, relative to a typical large company
  double beta;   // sensitivity to the market as a whole
  std::array<double, kSectors> sector_weights;
  bool fund;
};

[[nodiscard]] constexpr Listing company(const char* symbol, Sector sector, double price, double vol,
                                        double beta) {
  Listing listing{symbol, price, vol, beta, {}, false};
  listing.sector_weights[sector] = 1.0;
  return listing;
}

[[nodiscard]] constexpr Listing fund(const char* symbol, double price, double beta,
                                     std::array<double, kSectors> weights) {
  return Listing{symbol, price, 0.12, beta, weights, true};
}

// LISTINGS from universe.ts, in the same order: the order the model draws its
// random numbers in.
inline constexpr std::array<Listing, 34> kListings = {
    company("AAPL", TECH, 228.5, 1.0, 1.05),
    company("MSFT", TECH, 431.2, 0.9, 1.0),
    company("GOOGL", TECH, 165.3, 1.0, 1.05),
    company("META", TECH, 582.7, 1.2, 1.2),
    company("CRM", TECH, 284.3, 1.0, 1.1),
    company("ORCL", TECH, 171.5, 1.0, 1.0),
    company("PLTR", TECH, 41.3, 2.2, 1.6),
    company("NVDA", SEMIS, 134.8, 1.7, 1.6),
    company("AMD", SEMIS, 164.1, 1.8, 1.6),
    company("INTC", SEMIS, 22.7, 1.6, 1.2),
    company("AVGO", SEMIS, 176.4, 1.4, 1.3),
    company("MU", SEMIS, 102.7, 1.6, 1.4),
    company("AMZN", CONSUMER, 187.9, 1.1, 1.15),
    company("TSLA", CONSUMER, 251.4, 2.0, 1.7),
    company("WMT", CONSUMER, 80.4, 0.6, 0.55),
    company("KO", CONSUMER, 70.2, 0.5, 0.5),
    company("SHOP", CONSUMER, 81.2, 1.6, 1.5),
    company("UBER", CONSUMER, 74.8, 1.4, 1.3),
    company("GME", CONSUMER, 21.9, 2.8, 1.4),
    company("JPM", FINANCE, 211.6, 0.8, 1.0),
    company("BAC", FINANCE, 39.8, 0.9, 1.1),
    company("GS", FINANCE, 495.3, 0.9, 1.15),
    company("V", FINANCE, 276.9, 0.7, 0.9),
    company("XOM", ENERGY, 121.5, 0.8, 0.75),
    company("BA", INDUSTRIAL, 154.9, 1.3, 1.2),
    company("F", INDUSTRIAL, 10.7, 1.1, 1.1),
    company("NFLX", MEDIA, 712.4, 1.3, 1.15),
    company("DIS", MEDIA, 93.6, 0.9, 1.0),
    company("COIN", FINTECH, 178.6, 2.5, 1.9),
    company("SOFI", FINTECH, 8.9, 2.2, 1.6),
    fund("SPY", 573.8, 1.0, {0.3, 0.1, 0.15, 0.15, 0, 0, 0, 0}),
    fund("QQQ", 487.1, 1.15, {0.5, 0.25, 0.1, 0, 0, 0, 0, 0}),
    fund("IWM", 220.4, 1.25, {0, 0, 0.1, 0.2, 0, 0.2, 0, 0}),
    fund("DIA", 423.6, 0.9, {0, 0, 0, 0.25, 0.1, 0.2, 0, 0}),
};

// --- The model (model.ts) --------------------------------------------------------

enum class Mood : std::uint8_t { Calm, Steady, Rally, Selloff, Turbulent };

struct MoodProfile {
  double volatility;  // multiplier on every source of volatility
  double drift;       // market drift per tick
  double duration;    // average length in ticks
  // Relative odds of each mood coming next, in the order model.ts lists them.
  std::array<std::pair<Mood, double>, 4> next;
  std::size_t next_count;
};

inline constexpr double kMinute = 120;  // ticks: one tick is half a second

[[nodiscard]] constexpr MoodProfile profile(Mood mood) {
  switch (mood) {
    case Mood::Calm:
      return {0.55, 0, 14 * kMinute, {{{Mood::Steady, 6}, {Mood::Rally, 2}, {Mood::Selloff, 1}, {}}}, 3};
    case Mood::Steady:
      return {1, 0, 18 * kMinute,
              {{{Mood::Calm, 3}, {Mood::Rally, 3}, {Mood::Selloff, 2}, {Mood::Turbulent, 1}}}, 4};
    case Mood::Rally:
      return {0.9, 0.0000045, 10 * kMinute,
              {{{Mood::Steady, 5}, {Mood::Calm, 2}, {Mood::Turbulent, 1}, {Mood::Selloff, 1}}}, 4};
    case Mood::Selloff:
      return {1.7, -0.0000065, 7 * kMinute,
              {{{Mood::Turbulent, 3}, {Mood::Steady, 3}, {Mood::Rally, 2}, {}}}, 3};
    case Mood::Turbulent:
      return {2.2, 0, 6 * kMinute,
              {{{Mood::Steady, 4}, {Mood::Selloff, 2}, {Mood::Rally, 2}, {Mood::Calm, 1}}}, 4};
  }
  return {};
}

// Calibration, per tick.
inline constexpr double kMarketSigma = 0.00006;
inline constexpr double kSectorSigma = 0.00005;
inline constexpr double kSectorTrendPersistence = 0.9992;
inline constexpr double kSectorTrendSigma = 0.00000035;
inline constexpr double kCompanySigma = 0.00015;
inline constexpr double kLastingShare = 0.5;
inline constexpr double kVarianceOmega = 0.02;
inline constexpr double kVariancePersist = 0.9;
inline constexpr double kVarianceReact = 0.08;
inline constexpr double kVarianceLow = 0.3;
inline constexpr double kVarianceHigh = 8;
inline constexpr double kDislocationDecay = 0.004;
inline constexpr double kGapProbability = 1 / (180 * kMinute);
inline constexpr double kAnchorPull = 0.000002;
inline constexpr double kImpact = 0.0004;
inline constexpr double kImpactReference = 5000;
inline constexpr double kMinPrice = 0.5;
inline constexpr double kOvernightMarketSigma = 0.004;
inline constexpr double kOvernightSectorSigma = 0.003;
inline constexpr double kOvernightCompanySigma = 0.007;
inline constexpr double kOpeningVariance = 2.4;
inline constexpr double kSurgeLevel = 3.2;

struct SymbolState {
  double fair = 0;         // lasting value of the stock
  double dislocation = 0;  // temporary log dislocation of the price from fair value
  double variance = 1;     // current variance relative to normal
  double shock = 0;        // last standardised shock, which drives the next variance
  bool surging = false;
};

class MarketModel {
 public:
  // `random` is the uniform source; it must outlive the model.
  MarketModel(std::span<const Listing> listings, Mulberry32& random)
      : listings_(listings.begin(), listings.end()), random_(random), states_(listings.size()) {
    for (std::size_t index = 0; index < listings_.size(); ++index) {
      // A new listing opens within a couple of percent of its reference price.
      states_[index].fair = listings_[index].price * std::exp(0.012 * gaussian());
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return listings_.size(); }
  [[nodiscard]] const Listing& listing(std::size_t index) const noexcept { return listings_[index]; }
  [[nodiscard]] Mood mood() const noexcept { return mood_; }

  [[nodiscard]] double price_of(std::size_t index) const noexcept {
    const SymbolState& s = states_[index];
    return std::max(kMinPrice, s.fair * std::exp(s.dislocation));
  }

  [[nodiscard]] double variance_of(std::size_t index) const noexcept {
    return states_[index].variance;
  }

  // An executed order pushes the price, by an amount that grows with the
  // square root of its size and then fades.
  void apply_flow(std::size_t index, double signed_quantity) noexcept {
    if (signed_quantity == 0) {
      return;
    }
    const double sign = signed_quantity > 0 ? 1.0 : -1.0;
    states_[index].dislocation +=
        sign * kImpact * std::sqrt(std::abs(signed_quantity) / kImpactReference);
  }

  // The news that arrived while the market was shut: everything opens away
  // from where it closed, and the first minutes are unsettled.
  void reopen(double scale = 1) {
    const double market = kOvernightMarketSigma * scale * gaussian();
    std::array<double, kSectors> sectors{};
    for (double& sector : sectors) {
      sector = kOvernightSectorSigma * scale * gaussian();
    }
    for (std::size_t index = 0; index < listings_.size(); ++index) {
      const Listing& listing = listings_[index];
      SymbolState& s = states_[index];
      double change = listing.beta * market + kOvernightCompanySigma * listing.vol * scale * gaussian();
      for (std::size_t sector = 0; sector < kSectors; ++sector) {
        change += listing.sector_weights[sector] * sectors[sector];
      }
      s.fair = std::max(kMinPrice, s.fair * std::exp(change));
      s.dislocation = 0;
      s.shock = 0;
      s.variance = std::clamp(std::max(s.variance, kOpeningVariance), kVarianceLow, kVarianceHigh);
      s.surging = false;
    }
  }

  // Advances every listing by one tick. `activity` gets, for each listing, how
  // busy trading was: roughly 1 in ordinary conditions.
  void step(std::span<double> activity) {
    ++mood_age_;
    if (random_() < 1 / profile(mood_).duration) {
      mood_ = pick(profile(mood_));
      mood_age_ = 0;
    }
    const MoodProfile mood = profile(mood_);

    const double market = mood.drift + kMarketSigma * mood.volatility * gaussian();
    std::array<double, kSectors> sector_returns{};
    for (std::size_t sector = 0; sector < kSectors; ++sector) {
      sector_trend_[sector] = kSectorTrendPersistence * sector_trend_[sector] + kSectorTrendSigma * gaussian();
      sector_returns[sector] = sector_trend_[sector] + kSectorSigma * mood.volatility * gaussian();
    }

    for (std::size_t index = 0; index < listings_.size(); ++index) {
      const Listing& listing = listings_[index];
      SymbolState& s = states_[index];

      s.variance = std::clamp(kVarianceOmega + kVariancePersist * s.variance +
                                  kVarianceReact * s.variance * s.shock * s.shock,
                              kVarianceLow, kVarianceHigh);
      const double sigma = kCompanySigma * listing.vol * mood.volatility * std::sqrt(s.variance);
      s.shock = gaussian();

      double factors = listing.beta * market;
      for (std::size_t sector = 0; sector < kSectors; ++sector) {
        factors += listing.sector_weights[sector] * sector_returns[sector];
      }

      double lasting = factors + sigma * kLastingShare * s.shock;
      // As in model.ts, a fund never draws for a gap: `&&` stops before the random number.
      if (!listing.fund && random_() < kGapProbability) {
        const double direction = random_() < 0.5 ? -1.0 : 1.0;
        lasting += direction * (0.008 + 0.014 * random_()) * std::sqrt(listing.vol);
        s.variance = std::clamp(s.variance * 2.5, kVarianceLow, kVarianceHigh);
      }

      const double before = s.fair * std::exp(s.dislocation);
      s.fair *= std::exp(lasting + kAnchorPull * std::log(listing.price / s.fair));
      s.dislocation = (1 - kDislocationDecay) * s.dislocation +
                      sigma * (1 - kLastingShare) * gaussian();
      const double after = s.fair * std::exp(s.dislocation);

      if (!s.surging && s.variance > kSurgeLevel) {
        s.surging = true;
      } else if (s.surging && s.variance < kSurgeLevel / 2) {
        s.surging = false;
      }

      const double move = std::abs(std::log(after / before)) / (kCompanySigma * std::max(listing.vol, 0.3));
      activity[index] = mood.volatility * std::sqrt(s.variance) * (0.6 + 0.4 * move);
    }
  }

 private:
  // Box-Muller on the uniform source, exactly as model.ts draws it.
  double gaussian() {
    double u = 0;
    while (u == 0) {
      u = random_();
    }
    return std::sqrt(-2 * std::log(u)) * std::cos(2 * 3.141592653589793 * random_());
  }

  Mood pick(const MoodProfile& from) {
    double total = 0;
    for (std::size_t index = 0; index < from.next_count; ++index) {
      total += from.next[index].second;
    }
    double roll = random_() * total;
    for (std::size_t index = 0; index < from.next_count; ++index) {
      roll -= from.next[index].second;
      if (roll <= 0) {
        return from.next[index].first;
      }
    }
    return from.next[0].first;
  }

  std::vector<Listing> listings_;
  Mulberry32& random_;
  std::vector<SymbolState> states_;
  Mood mood_ = Mood::Steady;
  std::uint64_t mood_age_ = 0;
  std::array<double, kSectors> sector_trend_{};
};

// --- One tick's quote and trade (simulatedProvider.ts, requote) -----------------

struct Level1 {
  double bid = 0;
  double ask = 0;
  std::uint32_t bid_size = 0;
  std::uint32_t ask_size = 0;
  double last = 0;              // the tick's trade: at the bid or at the ask
  std::uint32_t last_size = 0;
};

// The Level 1 quote around a price. Stormier trading means a wider spread
// and thinner size. `random` stands in for the app's Math.random.
[[nodiscard]] inline Level1 requote(double price, double busy, Mulberry32& random) {
  Level1 quote;
  const double spread = std::max(0.01, round2(price * 0.00008 * std::min(3.0, std::max(1.0, busy))));
  quote.bid = round2(price - spread / 2);
  quote.ask = round2(quote.bid + spread);
  const double depth = std::max(1.0, js_round(12 / std::max(0.6, busy)));
  quote.bid_size = static_cast<std::uint32_t>(100 * (1 + std::floor(random() * depth * 2)));
  quote.ask_size = static_cast<std::uint32_t>(100 * (1 + std::floor(random() * depth * 2)));
  quote.last = random() < 0.5 ? quote.bid : quote.ask;
  quote.last_size = static_cast<std::uint32_t>(std::max(1.0, js_round(busy * (20 + random() * 380))));
  return quote;
}

// --- The levels behind the quote (depth.ts, buildBook) ---------------------------

inline constexpr std::size_t kDepthLevels = 10;
inline constexpr std::int64_t kReshuffleMs = 20'000;

// FNV-1a over the key's characters, as noise() in depth.ts: a stable number
// in [0, 1) for a string.
[[nodiscard]] inline double depth_noise(std::string_view key) noexcept {
  std::uint32_t hash = 2166136261u;
  for (const char c : key) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return static_cast<double>(hash % 10'000) / 10'000.0;
}

[[nodiscard]] inline std::uint32_t round_lot(double shares) noexcept {
  return static_cast<std::uint32_t>(std::max(100.0, js_round(shares / 100) * 100));
}

// A displayed size under 100 is in round lots, as some feeds quote it.
[[nodiscard]] inline std::uint32_t top_size(std::uint32_t size) noexcept {
  return size == 0 ? 0 : size < 100 ? size * 100 : size;
}

struct DepthLevel {
  std::int64_t cents;
  std::uint32_t size;
};

// One side of the modelled book: the touch, then a level every cent behind
// it, thicker further out and uneven from level to level, redrawn every
// twenty seconds. `now_ms` is the wall-clock time the app would have seen.
inline std::size_t depth_side(std::string_view symbol, bool bid, std::int64_t touch_cents,
                              std::uint32_t touch_size, std::uint32_t typical, std::int64_t now_ms,
                              std::span<DepthLevel> out) {
  if (touch_cents <= 0 || touch_size == 0 || out.empty()) {
    return 0;
  }
  const std::int64_t era = now_ms / kReshuffleMs;
  const std::int64_t away = bid ? -1 : 1;
  std::size_t count = 0;
  out[count++] = DepthLevel{touch_cents, touch_size};
  std::string key;
  for (std::size_t level = 1; level < kDepthLevels && count < out.size(); ++level) {
    const std::int64_t cents = touch_cents + away * static_cast<std::int64_t>(level);
    if (cents <= 0) {
      break;
    }
    key.assign(symbol);
    key += bid ? "|bid|" : "|ask|";
    key += std::to_string(cents);
    key += '|';
    key += std::to_string(era);
    const double variation = 0.5 + 1.1 * depth_noise(key);
    out[count++] = DepthLevel{
        cents, round_lot(typical * (0.8 + 0.22 * static_cast<double>(level)) * variation)};
  }
  return count;
}

}  // namespace bt
