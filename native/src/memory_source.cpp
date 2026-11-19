#include "memory_source.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace browsermem {

namespace {

constexpr size_t kChunk = size_t(2) << 20;  // 2 MiB per work item
constexpr size_t kPage = 4096;
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;

unsigned DefaultWorkers() {
  unsigned n = std::thread::hardware_concurrency();
  return std::clamp(n, 1u, 8u);
}

bool IsWantedRegion(const MEMORY_BASIC_INFORMATION& m) {
  return m.State == MEM_COMMIT && m.Type == MEM_PRIVATE &&
         !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (m.Protect & kWritable);
}

struct Chunk {
  uint64_t base;
  size_t size;   // bytes to read (owned + overlap, clipped to the region)
  size_t owned;  // bytes that belong to this chunk
};

std::vector<Chunk> SplitIntoChunks(const std::vector<RegionInfo>& regions, size_t overlap) {
  std::vector<Chunk> chunks;
  for (const RegionInfo& r : regions) {
    for (size_t off = 0; off < r.size; off += kChunk) {
      size_t owned = std::min(kChunk, r.size - off);
      size_t size = std::min(owned + overlap, r.size - off);
      chunks.push_back({r.base + off, size, owned});
    }
  }
  return chunks;
}

}  // namespace

void ParallelFor(size_t count, unsigned workers,
                 const std::function<void(unsigned worker, size_t index)>& fn) {
  workers = std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(count)));
  std::atomic<size_t> next{0};
  auto run = [&](unsigned worker) {
    for (size_t i; (i = next.fetch_add(1)) < count;) fn(worker, i);
  };
  std::vector<std::thread> threads;
  threads.reserve(workers - 1);
  for (unsigned w = 1; w < workers; ++w) threads.emplace_back(run, w);
  run(0);
  for (auto& t : threads) t.join();
}

// ---- LiveProcessSource --------------------------------------------------------

LiveProcessSource::~LiveProcessSource() {
  if (process_) CloseHandle(process_);
}

bool LiveProcessSource::Open(DWORD pid, std::string& error) {
  if (process_) CloseHandle(process_);
  process_ = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!process_) {
    error = "OpenProcess(" + std::to_string(pid) + ") failed, Win32 error " +
            std::to_string(GetLastError());
    return false;
  }
  workers_ = DefaultWorkers();
  buffers_.assign(workers_, {});
  return true;
}

std::vector<RegionInfo> LiveProcessSource::EnumerateRegions() const {
  std::vector<RegionInfo> out;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  auto* cursor = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
  auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
  MEMORY_BASIC_INFORMATION m;
  while (cursor < end && VirtualQueryEx(process_, cursor, &m, sizeof(m)) == sizeof(m)) {
    if (IsWantedRegion(m)) out.push_back({reinterpret_cast<uint64_t>(m.BaseAddress), m.RegionSize});
    auto* next = static_cast<uint8_t*>(m.BaseAddress) + m.RegionSize;
    if (next <= cursor) break;
    cursor = next;
  }
  return out;
}

std::optional<RegionInfo> LiveProcessSource::QueryRegion(uint64_t address) const {
  MEMORY_BASIC_INFORMATION m;
  if (VirtualQueryEx(process_, reinterpret_cast<void*>(address), &m, sizeof(m)) != sizeof(m) ||
      !IsWantedRegion(m))
    return std::nullopt;
  return RegionInfo{reinterpret_cast<uint64_t>(m.BaseAddress), m.RegionSize};
}

size_t LiveProcessSource::Read(uint64_t address, void* out, size_t length) const {
  SIZE_T got = 0;
  if (ReadProcessMemory(process_, reinterpret_cast<void*>(address), out, length, &got) &&
      got == length)
    return length;
  // Partial: read page by page up to the first unreadable page.
  size_t done = 0;
  while (done < length) {
    uint64_t at = address + done;
    size_t n = std::min(length - done, kPage - static_cast<size_t>(at % kPage));
    if (!ReadProcessMemory(process_, reinterpret_cast<void*>(at), static_cast<uint8_t*>(out) + done,
                           n, &got) ||
        got != n)
      break;
    done += n;
  }
  return done;
}

void LiveProcessSource::Scan(const std::vector<RegionInfo>& regions, size_t overlap,
                             const Visitor& visit) const {
  const std::vector<Chunk> chunks = SplitIntoChunks(regions, overlap);
  ParallelFor(chunks.size(), workers_, [&](unsigned worker, size_t i) {
    const Chunk& c = chunks[i];
    std::vector<uint8_t>& buf = buffers_[worker];
    if (buf.size() < c.size) buf.resize(std::max(c.size, kChunk + overlap));
    SIZE_T got = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<void*>(c.base), buf.data(), c.size, &got) ||
        got != c.size) {
      // Pages can be decommitted while we read: zero what we can't read.
      for (size_t off = 0; off < c.size; off += kPage) {
        size_t n = std::min(kPage, c.size - off);
        if (!ReadProcessMemory(process_, reinterpret_cast<void*>(c.base + off), buf.data() + off, n,
                               &got) ||
            got != n)
          std::memset(buf.data() + off, 0, n);
      }
    }
    visit(worker, c.base, buf.data(), c.size, c.owned);
  });
}

// ---- SnapshotSource -------------------------------------------------------------

SnapshotSource::SnapshotSource(const ProcessSnapshot& snapshot)
    : snapshot_(snapshot), workers_(DefaultWorkers()) {}

std::vector<RegionInfo> SnapshotSource::Regions() const {
  std::vector<RegionInfo> out;
  out.reserve(snapshot_.regions().size());
  for (const MemoryRegion& r : snapshot_.regions()) out.push_back({r.base, r.bytes.size()});
  return out;
}

size_t SnapshotSource::Read(uint64_t address, void* out, size_t length) const {
  const auto& regions = snapshot_.regions();
  auto it = std::upper_bound(regions.begin(), regions.end(), address,
                             [](uint64_t a, const MemoryRegion& r) { return a < r.base; });
  if (it == regions.begin()) return 0;
  --it;
  uint64_t end = it->base + it->bytes.size();
  if (address >= end) return 0;
  size_t n = static_cast<size_t>(std::min<uint64_t>(length, end - address));
  std::memcpy(out, it->bytes.data() + (address - it->base), n);
  return n;
}

void SnapshotSource::Scan(const std::vector<RegionInfo>& regions, size_t overlap,
                          const Visitor& visit) const {
  const std::vector<Chunk> chunks = SplitIntoChunks(regions, overlap);
  ParallelFor(chunks.size(), workers_, [&](unsigned worker, size_t i) {
    const Chunk& c = chunks[i];
    std::vector<uint8_t> tmp;
    const uint8_t* data = snapshot_.At(c.base, c.size);
    if (!data) {  // chunk not inside one region (unusual): copy what exists
      tmp.resize(c.size);
      size_t n = Read(c.base, tmp.data(), c.size);
      std::memset(tmp.data() + n, 0, c.size - n);
      data = tmp.data();
    }
    visit(worker, c.base, data, c.size, c.owned);
  });
}

}  // namespace browsermem
