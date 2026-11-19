#pragma once

#include <string>
#include <vector>

#include "browsers.hpp"
#include "memory.hpp"
#include "url_index.hpp"

namespace browsermem {

struct UrlCandidate {
  std::string url;
  double score = 0;
};

struct Resolution {
  std::string url;                      // empty when unresolved
  std::wstring title;                   // title variant that matched memory
  double confidence = 0;                // 0..1
  std::vector<UrlCandidate> candidates; // best first
  uint32_t elapsedMs = 0;
  size_t bytesScanned = 0;
  size_t uniqueUrls = 0;

  bool resolved() const { return !url.empty(); }
};

// Core algorithm, independent of live processes (works on any snapshot,
// e.g. one rebuilt from a memory image):
//   1. find the tab title in memory,
//   2. find every pointer to those title buffers -- they sit inside the tab's
//      C++ objects (TabRendererData, NavigationEntryImpl, ...),
//   3. vote for URL strings referenced (or stored inline) near those pointers.
Resolution CorrelateTitle(const ProcessSnapshot& snapshot, const UrlIndex& urls,
                          const std::vector<std::wstring>& titleCandidates,
                          size_t inlineTitleBytes = 22);

// Snapshot the window's process and resolve its active tab.
bool ResolveActiveTab(const BrowserWindow& window, Resolution& out, std::string& error);

}  // namespace browsermem
