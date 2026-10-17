# HFT Strategy: validation

**Success rate out of sample: 84.9% (75907 of 89369 trades)** (the true rate is likely 84.7-85.2%)  
**Holdout: 86.6% (30643 of 35383 trades)**

**Verdict: An edge, on this data. It made money out of sample and in the holdout, beat the coin-toss baseline and survived the stress tests. That is evidence, not a promise: markets change.**

- None of the signals predicts the price: the strongest correlation with the next second's move is 0.012, where a usable signal would show a few hundredths or more.
- Where the edge comes from: choosing where and when to quote, not which side. With the same filters and a coin toss for the side it makes $0.54 a trade out of sample, against the strategy's $0.54; with no filters at all, -$2.03.

**Where it stops working:**

- It loses money with this change: Commission $0.0035 a share (-$0.04 a trade).
- It loses money with this change: Commission $0.005 a share (-$0.29 a trade).
- It loses money with this change: Trade prints do not fill resting orders (as in standard competitions) (-$0.56 a trade).

A success rate alone says little: a strategy can win most trades and still lose money if the losers are bigger. Read it with profit per trade and its t (about 2 or more before it means anything).

## Out of sample

| Sessions | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| Walk-forward tests (16) | 84.9% (75907 of 89369 trades) | $48061.01 | $0.54 | 150.51 | -$54.00 | $59.40 | 69.7% | -0.39 / -0.37 |
| Holdout (6) | 86.6% (30643 of 35383 trades) | $19819.52 | $0.56 | 98.00 | -$58.00 | $134.47 | 71.4% | -0.41 / -0.40 |
| Coin-toss baseline, walk-forward tests | 36.5% (1495 of 4094 trades) | -$8318.98 | -$2.03 | -12.65 | -$234.00 | $8350.81 | 49.3% | 0.68 / 0.80 |
| Coin-toss baseline, holdout | 34.4% (479 of 1394 trades) | -$3132.95 | -$2.25 | -7.87 | -$192.00 | $3167.63 | 48.2% | 0.83 / 1.01 |
| Coin-toss side with the strategy's filters, walk-forward tests | 85.0% (75781 of 89195 trades) | $48257.14 | $0.54 | 154.32 | -$67.00 | $76.04 | 69.9% | -0.39 / -0.38 |
| Coin-toss side with the strategy's filters, holdout | 86.6% (30856 of 35633 trades) | $19875.72 | $0.56 | 95.07 | -$58.00 | $79.19 | 71.2% | -0.41 / -0.40 |

The coin-toss baseline enters at random, on a random side, using nothing it reads from the market; every exit rule is the same. The second baseline keeps the strategy's filters (volatility, toxicity) and tosses a coin only for the side: if it does about as well as the strategy, the edge is in choosing where and when to quote, not which side.

## Overfitting

| Chosen settings | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| In sample (where they were chosen) | 84.8% (156650 of 184743 trades) | $98538.89 | $0.53 | 210.62 | -$56.00 | $59.40 | 69.5% | -0.38 / -0.37 |
| Out of sample (the sessions after) | 84.9% (75907 of 89369 trades) | $48061.01 | $0.54 | 150.51 | -$54.00 | $59.40 | 69.7% | -0.39 / -0.37 |

- Candidates tried: 12.
- Rank correlation, training to test: 0.99 on average (1: the best in training stay best; 0: training says nothing about the test).
- Training winner below the test median: 0% of steps (about 50% means the choice is luck).

### Walk-forward steps

| Train | Test | Chosen | Train net | Test: success rate | Test net | Rank corr. |
|---|---|---|---|---|---|---|
| 1-4 | 5-6 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $15491.00 | 86.0% (12276 of 14272 trades) | $7944.44 | 1.00 |
| 3-6 | 7-8 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $16036.46 | 84.0% (11081 of 13190 trades) | $6580.36 | 1.00 |
| 5-8 | 9-10 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $14524.80 | 85.4% (9430 of 11041 trades) | $6004.41 | 0.99 |
| 7-10 | 11-12 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $12584.77 | 82.5% (7701 of 9339 trades) | $4425.44 | 0.99 |
| 9-12 | 13-14 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $10429.85 | 81.7% (7311 of 8948 trades) | $4321.38 | 0.97 |
| 11-14 | 15-16 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $8746.82 | 83.6% (7403 of 8855 trades) | $4768.48 | 0.99 |
| 13-16 | 17-18 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $9089.86 | 86.7% (10393 of 11992 trades) | $6866.85 | 0.95 |
| 15-18 | 19-20 | max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2 | $11635.33 | 87.9% (10312 of 11732 trades) | $7149.65 | 0.99 |

