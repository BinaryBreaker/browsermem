#include "url_index.hpp"

#include <algorithm>
#include <cstring>

#include "url_syntax.hpp"

namespace browsermem {

void UrlIndex::Add(std::string url, uint64_t address) {
  auto [it, inserted] = byUrl_.emplace(std::move(url), (uint32_t)urls_.size());
  if (inserted) {
    urls_.push_back(it->first);
    counts_.push_back(0);
  }
  counts_[it->second]++;
  byAddress_[address] = it->second;
  sorted_.emplace_back(address, it->second);
}

void UrlIndex::Build(const ProcessSnapshot& snapshot) {
  for (const MemoryRegion& region : snapshot.regions()) {
    CarveUrls(region.bytes.data(), region.bytes.size(), region.base, region.bytes.size(),
              [this](std::string url, uint64_t address) { Add(std::move(url), address); });
  }
  std::sort(sorted_.begin(), sorted_.end());
}

}  // namespace browsermem
