#pragma once

#include <vector>

#include "common.hpp"

namespace browsermem {

struct MemoryRegion {
  uint64_t base = 0;
  std::vector<uint8_t> bytes;
};

// Point-in-time copy of a process's private read/write memory (heaps,
// PartitionAlloc / jemalloc pages). This is where browsers keep their C++
// objects; images and mapped files are skipped.
class ProcessSnapshot {
 public:
  // Returns false and fills `error` on failure.
  bool Capture(DWORD pid, std::string& error);

  // Build a snapshot from externally supplied memory (minidump, raw image).
  // Call Finalize() before querying.
  void AddRegion(uint64_t base, const uint8_t* data, size_t size);
  void Finalize();

  // Bytes [address, address + length) if they lie inside one region, else null.
  const uint8_t* At(uint64_t address, size_t length) const;

  const std::vector<MemoryRegion>& regions() const { return regions_; }
  size_t totalBytes() const { return totalBytes_; }

 private:
  std::vector<MemoryRegion> regions_;  // sorted by base
  size_t totalBytes_ = 0;
};

}  // namespace browsermem
