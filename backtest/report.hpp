#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "backtest/simulator.hpp"

// Judging a run, or a stretch of sessions of one: the numbers every report
// leads with, always in the same order. The success rate comes first, as
// "X% (N of M trades)", then net profit, profit per trade, the worst trade,
// the largest drawdown, the fill rate and adverse selection.

namespace bt {

struct Summary {
  std::uint64_t trades = 0;     // round trips
  std::uint64_t wins = 0;       // made money, after commission
  std::uint64_t losses = 0;     // lost money; the rest broke even
  std::uint64_t forced = 0;     // closed by the session's end, not by the strategy
  double net = 0;               // dollars, after commission
  double per_trade = 0;
  double worst = 0;             // the worst trade
  double best = 0;
  double stdev = 0;             // of profit per trade
  double t_stat = 0;            // per_trade / (stdev / sqrt(trades)): about 2 or more before an edge is believable
  double max_drawdown = 0;      // dollars, from a peak of cumulative closed-trade profit
  double fill_rate = 0;         // of shares offered in resting orders, the share that filled
  double adverse_1s = 0;        // cents a share the price moved against a resting fill, one second on
  double adverse_5s = 0;        // and five seconds on
  std::uint64_t orders = 0;
  std::uint32_t sessions = 0;

  [[nodiscard]] double success_rate() const noexcept {
    return trades == 0 ? 0 : static_cast<double>(wins) / static_cast<double>(trades);
  }

  // The 95% range the true success rate is likely in, given how many trades
  // there were (the Wilson interval). Few trades: a wide range.
  [[nodiscard]] std::pair<double, double> success_range() const noexcept {
    if (trades == 0) {
      return {0, 0};
    }
    const double n = static_cast<double>(trades);
    const double p = success_rate();
    const double z = 1.96;
    const double centre = (p + z * z / (2 * n)) / (1 + z * z / n);
    const double half = z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / (1 + z * z / n);
    return {std::max(0.0, centre - half), std::min(1.0, centre + half)};
  }
};

// A summary of sessions [first, end) of a run. Every trip carries its session.
[[nodiscard]] inline Summary summarize(const RunResult& run, std::uint32_t first, std::uint32_t end) {
  Summary summary;
  summary.sessions = end > first ? end - first : 0;
  double peak = 0;
  double cumulative = 0;
  double sum_squares = 0;
  bool any = false;
  for (const RoundTrip& trip : run.trips) {
    if (trip.session < first || trip.session >= end) {
      continue;
    }
    ++summary.trades;
    if (trip.pnl > 1e-9) {
      ++summary.wins;
    } else if (trip.pnl < -1e-9) {
      ++summary.losses;
    }
    summary.forced += trip.forced ? 1 : 0;
    summary.net += trip.pnl;
    sum_squares += trip.pnl * trip.pnl;
    summary.worst = any ? std::min(summary.worst, trip.pnl) : trip.pnl;
    summary.best = any ? std::max(summary.best, trip.pnl) : trip.pnl;
    any = true;
    cumulative += trip.pnl;
    peak = std::max(peak, cumulative);
    summary.max_drawdown = std::max(summary.max_drawdown, peak - cumulative);
  }
  if (summary.trades > 0) {
    const double n = static_cast<double>(summary.trades);
    summary.per_trade = summary.net / n;
    if (summary.trades > 1) {
      const double variance = std::max(0.0, (sum_squares - n * summary.per_trade * summary.per_trade) / (n - 1));
      summary.stdev = std::sqrt(variance);
      summary.t_stat = summary.stdev > 0 ? summary.per_trade / (summary.stdev / std::sqrt(n)) : 0;
    }
  }
  std::uint64_t submitted = 0;
  std::uint64_t filled = 0;
  double adverse_1s = 0;
  double adverse_5s = 0;
  std::uint64_t adverse_1s_count = 0;
  std::uint64_t adverse_5s_count = 0;
  for (std::uint32_t session = first; session < end && session < run.by_session.size(); ++session) {
    const SessionCounts& counts = run.by_session[session];
    submitted += counts.passive_submitted;
    filled += counts.passive_filled;
    adverse_1s += counts.adverse_1s_sum;
    adverse_5s += counts.adverse_5s_sum;
    adverse_1s_count += counts.adverse_1s_count;
    adverse_5s_count += counts.adverse_5s_count;
    summary.orders += counts.orders_sent;
  }
  summary.fill_rate = submitted == 0 ? 0 : static_cast<double>(filled) / static_cast<double>(submitted);
  summary.adverse_1s = adverse_1s_count == 0 ? 0 : adverse_1s / static_cast<double>(adverse_1s_count);
  summary.adverse_5s = adverse_5s_count == 0 ? 0 : adverse_5s / static_cast<double>(adverse_5s_count);
  return summary;
}

[[nodiscard]] inline Summary summarize(const RunResult& run) {
  return summarize(run, 0, std::max<std::uint32_t>(run.sessions, static_cast<std::uint32_t>(run.by_session.size())));
}

// Several stretches taken together, as one: the walk-forward test windows.
[[nodiscard]] inline Summary combine(const std::vector<std::pair<const RunResult*, std::pair<std::uint32_t, std::uint32_t>>>& parts) {
  RunResult joined;
  std::uint32_t session = 0;
  for (const auto& [run, range] : parts) {
    for (std::uint32_t index = range.first; index < range.second; ++index, ++session) {
      for (const RoundTrip& trip : run->trips) {
        if (trip.session == index) {
          RoundTrip copy = trip;
          copy.session = session;
          joined.trips.push_back(copy);
        }
      }
      joined.by_session.push_back(index < run->by_session.size() ? run->by_session[index] : SessionCounts{});
    }
  }
  joined.sessions = session;
  return summarize(joined, 0, session);
}

// --- Formatting -------------------------------------------------------------------

[[nodiscard]] inline std::string format(const char* pattern, double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), pattern, value);
  return buffer;
}