## Does the book predict the price?

Correlation of each signal with the move of the mid that follows (from the final settings' run):

| Signal | Next 1 s | Next 5 s | Cents per unit, 1 s |
|---|---|---|---|
| order-book imbalance | 0.000 | 0.000 | 0.025 |
| micro-price | -0.000 | -0.000 | -0.002 |
| trade flow | -0.000 | -0.000 | -0.002 |
| trend | 0.012 | 0.026 | 0.089 |
| combined score | -0.000 | -0.000 | -0.001 |

Over 41369398 quotes. A useful signal for this kind of trading would show a correlation of a few hundredths or more.

## Sensitivity

The final settings with one setting moved, over all 26 sessions (these include the ones they were tuned on, so compare each row with the first, not with zero).

| Change | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| None (final settings) | 85.6% (130238 of 152161 trades) | $83371.53 | $0.55 | 197.16 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| enter_score 1 -> 0.5 | 85.5% (127731 of 149359 trades) | $81341.44 | $0.54 | 187.76 | -$57.00 | $122.04 | 70.5% | -0.39 / -0.38 |
| enter_score 1 -> 1.5 | 85.6% (130251 of 152172 trades) | $83384.39 | $0.55 | 197.20 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| cancel_score -1 -> -0.5 | 85.2% (125671 of 147448 trades) | $79956.45 | $0.54 | 190.61 | -$57.00 | $102.25 | 65.9% | -0.39 / -0.38 |
| cancel_score -1 -> -1.5 | 85.6% (130265 of 152184 trades) | $83405.35 | $0.55 | 197.26 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| max_toxicity 0.6 -> 0.3 | 85.7% (126784 of 147976 trades) | $81029.15 | $0.55 | 196.25 | -$57.00 | $125.52 | 70.4% | -0.39 / -0.38 |
| max_toxicity 0.6 -> 0.9 | 85.6% (130209 of 152137 trades) | $83345.37 | $0.55 | 197.05 | -$58.00 | $138.89 | 70.4% | -0.39 / -0.38 |
| scratch_score 0 -> 0.5 | 65.4% (139207 of 212723 trades) | $55553.00 | $0.26 | 117.36 | -$72.00 | $78.86 | 62.1% | -0.36 / -0.34 |
| w_obi 1 -> 0.5 | 85.6% (130278 of 152195 trades) | $83418.21 | $0.55 | 197.30 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| w_obi 1 -> 1.5 | 85.6% (130146 of 152027 trades) | $83226.30 | $0.55 | 196.25 | -$58.00 | $130.62 | 70.3% | -0.39 / -0.38 |
| w_micro 1 -> 0.5 | 85.6% (130278 of 152195 trades) | $83418.21 | $0.55 | 197.30 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| w_micro 1 -> 1.5 | 85.6% (130136 of 151950 trades) | $83310.62 | $0.55 | 197.35 | -$58.00 | $121.94 | 70.2% | -0.39 / -0.38 |
| w_flow 0.5 -> 0.25 | 85.6% (130278 of 152195 trades) | $83418.21 | $0.55 | 197.30 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| w_flow 0.5 -> 0.75 | 85.6% (130252 of 152165 trades) | $83374.84 | $0.55 | 197.15 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| target_ticks 1 -> 2 | 50.5% (33617 of 66621 trades) | $25743.36 | $0.39 | 54.07 | -$70.00 | $178.05 | 57.4% | -0.44 / -0.44 |
| time_stop_ms 30000 -> 15000 | 83.8% (145256 of 173277 trades) | $87274.16 | $0.50 | 208.19 | -$70.00 | $73.07 | 68.7% | -0.38 / -0.37 |
| time_stop_ms 30000 -> 45000 | 86.0% (121294 of 141048 trades) | $77956.80 | $0.55 | 181.79 | -$65.00 | $72.22 | 70.9% | -0.40 / -0.39 |
| entry_timeout_ms 30000 -> 15000 | 85.6% (130223 of 152147 trades) | $83369.31 | $0.55 | 197.19 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| entry_timeout_ms 30000 -> 45000 | 85.6% (130238 of 152161 trades) | $83371.53 | $0.55 | 197.16 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| max_volatility 0.5 -> 0.25 | 90.1% (37025 of 41091 trades) | $24349.36 | $0.59 | 159.92 | -$33.00 | $33.00 | 71.4% | -0.43 / -0.43 |
| max_volatility 0.5 -> 0.75 | 80.5% (190745 of 237038 trades) | $98152.55 | $0.41 | 126.10 | -$142.00 | $232.70 | 67.6% | -0.35 / -0.32 |
| vol_halflife_ms 60000 -> 30000 | 85.4% (131335 of 153840 trades) | $82259.42 | $0.53 | 187.51 | -$67.00 | $110.94 | 70.0% | -0.39 / -0.38 |
| vol_halflife_ms 60000 -> 90000 | 85.7% (130115 of 151835 trades) | $84118.43 | $0.55 | 204.78 | -$58.00 | $94.51 | 70.6% | -0.39 / -0.38 |

## Stress

Changed nothing at all: Delays x5 (5 ms orders); Delays x50 (50 ms orders).

| Change | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| None (final settings) | 85.6% (130238 of 152161 trades) | $83371.53 | $0.55 | 197.16 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| Delays x5 (5 ms orders) | 85.6% (130238 of 152161 trades) | $83371.53 | $0.55 | 197.16 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| Delays x50 (50 ms orders) | 85.6% (130238 of 152161 trades) | $83371.53 | $0.55 | 197.16 | -$58.00 | $134.47 | 70.4% | -0.39 / -0.38 |
| Delays x500 (500 ms orders) | 80.9% (87380 of 107951 trades) | $55289.84 | $0.51 | 123.68 | -$69.00 | $144.15 | 67.5% | -0.45 / -0.44 |
| Commission $0.001 a share | 85.4% (129957 of 152161 trades) | $57870.37 | $0.38 | 137.63 | -$58.20 | $170.76 | 70.4% | -0.39 / -0.38 |
| Commission $0.0035 a share | 84.8% (125965 of 148480 trades) | -$5266.79 | -$0.04 | -12.96 | -$56.70 | $5347.15 | 70.5% | -0.39 / -0.38 |
| Commission $0.005 a share | 0.0% (0 of 45072 trades) | -$13058.96 | -$0.29 | -54.86 | -$57.00 | $13058.96 | 70.6% | -0.39 / -0.38 |
| Back of the queue (cancels never move it up) | 79.0% (87746 of 111092 trades) | $42095.08 | $0.38 | 94.79 | -$64.00 | $206.23 | 64.6% | -0.33 / -0.32 |
| Pro-rata queue (some cancels are ahead of it) | 85.4% (125844 of 147420 trades) | $77014.32 | $0.52 | 180.04 | -$72.00 | $109.32 | 68.8% | -0.39 / -0.38 |
| Front of the queue (every cancel is ahead of it: optimistic) | 88.9% (164076 of 184620 trades) | $116487.64 | $0.63 | 293.59 | -$72.00 | $72.00 | 73.8% | -0.43 / -0.41 |
| Trade prints do not fill resting orders (as in standard competitions) | 38.7% (8980 of 23209 trades) | -$13079.00 | -$0.56 | -40.20 | -$72.00 | $13079.00 | 44.3% | 0.08 / 0.10 |
| Its own trades do not move prices | 83.4% (124556 of 149336 trades) | $74679.62 | $0.50 | 164.81 | -$102.00 | $112.67 | 69.7% | -0.34 / -0.35 |

## How it was run

- Final settings: max_volatility 0.5, time_stop_ms 30000, breakeven_ticks 2.
- Market: Gomer Trading's practice market, seed 1, 46800 ticks a session; 20 walk-forward sessions (train 4, test 2) then 6 holdout.
- Exchange: delays 1000 / 1000 / 1000 us (feed / order / cancel), queue hft, commission $0 a share, trade prints fill resting orders.
- 47 backtests in 2243 s. Nothing was written to disk but this report.
- The run-level kill switch was off so that every session counts; the session kill switch was on.
