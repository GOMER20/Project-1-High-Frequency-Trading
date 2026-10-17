// hftbt: the HFT Strategy's backtester, from the command line.
//
//   hftbt run        [--config FILE] [--set section.name=value]... [--json FILE]
//   hftbt validate   [--config FILE] [--set ...]... [--out DIR]
//   hftbt generate   [--config FILE] [--set ...]... --out FILE
//   hftbt import-hft RECORDING --out FILE
//   hftbt info       FILE
//
// Everything is simulated: no order leaves this program.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "backtest/config.hpp"
#include "backtest/import.hpp"
#include "backtest/report.hpp"
#include "backtest/setup.hpp"
#include "backtest/strategy.hpp"
#include "backtest/ticks.hpp"
#include "backtest/validate.hpp"
#include "lob/recording.hpp"

namespace {

int usage() {
  std::fputs(
      "usage:\n"
      "  hftbt run        [--config FILE] [--set section.name=value]... [--json FILE]\n"
      "  hftbt validate   [--config FILE] [--set section.name=value]... [--out DIR]\n"
      "  hftbt generate   [--config FILE] [--set section.name=value]... --out FILE\n"
      "  hftbt import-hft RECORDING --out FILE\n"
      "  hftbt info       FILE\n"
      "\n"
      "run       backtests HFT Strategy on the [market] (or a tick file) and reports, success rate first\n"
      "validate  walk-forward, holdout, baseline, sensitivity and stress tests, and a verdict\n"
      "generate  writes the [market] as a tick file\n"
      "import-hft turns a recording of an HFT session into a tick file\n"
      "info      describes a tick file\n",
      stderr);
  return 2;
}

struct Arguments {
  std::string command;
  std::vector<std::string> positional;
  std::string config;
  std::vector<std::pair<std::string, std::string>> sets;
  std::string out;
  std::string json;
};

std::optional<Arguments> parse(int argc, char** argv) {
  if (argc < 2) {
    return std::nullopt;
  }
  Arguments arguments;
  arguments.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&]() -> std::optional<std::string> {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "hftbt: %s needs a value\n", argument.c_str());
        return std::nullopt;
      }
      return std::string(argv[++index]);
    };
    if (argument == "--config" || argument == "--out" || argument == "--json" || argument == "--set") {
      const std::optional<std::string> given = value();
      if (!given) {
        return std::nullopt;
      }
      if (argument == "--config") {
        arguments.config = *given;
      } else if (argument == "--out") {
        arguments.out = *given;
      } else if (argument == "--json") {
        arguments.json = *given;
      } else {
        const std::size_t equals = given->find('=');
        if (equals == std::string::npos) {
          std::fprintf(stderr, "hftbt: --set %s is not section.name=value\n", given->c_str());
          return std::nullopt;
        }
        arguments.sets.emplace_back(given->substr(0, equals), given->substr(equals + 1));
      }
    } else if (!argument.empty() && argument[0] == '-') {
      std::fprintf(stderr, "hftbt: unknown option %s\n", argument.c_str());
      return std::nullopt;
    } else {
      arguments.positional.push_back(argument);
    }
  }
  return arguments;
}

std::optional<bt::Settings> load_settings(const Arguments& arguments) {
  std::string error;
  std::optional<bt::Settings> settings =
      arguments.config.empty() ? bt::Settings::parse("", error) : bt::Settings::load(arguments.config, error);
  if (!settings) {
    std::fprintf(stderr, "hftbt: %s\n", error.c_str());
    return std::nullopt;
  }
  for (const auto& [key, value] : arguments.sets) {
    settings->set(key, value);
  }
  return settings;
}

// Reads the settings into a setup, or reports every problem and gives up.
std::optional<bt::RunSetup> setup_from(bt::Settings settings) {
  bt::RunSetup setup = bt::read_setup(settings);
  const std::vector<std::string> problems = settings.problems();
  if (!problems.empty()) {
    for (const std::string& problem : problems) {
      std::fprintf(stderr, "hftbt: %s\n", problem.c_str());
    }
    return std::nullopt;
  }
  return setup;
}

bool write_file(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  file.close();
  if (!file) {
    std::fprintf(stderr, "hftbt: cannot write %s\n", path.c_str());
    return false;
  }
  return true;
}

