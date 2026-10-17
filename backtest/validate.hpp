#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "backtest/config.hpp"
#include "backtest/report.hpp"
#include "backtest/setup.hpp"
#include "backtest/strategy.hpp"
#include "backtest/ticks.hpp"

// Phase 4: is there an edge, or did the tuning just find luck?
//
//   1. Tuning candidates. Every combination of the [validation] grid is run
//      over one stream of sessions, day after day.
//   2. Walk-forward. Step through the sessions: choose the candidate that made
//      the most on the last `train_sessions`, then judge it only on the next
//      `test_sessions`, which played no part in choosing it. The test windows,
//      put together, are the out-of-sample result.
//   3. Holdout. The candidate that did best over all the walk-forward sessions
//      is the final choice, and is judged once on the last `holdout_sessions`,
//      which nothing above looked at.
//   4. Baseline. The final settings with the entry signal replaced by a coin
//      toss, everything else the same. A real signal has to beat it.
//   5. Overfitting. How much worse the chosen settings do out of sample than
//      in; whether a candidate's rank in training predicts its rank in the
//      test (the rank correlation); and how often the training winner comes
//      in below the median when tested.
//   6. Sensitivity. Each setting moved down and up: an edge that exists only
//      at one exact value is a fluke.
//   7. Stress. Longer delays, commission, a worse place in the queue, and the
//      other fill regime.
//   8. The signal check: whether the book predicts the price at all.
//
// Then a plain verdict. The run-level kill switch is off throughout, so that
// every session is measured; the session kill switch stays on.

