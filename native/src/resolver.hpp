#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "browsers.hpp"
#include "memory_source.hpp"

namespace browsermem {

struct UrlCandidate {
  std::string url;
  double score = 0;
};

// Where the time went (milliseconds), for diagnostics and benchmarks.
struct ResolveStats {
  double enumerateMs = 0;  // listing memory regions
  double titleMs = 0;      // finding the title text
  double pointerMs = 0;    // finding pointers to the title
  double voteMs = 0;       // reading neighbouring fields, scoring URLs
  uint64_t bytesScanned = 0;
  bool fastPath = false;   // resolved from the hot-region cache
  std::string fastPathMiss;  // why a fast-path attempt was rejected (diagnostics)
};

struct Resolution {
  std::string url;                      // empty when unresolved
  std::wstring title;                   // title variant that matched memory
  double confidence = 0;                // 0..1
  std::vector<UrlCandidate> candidates; // best first
  uint32_t elapsedMs = 0;
  ResolveStats stats;

  bool resolved() const { return !url.empty(); }
};

// Addresses that contributed to a resolution; used to learn hot regions.
struct Evidence {
  std::vector<uint64_t> addresses;
};

// Core algorithm, independent of where memory comes from:
//   1. find the tab title in memory,
//   2. find every pointer to those title buffers -- they sit inside the tab's
//      C++ objects (TabRendererData, NavigationEntryImpl, ...),
//   3. score URL strings referenced (or stored inline) near those pointers.
// `knownRegions` (optional) lets step 3 skip pointers to unmapped memory.
Resolution CorrelateTitle(const MemorySource& source, const std::vector<RegionInfo>& regions,
                          const std::vector<std::wstring>& titleCandidates,
                          size_t inlineTitleBytes = 22, Evidence* evidence = nullptr,
                          const std::vector<RegionInfo>* knownRegions = nullptr);

// Resolves tabs of one live browser process. Remembers which memory regions
// held the tab objects ("hot regions") and searches those first; falls back
// to a full parallel scan when that doesn't give a confident answer.
class LiveResolver {
 public:
  explicit LiveResolver(DWORD pid) : pid_(pid) {}

  bool Resolve(const std::vector<std::wstring>& titles, size_t inlineTitleBytes,
               Resolution& out, std::string& error, bool allowFastPath = true);

  DWORD pid() const { return pid_; }
  uint64_t creationTime() const { return creationTime_; }
  void ForgetHotRegions();

 private:
  void Remember(const Evidence& evidence);

  const DWORD pid_;
  uint64_t creationTime_ = 0;
  std::mutex mu_;
  LiveProcessSource source_;
  std::vector<uint64_t> hot_;  // region base addresses, most recent first
};

// Shared resolver for a process (handles pid reuse).
std::shared_ptr<LiveResolver> ResolverFor(DWORD pid);

// Resolve the active tab of a browser window.
bool ResolveActiveTab(const BrowserWindow& window, Resolution& out, std::string& error);

}  // namespace browsermem
