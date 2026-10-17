// Prints what Gomer Trading's own market model does with a given seed, so
// that the C++ port in backtest/practice_market.hpp can be checked against it.
//
//   cd "Project 2/backend" && npx tsx "../../Project 1/backtest/tools/practice_market_golden.ts" \
//       > "../../Project 1/tests/data/practice_market_golden.txt"
//
// Each line: step, listing index, price, variance, activity (at that step).
import { LISTINGS } from '../../../Project 2/backend/src/market/universe.ts';
import { MarketModel, seededRandom } from '../../../Project 2/backend/src/market/model.ts';

const SEED = 7;
const model = new MarketModel(LISTINGS, seededRandom(SEED));
const out: string[] = [`# seed ${SEED}; step listing price variance activity`];
const report = (step: number, activity: Map<string, number> | null) => {
  LISTINGS.forEach((listing, index) => {
    out.push(
      [step, index, model.priceOf(listing.symbol).toPrecision(17), model.varianceOf(listing.symbol).toPrecision(17), activity ? activity.get(listing.symbol)!.toPrecision(17) : '0'].join(' '),
    );
  });
};
report(0, null);
const marks = new Set([1, 2, 10, 100, 1000, 3000, 6000]);
for (let step = 1; step <= 6000; step++) {
  const { activity } = model.step();
  if (step === 2000) model.applyFlow('AAPL', 25_000);
  if (step === 2500) model.applyFlow('NVDA', -4_000);
  if (marks.has(step)) report(step, activity);
}
model.reopen(1.3);
report(6001, null);
for (let step = 6002; step <= 6500; step++) {
  const { activity } = model.step();
  if (step === 6500) report(step, activity);
}
console.log(out.join('\n'));

// The random source itself, and the levels the depth model puts behind a quote.
import { buildBook } from '../../../Project 2/backend/src/market/depth.ts';
const random = seededRandom(12345);
const draws: string[] = [];
for (let i = 0; i < 8; i++) draws.push(random().toPrecision(17));
console.log(`random ${draws.join(' ')}`);
for (const [symbol, bid, ask, bidSize, askSize, now] of [
  ['AAPL', 228.49, 228.51, 500, 300, 1_791_811_800_000],
  ['F', 10.69, 10.7, 2_400, 7, 1_791_811_823_456],
  ['NFLX', 712.31, 712.36, 100, 1_200, 1_791_900_000_000],
] as const) {
  const book = buildBook({ symbol, bid, ask, bidSize, askSize }, { bids: 0, asks: 0 }, now);
  console.log(`depth ${symbol} ${now} bids ${book.bids.map((l) => `${Math.round(l.price * 100)}:${l.size}`).join(',')} asks ${book.asks.map((l) => `${Math.round(l.price * 100)}:${l.size}`).join(',')}`);
}
