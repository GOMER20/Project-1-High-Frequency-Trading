# HFT Strategy: validation

**Success rate out of sample: 30.1% (589 of 1958 trades)** (the true rate is likely 28-32%)  
**Holdout: 28.6% (255 of 893 trades)**

**Verdict: No edge. The strategy loses money on sessions it was not tuned on.**

- It lost $8066.00 on the walk-forward test sessions, which it was not tuned on.
- It lost $3043.00 on the holdout sessions.
- Its signal did worse than a coin toss: profit per trade -$4.12 against the random-entry baseline's -$3.52 out of sample (t = -3.47).
- None of the signals predicts the price: the strongest correlation with the next second's move is 0.013, where a usable signal would show a few hundredths or more.
- The session kill switch stopped trading in 26 of 26 sessions, so the net profit figures mostly measure that limit; profit per trade is the fairer comparison.

A success rate alone says little: a strategy can win most trades and still lose money if the losers are bigger. Read it with profit per trade and its t (about 2 or more before it means anything).

## Out of sample

| Sessions | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| Walk-forward tests (16) | 30.1% (589 of 1958 trades) | -$8066.00 | -$4.12 | -30.77 | -$42.00 | $8066.00 | 33.0% | 2.40 / 3.42 |
| Holdout (6) | 28.6% (255 of 893 trades) | -$3043.00 | -$3.41 | -20.78 | -$34.00 | $3043.00 | 32.1% | 2.07 / 2.55 |
| Coin-toss baseline, walk-forward tests | 28.9% (664 of 2297 trades) | -$8090.00 | -$3.52 | -32.61 | -$45.00 | $8091.00 | 34.8% | 2.19 / 3.17 |
| Coin-toss baseline, holdout | 26.2% (221 of 844 trades) | -$3027.00 | -$3.59 | -19.79 | -$35.00 | $3028.00 | 35.2% | 2.21 / 3.10 |

## Overfitting

| Chosen settings | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| In sample (where they were chosen) | 32.6% (1486 of 4556 trades) | -$16057.00 | -$3.52 | -44.63 | -$49.00 | $16057.00 | 32.7% | 2.04 / 2.70 |
| Out of sample (the sessions after) | 30.1% (589 of 1958 trades) | -$8066.00 | -$4.12 | -30.77 | -$42.00 | $8066.00 | 33.0% | 2.40 / 3.42 |

- Candidates tried: 12.
- Rank correlation, training to test: 0.12 on average (1: the best in training stay best; 0: training says nothing about the test).
- Training winner below the test median: 25% of steps (about 50% means the choice is luck).

### Walk-forward steps

| Train | Test | Chosen | Train net | Test: success rate | Test net | Rank corr. |
|---|---|---|---|---|---|---|
| 1-4 | 5-6 | enter_score 0.5, w_trend 0, time_stop_ms 1000 | -$2005.00 | 27.1% (59 of 218 trades) | -$1009.00 | 0.31 |
| 3-6 | 7-8 | enter_score 0.5, w_trend 0, time_stop_ms 1000 | -$2010.00 | 24.8% (57 of 230 trades) | -$1008.00 | 0.20 |
| 5-8 | 9-10 | enter_score 0.3, w_trend 2, time_stop_ms 3000 | -$2009.00 | 47.8% (77 of 161 trades) | -$1020.00 | -0.15 |
| 7-10 | 11-12 | enter_score 0.5, w_trend 0, time_stop_ms 1000 | -$2013.00 | 23.8% (44 of 185 trades) | -$1003.00 | -0.03 |
| 9-12 | 13-14 | enter_score 0.5, w_trend 0, time_stop_ms 1000 | -$2008.00 | 28.4% (115 of 405 trades) | -$1003.00 | 0.74 |
| 11-14 | 15-16 | enter_score 0.3, w_trend 0, time_stop_ms 3000 | -$2004.00 | 37.1% (75 of 202 trades) | -$1007.00 | -0.59 |
| 13-16 | 17-18 | enter_score 0.8, w_trend 0, time_stop_ms 1000 | -$2007.00 | 21.9% (56 of 256 trades) | -$1006.00 | 0.20 |
| 15-18 | 19-20 | enter_score 0.5, w_trend 2, time_stop_ms 1000 | -$2001.00 | 35.2% (106 of 301 trades) | -$1010.00 | 0.29 |