[[nodiscard]] inline std::string money(double dollars) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s$%.2f", dollars < -0.005 ? "-" : "", std::abs(dollars));
  return buffer;
}

// "35.6% (46 of 132 trades)"
[[nodiscard]] inline std::string success_text(const Summary& summary) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "%.1f%% (%llu of %llu trades)", summary.success_rate() * 100,
                static_cast<unsigned long long>(summary.wins), static_cast<unsigned long long>(summary.trades));
  return buffer;
}

// The standard block of lines, success rate first.
[[nodiscard]] inline std::string summary_lines(const Summary& summary, const std::string& indent = "") {
  const auto [low, high] = summary.success_range();
  std::string text;
  text += indent + "Success rate:     " + success_text(summary);
  if (summary.trades > 0) {
    text += format(", likely %.0f", low * 100) + format("-%.0f%%", high * 100);
  }
  text += "\n";
  text += indent + "Net profit:       " + money(summary.net) + "\n";
  text += indent + "Profit per trade: " + money(summary.per_trade) +
          (summary.trades > 1 ? format(" (t = %.2f)", summary.t_stat) : "") + "\n";
  text += indent + "Worst trade:      " + money(summary.worst) + "\n";
  text += indent + "Max drawdown:     " + money(summary.max_drawdown) + "\n";
  text += indent + "Fill rate:        " + format("%.1f%%", summary.fill_rate * 100) + " of shares offered\n";
  text += indent + "Adverse selection:" + format(" %.2f", summary.adverse_1s) + format(" cents a share after 1 s, %.2f after 5 s", summary.adverse_5s) + "\n";
  return text;
}

// One row of a Markdown table, success rate first.
[[nodiscard]] inline std::string summary_row(const std::string& label, const Summary& summary) {
  return "| " + label + " | " + success_text(summary) + " | " + money(summary.net) + " | " + money(summary.per_trade) +
         " | " + format("%.2f", summary.t_stat) + " | " + money(summary.worst) + " | " + money(summary.max_drawdown) +
         " | " + format("%.1f%%", summary.fill_rate * 100) + " | " + format("%.2f", summary.adverse_1s) + " / " +
         format("%.2f", summary.adverse_5s) + " |\n";
}

[[nodiscard]] inline std::string summary_header(const std::string& first_column) {
  return "| " + first_column +
         " | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |\n"
         "|---|---|---|---|---|---|---|---|---|\n";
}

[[nodiscard]] inline std::string json_escape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

[[nodiscard]] inline std::string summary_json(const Summary& summary) {
  const auto [low, high] = summary.success_range();
  char buffer[1024];
  std::snprintf(buffer, sizeof(buffer),
                "{\"success_rate\":%.6f,\"success_text\":\"%s\",\"success_low\":%.6f,\"success_high\":%.6f,"
                "\"trades\":%llu,\"wins\":%llu,\"losses\":%llu,\"forced\":%llu,\"net\":%.4f,\"per_trade\":%.6f,"
                "\"t_stat\":%.4f,\"worst\":%.4f,\"best\":%.4f,\"max_drawdown\":%.4f,\"fill_rate\":%.6f,"
                "\"adverse_1s_cents\":%.6f,\"adverse_5s_cents\":%.6f,\"orders\":%llu,\"sessions\":%u}",
                summary.success_rate(), success_text(summary).c_str(), low, high,
                static_cast<unsigned long long>(summary.trades), static_cast<unsigned long long>(summary.wins),
                static_cast<unsigned long long>(summary.losses), static_cast<unsigned long long>(summary.forced),
                summary.net, summary.per_trade, summary.t_stat, summary.worst, summary.best, summary.max_drawdown,
                summary.fill_rate, summary.adverse_1s, summary.adverse_5s,
                static_cast<unsigned long long>(summary.orders), summary.sessions);
  return buffer;
}

}  // namespace bt
