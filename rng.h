#pragma once
// A direct port of the rand<T>, rand_range, rand_idx functions from
// src/crypto/crypto.h and crypto.cpp. The only difference: std::mutex
// instead of boost::mutex (just the thread-safety mechanism, unrelated
// to the RNG algorithm or the resulting distribution's math). The
// actual entropy source underneath is still the original
// src/crypto/random.c + keccak.c (unmodified).
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <random>
#include <mutex>

extern "C" void generate_random_bytes_not_thread_safe(size_t n, void *result);

namespace crypto
{
  inline std::mutex& get_random_lock()
  {
    static std::mutex m;
    return m;
  }

  inline void rand(size_t N, uint8_t *bytes)
  {
    std::lock_guard<std::mutex> lock(get_random_lock());
    generate_random_bytes_not_thread_safe(N, bytes);
  }

  template<typename T>
  T rand()
  {
    static_assert(std::is_standard_layout<T>::value, "cannot write random bytes into non-standard layout type");
    static_assert(std::is_trivially_copyable<T>::value, "cannot write random bytes into non-trivially copyable type");
    typename std::remove_cv<T>::type res;
    rand(sizeof(T), (uint8_t*)&res);
    return res;
  }

  struct random_device
  {
    typedef uint64_t result_type;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return result_type(-1); }
    result_type operator()() const { return crypto::rand<result_type>(); }
  };

  template<typename T>
  typename std::enable_if<std::is_integral<T>::value, T>::type rand_range(T range_min, T range_max)
  {
    crypto::random_device rd;
    std::uniform_int_distribution<T> dis(range_min, range_max);
    return dis(rd);
  }

  template<typename T>
  typename std::enable_if<std::is_unsigned<T>::value, T>::type rand_idx(T sz)
  {
    return crypto::rand_range<T>(0, sz - 1);
  }
}