## Does the book predict the price?

Correlation of each signal with the move of the mid that follows (from the final settings' run):

| Signal | Next 1 s | Next 5 s | Cents per unit, 1 s |
|---|---|---|---|
| order-book imbalance | 0.000 | 0.000 | 0.035 |
| micro-price | -0.000 | -0.001 | -0.005 |
| trade flow | 0.000 | 0.000 | 0.006 |
| trend | 0.013 | 0.029 | 0.100 |
| combined score | -0.000 | -0.000 | -0.004 |

Over 12167470 quotes. A useful signal for this kind of trading would show a correlation of a few hundredths or more.

## Sensitivity

The final settings with one setting moved, over all 26 sessions (these include the ones they were tuned on, so compare each row with the first, not with zero).

| Change | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| None (final settings) | 27.4% (989 of 3606 trades) | -$13100.00 | -$3.63 | -42.51 | -$42.00 | $13100.00 | 31.2% | 2.21 / 3.06 |
| enter_score 0.5 -> 0.25 | 28.5% (1083 of 3795 trades) | -$13120.00 | -$3.46 | -41.95 | -$52.00 | $13120.00 | 32.9% | 2.11 / 2.83 |
| enter_score 0.5 -> 0.75 | 27.2% (948 of 3487 trades) | -$13081.00 | -$3.75 | -42.33 | -$41.00 | $13083.00 | 29.3% | 2.07 / 2.78 |
| cancel_score 0.1 -> 0.05 | 27.4% (987 of 3608 trades) | -$13113.00 | -$3.63 | -42.55 | -$42.00 | $13113.00 | 31.3% | 2.20 / 3.05 |
| cancel_score 0.1 -> 0.15 | 27.3% (985 of 3603 trades) | -$13125.00 | -$3.64 | -42.56 | -$42.00 | $13125.00 | 31.1% | 2.21 / 3.07 |
| max_toxicity 0.6 -> 0.3 | 28.0% (1039 of 3711 trades) | -$13088.00 | -$3.53 | -42.30 | -$42.00 | $13088.00 | 31.0% | 2.11 / 2.77 |
| max_toxicity 0.6 -> 0.9 | 27.4% (987 of 3606 trades) | -$13114.00 | -$3.64 | -42.59 | -$42.00 | $13114.00 | 31.2% | 2.23 / 3.07 |
| scratch_score 0.5 -> 0.25 | 23.3% (827 of 3549 trades) | -$13083.00 | -$3.69 | -45.45 | -$39.00 | $13083.00 | 30.2% | 2.31 / 3.10 |
| scratch_score 0.5 -> 0.75 | 30.8% (1149 of 3735 trades) | -$13449.00 | -$3.60 | -29.04 | -$332.00 | $13449.00 | 32.2% | 2.11 / 2.87 |
| w_obi 1 -> 0.5 | 27.8% (1010 of 3629 trades) | -$13132.00 | -$3.62 | -42.10 | -$41.00 | $13133.00 | 30.7% | 2.16 / 2.76 |
| w_obi 1 -> 1.5 | 27.5% (1007 of 3662 trades) | -$13093.00 | -$3.58 | -43.48 | -$39.00 | $13093.00 | 31.9% | 2.16 / 3.12 |
| w_micro 1 -> 0.5 | 32.9% (1220 of 3706 trades) | -$13116.00 | -$3.54 | -39.49 | -$42.00 | $13124.00 | 31.4% | 1.85 / 2.71 |
| w_micro 1 -> 1.5 | 24.9% (895 of 3592 trades) | -$13108.00 | -$3.65 | -44.29 | -$39.00 | $13108.00 | 31.6% | 2.22 / 3.31 |
| w_flow 0.5 -> 0.25 | 27.6% (1013 of 3676 trades) | -$13448.00 | -$3.66 | -29.84 | -$332.00 | $13448.00 | 31.2% | 2.20 / 2.88 |
| w_flow 0.5 -> 0.75 | 28.6% (1055 of 3694 trades) | -$13134.00 | -$3.56 | -42.21 | -$42.00 | $13134.00 | 32.0% | 2.13 / 2.95 |
| target_ticks 1 -> 2 | 25.1% (958 of 3816 trades) | -$13124.00 | -$3.44 | -40.45 | -$41.00 | $13126.00 | 29.3% | 2.11 / 2.95 |
| time_stop_ms 1000 -> 500 | 18.3% (696 of 3805 trades) | -$13142.00 | -$3.45 | -49.03 | -$33.00 | $13142.00 | 29.2% | 2.25 / 3.35 |
| time_stop_ms 1000 -> 1500 | 31.9% (1115 of 3496 trades) | -$13098.00 | -$3.75 | -39.56 | -$42.00 | $13098.00 | 32.1% | 2.21 / 3.08 |
| entry_timeout_ms 2000 -> 1000 | 27.4% (989 of 3616 trades) | -$13117.00 | -$3.63 | -42.58 | -$42.00 | $13117.00 | 31.1% | 2.20 / 3.08 |
| entry_timeout_ms 2000 -> 3000 | 27.4% (989 of 3606 trades) | -$13100.00 | -$3.63 | -42.51 | -$42.00 | $13100.00 | 31.2% | 2.21 / 3.06 |

## Stress

Changed nothing at all: Delays x5 (5 ms orders); Delays x50 (50 ms orders); Back of the queue (cancels never move it up). In a market that quotes twice a second and fills resting orders only when the price moves through them, the whole level is cleared at once, so neither the place in the queue nor a few milliseconds of delay can make a difference.

| Change | Success rate | Net profit | Per trade | t | Worst trade | Max drawdown | Fill rate | Adverse 1 s / 5 s (cents) |
|---|---|---|---|---|---|---|---|---|
| None (final settings) | 27.4% (989 of 3606 trades) | -$13100.00 | -$3.63 | -42.51 | -$42.00 | $13100.00 | 31.2% | 2.21 / 3.06 |
| Delays x5 (5 ms orders) | 27.4% (989 of 3606 trades) | -$13100.00 | -$3.63 | -42.51 | -$42.00 | $13100.00 | 31.2% | 2.21 / 3.06 |
| Delays x50 (50 ms orders) | 27.4% (989 of 3606 trades) | -$13100.00 | -$3.63 | -42.51 | -$42.00 | $13100.00 | 31.2% | 2.21 / 3.06 |
| Delays x500 (500 ms orders) | 55.6% (5712 of 10272 trades) | -$13208.00 | -$1.29 | -13.40 | -$462.00 | $13210.00 | 16.2% | 1.51 / 2.72 |
| Commission $0.001 a share | 27.2% (909 of 3343 trades) | -$13098.60 | -$3.92 | -43.50 | -$42.20 | $13098.60 | 31.2% | 2.26 / 3.15 |
| Commission $0.0035 a share | 27.0% (780 of 2889 trades) | -$13114.30 | -$4.54 | -46.04 | -$39.70 | $13114.30 | 31.4% | 2.36 / 3.21 |
| Commission $0.005 a share | 0.0% (0 of 2671 trades) | -$13122.00 | -$4.91 | -47.28 | -$40.00 | $13122.00 | 31.5% | 2.41 / 3.30 |
| Back of the queue (cancels never move it up) | 27.4% (989 of 3606 trades) | -$13100.00 | -$3.63 | -42.51 | -$42.00 | $13100.00 | 31.2% | 2.21 / 3.06 |
| Trade prints fill resting orders (not how Gomer Trading works) | 36.4% (1558 of 4279 trades) | -$13110.37 | -$3.06 | -39.84 | -$41.00 | $13110.37 | 37.0% | 1.77 / 2.60 |
| Its own trades do not move prices | 18.3% (501 of 2738 trades) | -$13114.00 | -$4.79 | -44.27 | -$44.00 | $13114.00 | 29.1% | 3.34 / 3.70 |

## How it was run

- Final settings: enter_score 0.5, w_trend 0, time_stop_ms 1000.
- Market: Gomer Trading's practice market, seed 1, 46800 ticks a session; 20 walk-forward sessions (train 4, test 2) then 6 holdout.
- Exchange: delays 1000 / 1000 / 1000 us (feed / order / cancel), queue prorata, commission $0 a share, trade prints do not fill resting orders.
- 41 backtests in 773 s. Nothing was written to disk but this report.
- The run-level kill switch was off so that every session counts; the session kill switch was on.