std::string describe_market(const bt::RunSetup& setup) {
  if (!setup.ticks_file.empty()) {
    return "tick file " + setup.ticks_file;
  }
  return "Gomer Trading practice market, seed " + std::to_string(setup.market.seed) + ", " +
         std::to_string(setup.market.sessions) + " session" + (setup.market.sessions == 1 ? "" : "s") + " of " +
         std::to_string(setup.market.ticks_per_session) + " ticks";
}

int run(const Arguments& arguments) {
  const std::optional<bt::Settings> settings = load_settings(arguments);
  if (!settings) {
    return 1;
  }
  const std::optional<bt::RunSetup> setup = setup_from(*settings);
  if (!setup) {
    return 1;
  }
  const bt::RunOutput output = bt::run_one(*setup);
  if (!output.error.empty()) {
    std::fprintf(stderr, "hftbt: %s\n", output.error.c_str());
    return 1;
  }
  const bt::Summary summary = bt::summarize(output.result);
  std::printf("HFT Strategy backtest: %s\n\n", describe_market(*setup).c_str());
  std::fputs(bt::summary_lines(summary, "  ").c_str(), stdout);
  const bt::StrategyCounters& counters = output.counters;
  std::printf(
      "\n  Entries: %llu sent, %llu filled, %llu withdrawn; exits: %llu at target, %llu time stops, %llu scratched, "
      "%llu at the close\n"
      "  Orders sent: %llu; kill switch: %llu time%s; refused by limits: %llu; too toxic: %llu\n"
      "  Mark-to-market max drawdown: %s\n",
      static_cast<unsigned long long>(counters.entries), static_cast<unsigned long long>(counters.entries_filled),
      static_cast<unsigned long long>(counters.entries_withdrawn), static_cast<unsigned long long>(counters.target_exits),
      static_cast<unsigned long long>(counters.time_stops), static_cast<unsigned long long>(counters.scratches),
      static_cast<unsigned long long>(summary.forced), static_cast<unsigned long long>(output.result.orders_sent),
      static_cast<unsigned long long>(counters.kill_switches), counters.kill_switches == 1 ? "" : "s",
      static_cast<unsigned long long>(counters.refused_by_limits), static_cast<unsigned long long>(counters.too_toxic),
      bt::money(output.result.max_drawdown).c_str());
  std::puts("\n  One run says little: `hftbt validate` tests it out of sample against a coin-toss baseline.");
  if (!arguments.json.empty()) {
    std::string json = "{\"market\":\"" + bt::json_escape(describe_market(*setup)) + "\",\"summary\":" +
                       bt::summary_json(summary) + ",\"mark_to_market_max_drawdown\":" +
                       bt::format("%.4f", output.result.max_drawdown) + "}\n";
    if (!write_file(arguments.json, json)) {
      return 1;
    }
  }
  return 0;
}

int validate(const Arguments& arguments) {
  const std::optional<bt::Settings> settings = load_settings(arguments);
  if (!settings) {
    return 1;
  }
  const std::optional<bt::RunSetup> setup = setup_from(*settings);
  if (!setup) {
    return 1;
  }
  const bt::Validation validation = bt::validate(*settings, stderr);
  const std::string markdown = bt::validation_markdown(validation, *setup);
  std::fputs(markdown.c_str(), stdout);
  if (!validation.error.empty()) {
    return 1;
  }
  if (!arguments.out.empty()) {
    std::error_code error;
    std::filesystem::create_directories(arguments.out, error);
    const std::string md_path = arguments.out + "/validation.md";
    const std::string json_path = arguments.out + "/validation.json";
    if (!write_file(md_path, markdown) || !write_file(json_path, bt::validation_json(validation) + "\n")) {
      return 1;
    }
    std::fprintf(stderr, "Wrote %s and %s (%llu bytes).\n", md_path.c_str(), json_path.c_str(),
                 static_cast<unsigned long long>(std::filesystem::file_size(md_path) + std::filesystem::file_size(json_path)));
  }
  return 0;
}

