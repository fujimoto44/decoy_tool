// Source: monero-project/monero src/wallet/wallet2.cpp, line 1042-1105
// (gamma_picker::gamma_picker and gamma_picker::pick())
// BSD-3-Clause, Copyright (c) 2014-2024, The Monero Project
// Verbatim copy. Two mechanical changes only, neither affects the
// algorithm:
//   1) THROW_WALLET_EXCEPTION_IF(...) -> a plain throw (same message)
//   2) MTRACE(...) -> std::cerr (debug logging only, not part of the
//      decision logic)

#include <stdexcept>
#include <iostream>
#include <cmath>
#include <algorithm>
#include "gamma_picker.h"

#define DIFFICULTY_TARGET_V2 120  // seconds - src/cryptonote_config.h line 79
#define CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE 10  // src/cryptonote_config.h line 48
#define DEFAULT_UNLOCK_TIME (CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE * DIFFICULTY_TARGET_V2)
#define GAMMA_SHAPE 19.28
#define GAMMA_SCALE (1/1.61)
#define RECENT_SPEND_WINDOW (15 * DIFFICULTY_TARGET_V2)

#define THROW_WALLET_EXCEPTION_IF(cond, ex, msg) \
  do { if (cond) throw std::runtime_error(msg); } while(0)

gamma_picker::gamma_picker(const std::vector<uint64_t> &rct_offsets, double shape, double scale):
    rct_offsets(rct_offsets)
{
  gamma = std::gamma_distribution<double>(shape, scale);
  THROW_WALLET_EXCEPTION_IF(rct_offsets.size() < std::max<size_t>(1, CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE), 0, "Bad offset calculation");
  const size_t blocks_in_a_year = 86400 * 365 / DIFFICULTY_TARGET_V2;
  const size_t blocks_to_consider = std::min<size_t>(rct_offsets.size(), blocks_in_a_year);
  const size_t outputs_to_consider = rct_offsets.back() - (blocks_to_consider < rct_offsets.size() ? rct_offsets[rct_offsets.size() - blocks_to_consider - 1] : 0);
  begin = rct_offsets.data();
  end = rct_offsets.data() + rct_offsets.size() - (std::max(1, CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE) - 1);
  num_rct_outputs = *(end - 1);
  THROW_WALLET_EXCEPTION_IF(num_rct_outputs == 0, 0, "No rct outputs");
  THROW_WALLET_EXCEPTION_IF(outputs_to_consider == 0, 0, "No outputs in consideration window"); 
  average_output_time = DIFFICULTY_TARGET_V2 * blocks_to_consider / static_cast<double>(outputs_to_consider); // this assumes constant target over the whole rct range
};

gamma_picker::gamma_picker(const std::vector<uint64_t> &rct_offsets): gamma_picker(rct_offsets, GAMMA_SHAPE, GAMMA_SCALE) {}

uint64_t gamma_picker::pick()
{
  double x = gamma(engine);
  x = exp(x);

  if (x > DEFAULT_UNLOCK_TIME)
  {
    // We are trying to select an output from the chain that appeared 'x' seconds before the
    // current chain tip, where 'x' is selected from the gamma distribution recommended in Miller et al.
    // (https://arxiv.org/pdf/1704.04299/).
    // Our method is to get the average time delta between outputs in the recent past, estimate the number of
    // outputs 'n' that would have appeared between 'chain_tip - x' and 'chain_tip', select the real output at
    // 'current_num_outputs - n', then randomly select an output from the block where that output appears.
    // Source code to paper: https://github.com/maltemoeser/moneropaper
    //
    // Due to the 'default spendable age' mechanic in Monero, 'current_num_outputs' only contains
    // currently *unlocked* outputs, which means the earliest output that can be selected is not at the chain tip!
    // Therefore, we must offset 'x' so it matches up with the timing of the outputs being considered. We do
    // this by saying if 'x` equals the expected age of the first unlocked output (compared to the current
    // chain tip - i.e. DEFAULT_UNLOCK_TIME), then select the first unlocked output.
    x -= DEFAULT_UNLOCK_TIME;
  }
  else
  {
    // If the spent time suggested by the gamma is less than the unlock time, that means the gamma is suggesting an output
    // that is no longer feasible to be spent (possible since the gamma was constructed when consensus rules did not enforce the
    // lock time). The assumption made in this code is that an output expected spent quicker than the unlock time would likely
    // be spent within RECENT_SPEND_WINDOW after allowed. So it returns an output that falls between 0 and the RECENT_SPEND_WINDOW.
    // The RECENT_SPEND_WINDOW was determined with empirical analysis of observed data.
    x = crypto::rand_idx(static_cast<uint64_t>(RECENT_SPEND_WINDOW));
  }

  uint64_t output_index = x / average_output_time;
  if (output_index >= num_rct_outputs)
    return std::numeric_limits<uint64_t>::max(); // bad pick
  output_index = num_rct_outputs - 1 - output_index;

  const uint64_t *it = std::lower_bound(begin, end, output_index);
  THROW_WALLET_EXCEPTION_IF(it == end, 0, "output_index not found");
  uint64_t index = std::distance(begin, it);

  const uint64_t first_rct = index == 0 ? 0 : rct_offsets[index - 1];
  const uint64_t n_rct = rct_offsets[index] - first_rct;
  if (n_rct == 0)
    return std::numeric_limits<uint64_t>::max(); // bad pick
  return first_rct + crypto::rand_idx(n_rct);
};