namespace bt {

// --- Running ----------------------------------------------------------------------

struct RunOutput {
  RunResult result;
  StrategyCounters counters;
  SignalCheck signals;
  std::string error;
};

[[nodiscard]] inline std::unique_ptr<TickSource> open_source(const RunSetup& setup, std::string& error) {
  if (!setup.ticks_file.empty()) {
    std::unique_ptr<TickFileReader> reader = TickFileReader::open(setup.ticks_file);
    if (reader == nullptr) {
      error = "cannot read the tick file " + setup.ticks_file;
    }
    return reader;
  }
  return std::make_unique<PracticeMarketSource>(setup.market);
}

[[nodiscard]] inline RunOutput run_one(const RunSetup& setup) {
  RunOutput out;
  std::unique_ptr<TickSource> source = open_source(setup, out.error);
  if (source == nullptr) {
    return out;
  }
  HftStrategy strategy(setup.strategy);
  strategy.check_signals(&out.signals);
  Simulator simulator(*source, strategy, setup.exchange);
  out.result = simulator.run();
  out.counters = strategy.counters();
  return out;
}

// Runs each setup, side by side on `threads` threads (0: one per core). The
// results are in the order given and do not depend on the threads: each run
// is a world of its own.
[[nodiscard]] inline std::vector<RunOutput> run_many(const std::vector<RunSetup>& setups, unsigned threads) {
  std::vector<RunOutput> outputs(setups.size());
  unsigned count = threads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : threads;
  count = static_cast<unsigned>(std::min<std::size_t>(count, setups.size()));
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> workers;
  workers.reserve(count);
  for (unsigned worker = 0; worker < count; ++worker) {
    workers.emplace_back([&] {
      for (;;) {
        const std::size_t index = next.fetch_add(1);
        if (index >= setups.size()) {
          return;
        }
        outputs[index] = run_one(setups[index]);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  return outputs;
}

// --- Candidates -------------------------------------------------------------------

struct Override {
  std::string key;
  std::string value;
};

struct Candidate {
  std::vector<Override> overrides;

  [[nodiscard]] std::string label() const {
    if (overrides.empty()) {
      return "as configured";
    }
    std::string text;
    for (const Override& item : overrides) {
      text += (text.empty() ? "" : ", ") + item.key.substr(item.key.find('.') + 1) + " " + item.value;
    }
    return text;
  }
};

[[nodiscard]] inline std::string trimmed(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// "strategy.enter_score=0.3|0.5" and so on: every combination.
[[nodiscard]] inline std::optional<std::vector<Candidate>> expand_grid(const std::vector<std::string>& grid,
                                                                       std::string& error) {
  std::vector<Candidate> candidates{Candidate{}};
  for (const std::string& item : grid) {
    const std::size_t equals = item.find('=');
    if (equals == std::string::npos) {
      error = "validation.grid: " + item + " is not name=value|value";
      return std::nullopt;
    }
    const std::string key = trimmed(item.substr(0, equals));
    std::vector<std::string> values;
    std::string rest = item.substr(equals + 1);
    for (std::size_t from = 0;;) {
      const std::size_t bar = rest.find('|', from);
      const std::string value = trimmed(rest.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
      if (!value.empty()) {
        values.push_back(value);
      }
      if (bar == std::string::npos) {
        break;
      }
      from = bar + 1;
    }
    if (key.empty() || values.empty()) {
      error = "validation.grid: " + item + " needs a name and at least one value";
      return std::nullopt;
    }
    std::vector<Candidate> next;
    for (const Candidate& candidate : candidates) {
      for (const std::string& value : values) {
        Candidate more = candidate;
        more.overrides.push_back(Override{key, value});
        next.push_back(std::move(more));
      }
    }
    candidates = std::move(next);
    if (candidates.size() > 256) {
      error = "validation.grid makes more than 256 combinations";
      return std::nullopt;
    }
  }
  return candidates;
}

// The settings with some of them changed. Nothing if the result has problems.
[[nodiscard]] inline std::optional<RunSetup> make_setup(const Settings& base, const std::vector<Override>& overrides,
                                                        std::string& error) {
  Settings settings = base;
  for (const Override& item : overrides) {
    settings.set(item.key, item.value);
  }
  RunSetup setup = read_setup(settings);
  const std::vector<std::string> problems = settings.problems();
  if (!problems.empty()) {
    error.clear();
    for (const std::string& problem : problems) {
      error += (error.empty() ? "" : "; ") + problem;
    }
    return std::nullopt;
  }
  return setup;
}

[[nodiscard]] inline std::string number_text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  return buffer;
}

// --- Statistics ---------------------------------------------------------------------

// Ranks, 1 for the smallest, ties sharing the average.
[[nodiscard]] inline std::vector<double> ranks(const std::vector<double>& values) {
  std::vector<std::size_t> order(values.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return values[a] < values[b]; });
  std::vector<double> rank(values.size());
  for (std::size_t from = 0; from < order.size();) {
    std::size_t to = from;
    while (to + 1 < order.size() && values[order[to + 1]] == values[order[from]]) {
      ++to;
    }
    for (std::size_t index = from; index <= to; ++index) {
      rank[order[index]] = (static_cast<double>(from + to) / 2.0) + 1;
    }
    from = to + 1;
  }
  return rank;
}

[[nodiscard]] inline double rank_correlation(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() < 3) {
    return 0;
  }
  const std::vector<double> ra = ranks(a);
  const std::vector<double> rb = ranks(b);
  SignalCheck::Sums sums;
  for (std::size_t index = 0; index < ra.size(); ++index) {
    sums.add(ra[index], rb[index]);
  }
  return sums.correlation();
}

// How sure it is that one profit per trade beats another (Welch's t).
[[nodiscard]] inline double welch_t(const Summary& a, const Summary& b) {
  if (a.trades < 2 || b.trades < 2) {
    return 0;
  }
  const double spread = a.stdev * a.stdev / static_cast<double>(a.trades) + b.stdev * b.stdev / static_cast<double>(b.trades);
  return spread > 0 ? (a.per_trade - b.per_trade) / std::sqrt(spread) : 0;
}

// --- The validation -------------------------------------------------------------------

struct WalkStep {
  std::uint32_t train_first = 0;
  std::uint32_t test_first = 0;  // also where training ends
  std::uint32_t test_end = 0;
  std::size_t chosen = 0;
  Summary train;                 // the chosen candidate, where it was chosen
  Summary test;                  // and on what came next
  double rank_correlation = 0;   // of all candidates' training profit with their test profit
  bool best_below_median = false;
};

struct Variation {
  std::string label;
  Summary summary;
};

struct Validation {
  std::string error;
  std::uint32_t walk_sessions = 0;
  std::uint32_t total_sessions = 0;
  std::uint64_t runs = 0;
  double seconds = 0;

  std::vector<Candidate> candidates;
  std::vector<Summary> candidate_in_sample;  // each candidate over the walk-forward sessions
  std::vector<WalkStep> steps;
  Summary in_sample;            // the chosen candidates, on the sessions they were chosen on
  Summary out_of_sample;        // and on the sessions after
  Summary baseline_out_of_sample;
  double mean_rank_correlation = 0;
  double best_below_median_share = 0;

  std::size_t final_choice = 0;
  Summary final_in_sample;
  Summary holdout;
  Summary baseline_holdout;
  double baseline_gap_t = 0;    // out of sample: how far the strategy's profit per trade is above the baseline's

  Summary reference;            // the final settings over every session: what the rows below are compared with
  std::vector<Variation> sensitivity;
  std::vector<Variation> stress;
  SignalCheck signals;          // from the final settings' run
  StrategyCounters counters;    // the final settings' run
  std::vector<std::string> unchanged;  // stress tests that changed nothing at all

  bool edge = false;
  std::string verdict;          // one line
  std::vector<std::string> reasons;
};

// Runs all of it. `base` is the settings file, not yet read.
[[nodiscard]] inline Validation validate(const Settings& base, std::FILE* progress = nullptr) {
  const auto started = std::chrono::steady_clock::now();
  Validation out;
  std::string error;
  const std::optional<RunSetup> plain = make_setup(base, {}, error);
  if (!plain) {
    out.error = error;
    return out;
  }
  const ValidationConfig& config = plain->validation;
  const std::uint32_t walk = config.sessions;
  const std::uint32_t total = walk + config.holdout_sessions;
  out.walk_sessions = walk;
  out.total_sessions = total;

  std::vector<Override> common{{"strategy.max_run_loss", "1e18"}};
  if (plain->ticks_file.empty()) {
    common.push_back({"market.sessions", std::to_string(total)});
  }

  const std::optional<std::vector<Candidate>> candidates = expand_grid(config.grid, error);
  if (!candidates) {
    out.error = error;
    return out;
  }
  out.candidates = *candidates;

  // 1. Every candidate, over every session.
  std::vector<RunSetup> setups;
  for (const Candidate& candidate : out.candidates) {
    std::vector<Override> overrides = common;
    overrides.insert(overrides.end(), candidate.overrides.begin(), candidate.overrides.end());
    const std::optional<RunSetup> setup = make_setup(base, overrides, error);
    if (!setup) {
      out.error = "candidate " + candidate.label() + ": " + error;
      return out;
    }
    setups.push_back(*setup);
  }
  if (progress != nullptr) {
    std::fprintf(progress, "Tuning: %zu candidates x %u sessions...\n", setups.size(), total);
  }
  std::vector<RunOutput> runs = run_many(setups, config.threads);
  out.runs += runs.size();
  for (const RunOutput& run : runs) {
    if (!run.error.empty()) {
      out.error = run.error;
      return out;
    }
    if (run.result.sessions < total) {
      out.error = "the market has " + std::to_string(run.result.sessions) + " sessions; validation needs " +
                  std::to_string(total) + " (sessions + holdout_sessions)";
      return out;
    }
  }

  // 2. Walk-forward.
  std::vector<std::pair<const RunResult*, std::pair<std::uint32_t, std::uint32_t>>> oos_parts;
  std::vector<std::pair<const RunResult*, std::pair<std::uint32_t, std::uint32_t>>> is_parts;
  std::size_t below = 0;
  double correlation_sum = 0;
  for (std::uint32_t first = 0; first + config.train_sessions + config.test_sessions <= walk;
       first += config.test_sessions) {
    WalkStep step;
    step.train_first = first;
    step.test_first = first + config.train_sessions;
    step.test_end = step.test_first + config.test_sessions;
    std::vector<double> train_net;
    std::vector<double> test_net;
    for (const RunOutput& run : runs) {
      train_net.push_back(summarize(run.result, step.train_first, step.test_first).net);
      test_net.push_back(summarize(run.result, step.test_first, step.test_end).net);
    }
    step.chosen = static_cast<std::size_t>(std::max_element(train_net.begin(), train_net.end()) - train_net.begin());
    step.train = summarize(runs[step.chosen].result, step.train_first, step.test_first);
    step.test = summarize(runs[step.chosen].result, step.test_first, step.test_end);
    step.rank_correlation = rank_correlation(train_net, test_net);
    std::vector<double> sorted = test_net;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.size() % 2 == 1 ? sorted[sorted.size() / 2]
                                                 : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2;
    step.best_below_median = runs.size() > 1 && test_net[step.chosen] < median;
    below += step.best_below_median ? 1 : 0;
    correlation_sum += step.rank_correlation;
    oos_parts.push_back({&runs[step.chosen].result, {step.test_first, step.test_end}});
    is_parts.push_back({&runs[step.chosen].result, {step.train_first, step.test_first}});
    out.steps.push_back(step);
  }
  out.out_of_sample = combine(oos_parts);
  out.in_sample = combine(is_parts);
  if (!out.steps.empty()) {
    out.mean_rank_correlation = correlation_sum / static_cast<double>(out.steps.size());
    out.best_below_median_share = static_cast<double>(below) / static_cast<double>(out.steps.size());
  }

  // 3. The final choice, and the holdout.
  for (const RunOutput& run : runs) {
    out.candidate_in_sample.push_back(summarize(run.result, 0, walk));
  }
  for (std::size_t index = 1; index < runs.size(); ++index) {
    if (out.candidate_in_sample[index].net > out.candidate_in_sample[out.final_choice].net) {
      out.final_choice = index;
    }
  }
  const RunResult& final_run = runs[out.final_choice].result;
  out.final_in_sample = out.candidate_in_sample[out.final_choice];
  out.holdout = summarize(final_run, walk, total);
  out.reference = summarize(final_run, 0, total);
  out.signals = runs[out.final_choice].signals;
  out.counters = runs[out.final_choice].counters;
  const RunSetup& chosen = setups[out.final_choice];

  // 4, 6, 7. The baseline, sensitivity and stress: the final settings with one thing changed.
  std::vector<Override> final_overrides = common;
  final_overrides.insert(final_overrides.end(), out.candidates[out.final_choice].overrides.begin(),
                         out.candidates[out.final_choice].overrides.end());
  std::vector<std::string> labels;
  std::vector<RunSetup> more;
  std::vector<bool> is_stress;
  const auto add = [&](const std::string& label, std::vector<Override> changes, bool stress) {
    std::vector<Override> overrides = final_overrides;
    overrides.insert(overrides.end(), changes.begin(), changes.end());
    const std::optional<RunSetup> setup = make_setup(base, overrides, error);
    if (setup) {
      labels.push_back(label);
      more.push_back(*setup);
      is_stress.push_back(stress);
    }
  };
  add("baseline", {{"strategy.random_entries", "true"}}, false);

  struct Knob {
    const char* key;
    double value;
    bool whole;
  };
  const StrategyConfig& s = chosen.strategy;
  const Knob knobs[] = {
      {"strategy.enter_score", s.enter_score, false},
      {"strategy.cancel_score", s.cancel_score, false},
      {"strategy.max_toxicity", s.max_toxicity, false},
      {"strategy.scratch_score", s.scratch_score, false},
      {"strategy.w_obi", s.w_obi, false},
      {"strategy.w_micro", s.w_micro, false},
      {"strategy.w_flow", s.w_flow, false},
      {"strategy.target_ticks", static_cast<double>(s.target_ticks), true},
      {"strategy.time_stop_ms", static_cast<double>(s.time_stop_ms), true},
      {"strategy.entry_timeout_ms", static_cast<double>(s.entry_timeout_ms), true},
  };
  for (const Knob& knob : knobs) {
    std::vector<double> values;
    if (std::string(knob.key) == "strategy.target_ticks") {
      values = {knob.value - 1, knob.value + 1};
    } else if (knob.value == 0) {
      values = {knob.whole ? 1.0 : 0.5};
    } else {
      values = {knob.value * 0.5, knob.value * 1.5};
    }
    for (double value : values) {
      if (knob.whole) {
        value = std::max(1.0, std::round(value));
      }
      if (value == knob.value) {
        continue;
      }
      const std::string name = std::string(knob.key).substr(9);
      add(name + " " + number_text(knob.value) + " -> " + number_text(value), {{knob.key, number_text(value)}}, false);
    }
  }

  const SimConfig& exchange = chosen.exchange;
  for (const int times : {5, 50, 500}) {
    add("Delays x" + std::to_string(times) + " (" + number_text(static_cast<double>(exchange.order_latency_us * times) / 1000) + " ms orders)",
        {{"exchange.feed_latency_us", std::to_string(exchange.feed_latency_us * times)},
         {"exchange.order_latency_us", std::to_string(exchange.order_latency_us * times)},
         {"exchange.cancel_latency_us", std::to_string(exchange.cancel_latency_us * times)}},
        true);
  }
  for (const double commission : {0.001, 0.0035, 0.005}) {
    add("Commission $" + number_text(commission) + " a share", {{"exchange.commission_per_share", number_text(commission)}},
        true);
  }
  add("Back of the queue (cancels never move it up)", {{"exchange.queue", "back"}}, true);
  if (!exchange.print_fills) {
    add("Trade prints fill resting orders (not how Gomer Trading works)", {{"exchange.print_fills", "true"}}, true);
  } else {
    add("Trade prints do not fill resting orders (how Gomer Trading works)", {{"exchange.print_fills", "false"}}, true);
  }
  if (chosen.market.impact && chosen.ticks_file.empty()) {
    add("Its own trades do not move prices", {{"market.impact", "false"}}, true);
  }

  if (progress != nullptr) {
    std::fprintf(progress, "Baseline, sensitivity and stress: %zu runs...\n", more.size());
  }
  std::vector<RunOutput> extra = run_many(more, config.threads);
  out.runs += extra.size();
  for (std::size_t index = 0; index < extra.size(); ++index) {
    if (!extra[index].error.empty()) {
      out.error = extra[index].error;
      return out;
    }
    const Summary summary = summarize(extra[index].result, 0, total);
    if (labels[index] == "baseline") {
      std::vector<std::pair<const RunResult*, std::pair<std::uint32_t, std::uint32_t>>> parts;
      for (const WalkStep& step : out.steps) {
        parts.push_back({&extra[index].result, {step.test_first, step.test_end}});
      }
      out.baseline_out_of_sample = combine(parts);
      out.baseline_holdout = summarize(extra[index].result, walk, total);
    } else {
      (is_stress[index] ? out.stress : out.sensitivity).push_back(Variation{labels[index], summary});
    }
  }
  out.baseline_gap_t = welch_t(out.out_of_sample, out.baseline_out_of_sample);

  // 8. The verdict.
  const Summary& oos = out.out_of_sample;
  const Summary& holdout = out.holdout;
  const auto find_stress = [&](const std::string& prefix) -> const Summary* {
    for (const Variation& variation : out.stress) {
      if (variation.label.rfind(prefix, 0) == 0) {
        return &variation.summary;
      }
    }
    return nullptr;
  };
  const bool enough = oos.trades >= 30 && holdout.trades >= 30;
  const bool profitable = oos.net > 0 && holdout.net > 0;
  const bool significant = oos.t_stat >= 2 && holdout.t_stat >= 2;
  const bool beats_baseline = out.baseline_gap_t >= 2;
  const Summary* slower = find_stress("Delays x5");
  const Summary* costs = find_stress("Commission $0.001");
  const bool survives = (slower == nullptr || slower->net > 0) && (costs == nullptr || costs->net > 0);
  out.edge = enough && profitable && significant && beats_baseline && survives;

  if (!enough) {
    out.reasons.push_back("Too few trades to judge: " + std::to_string(oos.trades) + " out of sample and " +
                          std::to_string(holdout.trades) + " in the holdout (30 or more of each are needed).");
  }
  if (oos.net <= 0) {
    out.reasons.push_back("It lost " + money(-oos.net) + " on the walk-forward test sessions, which it was not tuned on.");
  }
  if (holdout.net <= 0) {
    out.reasons.push_back("It lost " + money(-holdout.net) + " on the holdout sessions.");
  }
  if (profitable && !significant) {
    out.reasons.push_back("It made money, but not by enough to tell from luck (t = " + format("%.2f", oos.t_stat) +
                          " out of sample, " + format("%.2f", holdout.t_stat) + " in the holdout; about 2 is needed).");
  }
  if (!beats_baseline) {
    out.reasons.push_back(std::string(out.baseline_gap_t <= -2 ? "Its signal did worse than a coin toss"
                                                               : "Its signal did not beat a coin toss") +
                          ": profit per trade " + money(oos.per_trade) + " against the random-entry baseline's " +
                          money(out.baseline_out_of_sample.per_trade) + " out of sample (t = " +
                          format("%.2f", out.baseline_gap_t) + ").");
  }
  if (profitable && !survives) {
    out.reasons.push_back("It does not survive realistic stress: 5x the delays or $0.001 a share of commission turns it to a loss.");
  }
  double best_correlation = 0;
  for (std::size_t signal = 0; signal < SignalCheck::kSignals; ++signal) {
    best_correlation = std::max(best_correlation, std::abs(out.signals.sums(0, signal).correlation()));
  }
  if (best_correlation < 0.02) {
    out.reasons.push_back("None of the signals predicts the price: the strongest correlation with the next second's move is " +
                          format("%.3f", best_correlation) + ", where a usable signal would show a few hundredths or more.");
  }
  if (out.counters.kill_switches > 0) {
    out.reasons.push_back("The session kill switch stopped trading in " + std::to_string(out.counters.kill_switches) +
                          " of " + std::to_string(total) + " sessions, so the net profit figures mostly measure that "
                          "limit; profit per trade is the fairer comparison.");
  }
  for (const Variation& variation : out.stress) {
    if (summary_json(variation.summary) == summary_json(out.reference)) {
      out.unchanged.push_back(variation.label);
    }
  }
  if (out.edge) {
    out.verdict = "An edge, on this data. It made money out of sample and in the holdout, beat the coin-toss baseline "
                  "and survived the stress tests. That is evidence, not a promise: markets change.";
  } else if (!enough) {
    out.verdict = "Not enough trades to tell.";
  } else if (!profitable) {
    out.verdict = "No edge. The strategy loses money on sessions it was not tuned on.";
  } else {
    out.verdict = "No reliable edge. It made money in places, but the checks below say not to trust it.";
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return out;
}

// --- Writing it up --------------------------------------------------------------------

[[nodiscard]] inline std::string validation_markdown(const Validation& v, const RunSetup& setup) {
  std::string md;
  md += "# HFT Strategy: validation\n\n";
  if (!v.error.empty()) {
    return md + "Validation could not run: " + v.error + "\n";
  }
  md += "**Success rate out of sample: " + success_text(v.out_of_sample) + "**";
  const auto [low, high] = v.out_of_sample.success_range();
  if (v.out_of_sample.trades > 0) {
    md += format(" (the true rate is likely %.0f", low * 100) + format("-%.0f%%)", high * 100);
  }
  md += "  \n**Holdout: " + success_text(v.holdout) + "**\n\n";
  md += "**Verdict: " + v.verdict + "**\n\n";
  for (const std::string& reason : v.reasons) {
    md += "- " + reason + "\n";
  }
  md += "\nA success rate alone says little: a strategy can win most trades and still lose money if the losers are "
        "bigger. Read it with profit per trade and its t (about 2 or more before it means anything).\n\n";

  md += "## Out of sample\n\n";
  md += summary_header("Sessions");
  md += summary_row("Walk-forward tests (" + std::to_string(v.out_of_sample.sessions) + ")", v.out_of_sample);
  md += summary_row("Holdout (" + std::to_string(v.holdout.sessions) + ")", v.holdout);
  md += summary_row("Coin-toss baseline, walk-forward tests", v.baseline_out_of_sample);
  md += summary_row("Coin-toss baseline, holdout", v.baseline_holdout);
  md += "\n";

  md += "## Overfitting\n\n";
  md += summary_header("Chosen settings");
  md += summary_row("In sample (where they were chosen)", v.in_sample);
  md += summary_row("Out of sample (the sessions after)", v.out_of_sample);
  md += "\n";
  md += "- Candidates tried: " + std::to_string(v.candidates.size()) + ".\n";
  md += "- Rank correlation, training to test: " + format("%.2f", v.mean_rank_correlation) +
        " on average (1: the best in training stay best; 0: training says nothing about the test).\n";
  md += "- Training winner below the test median: " + format("%.0f%%", v.best_below_median_share * 100) +
        " of steps (about 50% means the choice is luck).\n\n";

  md += "### Walk-forward steps\n\n";
  md += "| Train | Test | Chosen | Train net | Test: success rate | Test net | Rank corr. |\n|---|---|---|---|---|---|---|\n";
  for (const WalkStep& step : v.steps) {
    md += "| " + std::to_string(step.train_first + 1) + "-" + std::to_string(step.test_first) + " | " +
          std::to_string(step.test_first + 1) + "-" + std::to_string(step.test_end) + " | " +
          v.candidates[step.chosen].label() + " | " + money(step.train.net) + " | " + success_text(step.test) + " | " +
          money(step.test.net) + " | " + format("%.2f", step.rank_correlation) + " |\n";
  }
  md += "\n";

  md += "## Does the book predict the price?\n\n";
  md += "Correlation of each signal with the move of the mid that follows (from the final settings' run):\n\n";
  md += "| Signal | Next 1 s | Next 5 s | Cents per unit, 1 s |\n|---|---|---|---|\n";
  for (std::size_t signal = 0; signal < SignalCheck::kSignals; ++signal) {
    md += std::string("| ") + SignalCheck::kNames[signal] + " | " + format("%.3f", v.signals.sums(0, signal).correlation()) +
          " | " + format("%.3f", v.signals.sums(1, signal).correlation()) + " | " +
          format("%.3f", v.signals.sums(0, signal).slope()) + " |\n";
  }
  md += "\nOver " + format("%.0f", v.signals.sums(0, 0).n) +
        " quotes. A useful signal for this kind of trading would show a correlation of a few hundredths or more.\n\n";

  md += "## Sensitivity\n\nThe final settings with one setting moved, over all " + std::to_string(v.total_sessions) +
        " sessions (these include the ones they were tuned on, so compare each row with the first, not with zero).\n\n";
  md += summary_header("Change");
  md += summary_row("None (final settings)", v.reference);
  for (const Variation& variation : v.sensitivity) {
    md += summary_row(variation.label, variation.summary);
  }
  md += "\n## Stress\n\n";
  if (!v.unchanged.empty()) {
    md += "Changed nothing at all: ";
    for (std::size_t index = 0; index < v.unchanged.size(); ++index) {
      md += (index ? "; " : "") + v.unchanged[index];
    }
    md += ".";
    if (!setup.exchange.print_fills) {
      md += " In a market that quotes twice a second and fills resting orders only when the price moves through "
            "them, the whole level is cleared at once, so neither the place in the queue nor a few milliseconds "
            "of delay can make a difference.";
    }
    md += "\n\n";
  }
  md += summary_header("Change");
  md += summary_row("None (final settings)", v.reference);
  for (const Variation& variation : v.stress) {
    md += summary_row(variation.label, variation.summary);
  }

  md += "\n## How it was run\n\n";
  const Candidate& final_candidate = v.candidates[v.final_choice];
  md += "- Final settings: " + final_candidate.label() + ".\n";
  md += "- Market: " + (setup.ticks_file.empty() ? "Gomer Trading's practice market, seed " + std::to_string(setup.market.seed) +
                                                      ", " + std::to_string(setup.market.ticks_per_session) + " ticks a session"
                                                : "tick file " + setup.ticks_file) +
        "; " + std::to_string(v.walk_sessions) + " walk-forward sessions (train " + std::to_string(setup.validation.train_sessions) +
        ", test " + std::to_string(setup.validation.test_sessions) + ") then " +
        std::to_string(v.total_sessions - v.walk_sessions) + " holdout.\n";
  md += "- Exchange: delays " + std::to_string(setup.exchange.feed_latency_us) + " / " +
        std::to_string(setup.exchange.order_latency_us) + " / " + std::to_string(setup.exchange.cancel_latency_us) +
        " us (feed / order / cancel), queue " + queue_name(setup.exchange.queue) + ", commission $" +
        number_text(setup.exchange.commission_per_share) + " a share, trade prints " +
        (setup.exchange.print_fills ? "fill" : "do not fill") + " resting orders.\n";
  md += "- " + std::to_string(v.runs) + " backtests in " + format("%.0f", v.seconds) + " s. Nothing was written to disk but this report.\n";
  md += "- The run-level kill switch was off so that every session counts; the session kill switch was on.\n";
  return md;
}

[[nodiscard]] inline std::string validation_json(const Validation& v) {
  std::string json = "{";
  json += "\"error\":\"" + json_escape(v.error) + "\"";
  if (!v.error.empty()) {
    return json + "}";
  }
  json += ",\"edge\":" + std::string(v.edge ? "true" : "false");
  json += ",\"verdict\":\"" + json_escape(v.verdict) + "\",\"reasons\":[";
  for (std::size_t index = 0; index < v.reasons.size(); ++index) {
    json += (index ? ",\"" : "\"") + json_escape(v.reasons[index]) + "\"";
  }
  json += "],\"out_of_sample\":" + summary_json(v.out_of_sample);
  json += ",\"holdout\":" + summary_json(v.holdout);
  json += ",\"baseline_out_of_sample\":" + summary_json(v.baseline_out_of_sample);
  json += ",\"baseline_holdout\":" + summary_json(v.baseline_holdout);
  json += ",\"in_sample\":" + summary_json(v.in_sample);
  json += ",\"baseline_gap_t\":" + format("%.4f", v.baseline_gap_t);
  json += ",\"mean_rank_correlation\":" + format("%.4f", v.mean_rank_correlation);
  json += ",\"best_below_median_share\":" + format("%.4f", v.best_below_median_share);
  json += ",\"final_settings\":\"" + json_escape(v.candidates[v.final_choice].label()) + "\"";
  json += ",\"candidates\":" + std::to_string(v.candidates.size());
  json += ",\"steps\":[";
  for (std::size_t index = 0; index < v.steps.size(); ++index) {
    const WalkStep& step = v.steps[index];
    json += std::string(index ? "," : "") + "{\"train_first\":" + std::to_string(step.train_first) +
            ",\"test_first\":" + std::to_string(step.test_first) + ",\"test_end\":" + std::to_string(step.test_end) +
            ",\"chosen\":\"" + json_escape(v.candidates[step.chosen].label()) + "\",\"train\":" + summary_json(step.train) +
            ",\"test\":" + summary_json(step.test) + ",\"rank_correlation\":" + format("%.4f", step.rank_correlation) + "}";
  }
  json += "],\"signals\":[";
  for (std::size_t signal = 0; signal < SignalCheck::kSignals; ++signal) {
    json += std::string(signal ? "," : "") + "{\"name\":\"" + SignalCheck::kNames[signal] +
            "\",\"correlation_1s\":" + format("%.5f", v.signals.sums(0, signal).correlation()) +
            ",\"correlation_5s\":" + format("%.5f", v.signals.sums(1, signal).correlation()) + "}";
  }
  json += "],\"reference\":" + summary_json(v.reference);
  for (const auto* list : {&v.sensitivity, &v.stress}) {
    json += list == &v.sensitivity ? ",\"sensitivity\":[" : ",\"stress\":[";
    for (std::size_t index = 0; index < list->size(); ++index) {
      json += std::string(index ? "," : "") + "{\"change\":\"" + json_escape((*list)[index].label) +
              "\",\"result\":" + summary_json((*list)[index].summary) + "}";
    }
    json += "]";
  }
  json += ",\"runs\":" + std::to_string(v.runs) + ",\"seconds\":" + format("%.1f", v.seconds) + "}";
  return json;
}

}  // namespace bt
