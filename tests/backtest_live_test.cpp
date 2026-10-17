// Phase 6: the live side. Round trips counted live the way the backtester
// counts them, the gateway's framing, and the exact bytes of an order.
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "backtest/live.hpp"
#include "backtest/report.hpp"

namespace {

using bt::live::GatewayLink;
using bt::live::TripBook;
using lob::Side;

TEST(TripBook, CountsRoundTripsAsTheBacktesterDoes) {
  TripBook book(2);
  book.offered(100);
  book.fill(1'000'000, 0, Side::Buy, 1'000, 100, 0.0, true);  // bought at $10.00, resting
  book.mid(1'500'000, 0, 2'001);
  book.fill(2'000'000, 0, Side::Sell, 1'001, 100, 0.0, false);  // sold a cent higher
  const bt::RunResult result = book.result(3'000'000);
  ASSERT_EQ(result.trips.size(), 1u);
  EXPECT_NEAR(result.trips[0].pnl, 1.00, 1e-9);
  EXPECT_EQ(result.trips[0].side, Side::Buy);
  EXPECT_EQ(result.trips[0].shares, 100u);
  EXPECT_FALSE(result.trips[0].forced);
  EXPECT_NEAR(result.net_pnl, 1.00, 1e-9);
  const bt::Summary summary = bt::summarize(result);
  EXPECT_EQ(bt::success_text(summary), "100.0% (1 of 1 trades)");
  EXPECT_DOUBLE_EQ(summary.fill_rate, 1.0);
}

TEST(TripBook, ChargesCommissionAndCountsAnOpenPositionAtItsMark) {
  TripBook book(1);
  book.fill(0, 0, Side::Sell, 2'000, 50, 0.25, true);  // short 50 at $20.00, 25 cents commission
  book.mid(100, 0, 3'998);                              // the mid is now $19.99
  const bt::RunResult result = book.result(200);
  ASSERT_EQ(result.trips.size(), 1u);
  EXPECT_TRUE(result.trips[0].forced);  // still open
  EXPECT_NEAR(result.trips[0].pnl, 50 * 0.01 - 0.25, 1e-9);
  EXPECT_NEAR(result.commission, 0.25, 1e-12);
  EXPECT_EQ(book.shares(0), -50);
}

TEST(TripBook, AFillThroughZeroClosesOneTripAndOpensTheNext) {
  TripBook book(1);
  book.fill(0, 0, Side::Buy, 1'000, 100, 0.0, false);
  book.fill(10, 0, Side::Sell, 1'002, 150, 0.0, false);  // out of the 100, and 50 short
  book.fill(20, 0, Side::Buy, 1'001, 50, 0.0, false);
  const bt::RunResult result = book.result(30);
  ASSERT_EQ(result.trips.size(), 2u);
  EXPECT_NEAR(result.trips[0].pnl, 2.00, 1e-9);
  EXPECT_EQ(result.trips[1].side, Side::Sell);
  EXPECT_NEAR(result.trips[1].pnl, 0.50, 1e-9);
}

TEST(TripBook, MeasuresAdverseSelectionAfterAPassiveFill) {
  TripBook book(1);
  book.fill(0, 0, Side::Buy, 1'000, 100, 0.0, true);
  book.mid(1'000'000, 0, 1'998);  // a second on, the mid is a cent below where we bought
  book.mid(5'000'000, 0, 1'996);  // five seconds on, two
  const bt::Summary summary = bt::summarize(book.result(5'000'000));
  EXPECT_DOUBLE_EQ(summary.adverse_1s, 1.0);
  EXPECT_DOUBLE_EQ(summary.adverse_5s, 2.0);
}

class Pair : public ::testing::Test {
 protected:
  void SetUp() override {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    link.adopt(fds[0]);
    far = fds[1];
  }
  void TearDown() override { ::close(far); }

  // Reads exactly `size` bytes from the far end.
  std::vector<std::byte> take(std::size_t size) {
    std::vector<std::byte> bytes(size);
    std::size_t got = 0;
    while (got < size) {
      const ssize_t read = ::recv(far, bytes.data() + got, size - got, 0);
      if (read <= 0) {
        ADD_FAILURE() << "the far end closed";
        break;
      }
      got += static_cast<std::size_t>(read);
    }
    return bytes;
  }

  GatewayLink link;
  int far = -1;
};

TEST_F(Pair, FramesComeOutWholeHoweverTheyArriveCutUp) {
  bt::live::wire::BboMsg bbo{};
  bbo.h.length = sizeof(bbo);
  bbo.h.type = bt::live::wire::Bbo;
  bbo.h.version = 1;
  std::memcpy(bbo.symbol, "F", 1);
  bbo.bid = 106'900;
  bbo.ask = 107'000;
  std::array<std::byte, 2 * sizeof(bbo)> two{};
  std::memcpy(two.data(), &bbo, sizeof(bbo));
  std::memcpy(two.data() + sizeof(bbo), &bbo, sizeof(bbo));
  // The first frame and a bit of the second, then the rest.
  ASSERT_EQ(::send(far, two.data(), sizeof(bbo) + 5, 0), static_cast<ssize_t>(sizeof(bbo) + 5));
  std::vector<std::uint8_t> types;
  ASSERT_TRUE(link.poll(1'000, [&](std::uint8_t type, std::span<const std::byte> frame) {
    types.push_back(type);
    EXPECT_EQ(frame.size(), sizeof(bbo));
  }));
  EXPECT_EQ(types.size(), 1u);
  ASSERT_EQ(::send(far, two.data() + sizeof(bbo) + 5, sizeof(bbo) - 5, 0), static_cast<ssize_t>(sizeof(bbo) - 5));
  ASSERT_TRUE(link.poll(1'000, [&](std::uint8_t type, std::span<const std::byte> frame) {
    types.push_back(type);
    bt::live::wire::BboMsg read{};
    ASSERT_TRUE(bt::live::read_as(frame, read));
    EXPECT_EQ(bt::live::wire::to_cents(read.bid), 1'069);
    EXPECT_EQ(bt::live::wire::symbol_of(read.symbol), "F");
  }));
  EXPECT_EQ(types, (std::vector<std::uint8_t>{bt::live::wire::Bbo, bt::live::wire::Bbo}));
}

TEST_F(Pair, ClosingTheFarEndIsNoticed) {
  ::close(far);
  far = ::dup(0);  // something for TearDown to close
  EXPECT_FALSE(link.poll(1'000, [](std::uint8_t, std::span<const std::byte>) {}));
  EXPECT_FALSE(link.connected());
}

TEST_F(Pair, AnOrderGoesOutAsTheGatewaySpecifies) {
  TripBook trips(1);
  bt::live::LiveContext context(link, {{"SOFI", 890}}, trips);
  const std::uint64_t limit = context.submit(0, Side::Sell, 891, 100, bt::OrderKind::Limit);
  const std::uint64_t market = context.submit(0, Side::Buy, 0, 40, bt::OrderKind::Market);
  context.cancel(limit);

  bt::live::wire::NewOrderMsg first{};
  bt::live::wire::NewOrderMsg second{};
  bt::live::wire::OrderRefMsg cancel{};
  const auto a = take(sizeof(first));
  const auto b = take(sizeof(second));
  const auto c = take(sizeof(cancel));
  std::memcpy(&first, a.data(), sizeof(first));
  std::memcpy(&second, b.data(), sizeof(second));
  std::memcpy(&cancel, c.data(), sizeof(cancel));

  EXPECT_EQ(first.h.type, bt::live::wire::NewOrder);
  EXPECT_EQ(first.h.length, 48);
  EXPECT_EQ(first.h.seq, 1u);
  EXPECT_EQ(first.cl_ord_id, limit);
  EXPECT_EQ(std::string(first.symbol, 4), "SOFI");
  EXPECT_EQ(first.side, bt::live::wire::SideSell);
  EXPECT_EQ(first.ord_type, bt::live::wire::OrdLimit);
  EXPECT_EQ(first.tif, bt::live::wire::TifGtc);
  EXPECT_EQ(first.qty, 100u);
  EXPECT_EQ(first.price, 89'100);  // $8.91 in 1/10000 of a dollar

  EXPECT_EQ(second.cl_ord_id, market);
  EXPECT_EQ(second.ord_type, bt::live::wire::OrdMarket);
  EXPECT_EQ(second.tif, bt::live::wire::TifIoc);
  EXPECT_EQ(second.price, 0);
  EXPECT_EQ(second.h.seq, 2u);

  EXPECT_EQ(cancel.h.type, bt::live::wire::Cancel);
  EXPECT_EQ(cancel.cl_ord_id, limit);
  EXPECT_EQ(context.order_by_seq(3, true), limit);
  EXPECT_EQ(context.order_by_seq(2, false), market);
  EXPECT_NE(market, limit);
}

TEST(LiveClock, TimersFireInOrder) {
  GatewayLink link;
  TripBook trips(1);
  bt::live::LiveContext context(link, {{"F", 1'070}}, trips);
  const std::int64_t now = context.now();
  context.wake_at(now - 2, 2);
  context.wake_at(now - 3, 1);
  context.wake_at(now + 60'000'000, 3);  // a minute away: not yet
  std::vector<std::uint64_t> fired;
  context.fire_due([&](std::uint64_t token) { fired.push_back(token); });
  EXPECT_EQ(fired, (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(context.next_timer_us(), now + 60'000'000);
}

TEST(LiveClock, NewYorkTimeKnowsDaylightSaving) {
  // 2026-10-12 13:30 UTC was 9:30 in New York (daylight time)...
  EXPECT_EQ(bt::live::new_york(1'791'811'800'000'000).minutes, 9 * 60 + 30);
  EXPECT_EQ(bt::live::new_york(1'791'811'800'000'000).date, "2026-10-12");
  // ...and 2026-12-14 14:30 UTC was 9:30 there too (standard time).
  EXPECT_EQ(bt::live::new_york(1'797'258'600'000'000).minutes, 9 * 60 + 30);
}

}  // namespace
