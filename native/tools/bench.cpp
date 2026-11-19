// Developer benchmark: how long does resolving the active tab take, and where
// does the time go?
//
//   cmake -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBROWSERMEM_BUILD_TOOLS=ON
//   cmake --build build/bench && build\bench\browsermem_bench.exe [runs]
//
// Uses the most recently active browser window.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "browsers.hpp"
#include "memory.hpp"
#include "memory_source.hpp"
#include "resolver.hpp"

using namespace browsermem;

static void Print(const char* label, const Resolution& r) {
  const ResolveStats& s = r.stats;
  printf("  %-10s %4u ms | enum %5.1f  title %5.1f  ptr %5.1f  vote %5.1f | %6.1f MB  conf %.2f  %s\n",
         label, r.elapsedMs, s.enumerateMs, s.titleMs, s.pointerMs, s.voteMs,
         s.bytesScanned / 1048576.0, r.confidence, r.url.empty() ? "<unresolved>" : r.url.c_str());
}

// Simulates tab switching: resolve several known tab titles in turn.
//   browsermem_bench --scenario <pid> <title> [<title> ...]
static int Scenario(int argc, char** argv) {
  const DWORD pid = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10));
  LiveResolver resolver(pid);
  for (int round = 0; round < 3; ++round) {
    printf("round %d\n", round);
    for (int i = 3; i < argc; ++i) {
      Resolution r;
      std::string err;
      if (!resolver.Resolve({ToWide(argv[i])}, 22, r, err)) {
        printf("%s\n", err.c_str());
        return 1;
      }
      Print(r.stats.fastPath ? "fast" : "full", r);
      if (!r.stats.fastPathMiss.empty()) printf("             (fast path: %s)\n", r.stats.fastPathMiss.c_str());
    }
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 3 && std::string(argv[1]) == "--scenario") return Scenario(argc, argv);
  const int runs = argc > 1 ? std::atoi(argv[1]) : 5;
  auto windows = EnumerateBrowserWindows();
  if (windows.empty()) {
    printf("no browser window found\n");
    return 1;
  }
  const BrowserWindow& w = windows.front();
  const auto titles = TitleCandidates(w);
  const size_t inlineBytes = w.profile ? w.profile->inlineTitleBytes : 22;
  printf("%s pid %lu: %s\n\n", BrowserName(w.browser()), w.pid, ToUtf8(w.caption).c_str());

  // Previous engine for comparison: copy all memory, then analyse the copy.
  printf("copy-all (v0.1.0 style):\n");
  for (int i = 0; i < runs; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    ProcessSnapshot snap;
    std::string err;
    if (!snap.Capture(w.pid, err)) { printf("%s\n", err.c_str()); return 1; }
    SnapshotSource src(snap);
    auto regions = src.Regions();
    Resolution r = CorrelateTitle(src, regions, titles, inlineBytes, nullptr, &regions);
    r.elapsedMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
    Print("copy-all", r);
  }

  LiveResolver resolver(w.pid);
  printf("\nstreaming full scan:\n");
  for (int i = 0; i < runs; ++i) {
    Resolution r;
    std::string err;
    if (!resolver.Resolve(titles, inlineBytes, r, err, /*allowFastPath=*/false)) {
      printf("%s\n", err.c_str());
      return 1;
    }
    Print("full", r);
  }

  printf("\nhot-region fast path:\n");
  for (int i = 0; i < runs; ++i) {
    Resolution r;
    std::string err;
    resolver.Resolve(titles, inlineBytes, r, err, /*allowFastPath=*/true);
    Print(r.stats.fastPath ? "fast" : "fallback", r);
  }
  return 0;
}