int write_ticks(bt::TickSource& source, const std::string& path) {
  bt::TickFileWriter writer;
  if (!writer.open(path, source.symbols(), source.epoch_ms())) {
    std::fprintf(stderr, "hftbt: cannot create %s\n", path.c_str());
    return 1;
  }
  bt::Tick tick;
  while (source.next(tick)) {
    writer.write(tick);
  }
  const std::uint64_t written = writer.written();
  if (!writer.close()) {
    std::fprintf(stderr, "hftbt: writing %s failed (disk full?)\n", path.c_str());
    return 1;
  }
  const auto bytes = std::filesystem::file_size(path);
  std::printf("Wrote %llu events to %s: %.1f KB, %.2f bytes an event.\n", static_cast<unsigned long long>(written),
              path.c_str(), static_cast<double>(bytes) / 1024.0,
              written == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(written));
  return 0;
}

int generate(const Arguments& arguments) {
  if (arguments.out.empty()) {
    return usage();
  }
  const std::optional<bt::Settings> settings = load_settings(arguments);
  if (!settings) {
    return 1;
  }
  const std::optional<bt::RunSetup> setup = setup_from(*settings);
  if (!setup) {
    return 1;
  }
  bt::PracticeMarketOptions options = setup->market;
  options.impact = false;  // a stream on its own: nothing of ours in it
  bt::PracticeMarketSource source(options);
  return write_ticks(source, arguments.out);
}

int import_hft(const Arguments& arguments) {
  if (arguments.positional.size() != 1 || arguments.out.empty()) {
    return usage();
  }
  const std::optional<lob::Recording> recording = lob::Recording::load(arguments.positional[0]);
  if (!recording) {
    std::fprintf(stderr, "hftbt: %s is not a recording this build can read\n", arguments.positional[0].c_str());
    return 1;
  }
  bt::ImportResult imported = bt::import_recording(*recording);
  std::printf("Read %llu commands (%llu orders, %llu cancels)%s: %llu quotes and %llu trades.\n",
              static_cast<unsigned long long>(imported.commands), static_cast<unsigned long long>(imported.submits),
              static_cast<unsigned long long>(imported.cancels), imported.damaged ? ", cut short" : "",
              static_cast<unsigned long long>(imported.quotes), static_cast<unsigned long long>(imported.trades));
  bt::TickVectorSource source(imported.symbols, std::move(imported.ticks), bt::kPracticeEpochMs);
  return write_ticks(source, arguments.out);
}

int info(const Arguments& arguments) {
  if (arguments.positional.size() != 1) {
    return usage();
  }
  const std::string& path = arguments.positional[0];
  std::unique_ptr<bt::TickFileReader> reader = bt::TickFileReader::open(path);
  if (reader == nullptr) {
    std::fprintf(stderr, "hftbt: %s is not a tick file\n", path.c_str());
    return 1;
  }
  std::uint64_t counts[4] = {};
  std::uint32_t sessions = 0;
  std::int64_t first = 0;
  std::int64_t last = 0;
  bool any = false;
  bt::Tick tick;
  while (reader->next(tick)) {
    ++counts[static_cast<int>(tick.kind)];
    if (tick.kind == bt::TickKind::Open && tick.symbol == 0) {
      ++sessions;
    }
    first = any ? first : tick.time_us;
    last = tick.time_us;
    any = true;
  }
  std::printf("%s: %.1f KB\n", path.c_str(), static_cast<double>(std::filesystem::file_size(path)) / 1024.0);
  std::printf("  %zu symbols:", reader->symbols().size());
  for (const bt::SymbolInfo& symbol : reader->symbols()) {
    std::printf(" %s", symbol.name.c_str());
  }
  std::printf("\n  %u sessions, %llu quotes, %llu trades, %.1f hours from first to last\n", sessions,
              static_cast<unsigned long long>(counts[static_cast<int>(bt::TickKind::Quote)]),
              static_cast<unsigned long long>(counts[static_cast<int>(bt::TickKind::Trade)]),
              static_cast<double>(last - first) / 3.6e9);
  if (reader->damaged()) {
    std::puts("  The file is cut short or damaged; the above is as far as it is readable.");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Arguments> arguments = parse(argc, argv);
  if (!arguments) {
    return usage();
  }
  if (arguments->command == "run") {
    return run(*arguments);
  }
  if (arguments->command == "validate") {
    return validate(*arguments);
  }
  if (arguments->command == "generate") {
    return generate(*arguments);
  }
  if (arguments->command == "import-hft") {
    return import_hft(*arguments);
  }
  if (arguments->command == "info") {
    return info(*arguments);
  }
  return usage();
}
