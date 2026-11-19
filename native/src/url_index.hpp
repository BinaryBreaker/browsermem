#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "memory.hpp"

namespace browsermem {

// Every URL string carved out of a snapshot, with the address of each copy.
// Handles UTF-8 (std::string, GURL::spec_, nsCString) and UTF-16LE
// (std::u16string, nsString).
class UrlIndex {
 public:
  void Build(const ProcessSnapshot& snapshot);

  size_t size() const { return urls_.size(); }
  const std::string& url(uint32_t id) const { return urls_[id]; }
  uint32_t count(uint32_t id) const { return counts_[id]; }

  // Id of the URL string that starts exactly at `address`.
  std::optional<uint32_t> AtAddress(uint64_t address) const {
    auto it = byAddress_.find(address);
    if (it == byAddress_.end()) return std::nullopt;
    return it->second;
  }

  // Calls f(address, id) for every URL copy starting in [lo, hi].
  template <class F>
  void ForEachInRange(uint64_t lo, uint64_t hi, F&& f) const {
    auto it = std::lower_bound(sorted_.begin(), sorted_.end(), std::make_pair(lo, 0u));
    for (; it != sorted_.end() && it->first <= hi; ++it) f(it->first, it->second);
  }

 private:
  void Add(std::string url, uint64_t address);

  std::vector<std::string> urls_;
  std::vector<uint32_t> counts_;
  std::unordered_map<std::string, uint32_t> byUrl_;
  std::unordered_map<uint64_t, uint32_t> byAddress_;
  std::vector<std::pair<uint64_t, uint32_t>> sorted_;
};

}  // namespace browsermem
