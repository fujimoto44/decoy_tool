#pragma once
// Source: monero-project/monero src/wallet/wallet2.h, line 85-108 (class gamma_picker)
// BSD-3-Clause, Copyright (c) 2014-2024, The Monero Project
// Verbatim copy, no logic changed. The only difference is that
// crypto::rand<T> comes from our own rng.h (which in turn calls the
// original, unmodified random.c/keccak.c).
#include <vector>
#include <cstdint>
#include <limits>
#include <random>
#include "rng.h"

class gamma_picker
{
public:
  uint64_t pick();
  gamma_picker(const std::vector<uint64_t> &rct_offsets);
  gamma_picker(const std::vector<uint64_t> &rct_offsets, double shape, double scale);
  uint64_t get_num_rct_outs() const { return num_rct_outputs; }

private:
  struct gamma_engine
  {
    typedef uint64_t result_type;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }
    result_type operator()() { return crypto::rand<result_type>(); }
  } engine;

private:
  std::gamma_distribution<double> gamma;
  const std::vector<uint64_t> &rct_offsets;
  const uint64_t *begin, *end;
  uint64_t num_rct_outputs;
  double average_output_time;
};