#pragma once

// Uniform access to process memory for the resolver: a live process read on
// demand (fast path for real-time use) or an in-memory snapshot (forensics).

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "memory.hpp"

namespace browsermem {

struct RegionInfo {
  uint64_t base = 0;
  size_t size = 0;
};

class MemorySource {
 public:
  // fn(worker, chunkBase, data, size, owned): bytes [0, size) of a chunk.
  // Only matches that *start* below `owned` belong to this chunk; the rest
  // is overlap with the next chunk, so a match is never reported twice.
  using Visitor = std::function<void(unsigned worker, uint64_t base, const uint8_t* data,
                                     size_t size, size_t owned)>;

  virtual ~MemorySource() = default;

  // Copies as many bytes as possible from [address, address + length) and
  // returns how many were read, counting from `address`.
  virtual size_t Read(uint64_t address, void* out, size_t length) const = 0;

  // Visits every chunk of `regions`, in parallel. Reported matches must not
  // be longer than `overlap + 1` bytes.
  virtual void Scan(const std::vector<RegionInfo>& regions, size_t overlap,
                    const Visitor& visit) const = 0;

  virtual unsigned workers() const = 0;
};

// Reads a running process with ReadProcessMemory, streaming fixed-size chunks
// through reusable per-thread buffers (no large allocations).
class LiveProcessSource final : public MemorySource {
 public:
  LiveProcessSource() = default;
  ~LiveProcessSource() override;
  LiveProcessSource(const LiveProcessSource&) = delete;
  LiveProcessSource& operator=(const LiveProcessSource&) = delete;

  bool Open(DWORD pid, std::string& error);
  bool IsOpen() const { return process_ != nullptr; }

  // Committed, private, writable regions: where the browser keeps its objects.
  std::vector<RegionInfo> EnumerateRegions() const;
  // The current such region containing `address`, if any.
  std::optional<RegionInfo> QueryRegion(uint64_t address) const;

  size_t Read(uint64_t address, void* out, size_t length) const override;
  void Scan(const std::vector<RegionInfo>& regions, size_t overlap,
            const Visitor& visit) const override;
  unsigned workers() const override { return workers_; }

 private:
  HANDLE process_ = nullptr;
  unsigned workers_ = 1;
  mutable std::vector<std::vector<uint8_t>> buffers_;  // one per worker
};

// An in-memory snapshot (live capture, minidump, custom regions).
class SnapshotSource final : public MemorySource {
 public:
  explicit SnapshotSource(const ProcessSnapshot& snapshot);

  std::vector<RegionInfo> Regions() const;

  size_t Read(uint64_t address, void* out, size_t length) const override;
  void Scan(const std::vector<RegionInfo>& regions, size_t overlap,
            const Visitor& visit) const override;
  unsigned workers() const override { return workers_; }

 private:
  const ProcessSnapshot& snapshot_;
  unsigned workers_ = 1;
};

// Runs fn(worker, index) for index in [0, count) on up to `workers` threads.
void ParallelFor(size_t count, unsigned workers,
                 const std::function<void(unsigned worker, size_t index)>& fn);

}  // namespace browsermem
