#include "memory.hpp"

#include <algorithm>

namespace browsermem {

namespace {
constexpr size_t kMaxSnapshotBytes = size_t(4) << 30;  // safety cap: 4 GB
}

bool ProcessSnapshot::Capture(DWORD pid, std::string& error) {
  regions_.clear();
  totalBytes_ = 0;

  ScopedHandle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid));
  if (!process) {
    error = "OpenProcess(" + std::to_string(pid) + ") failed, Win32 error " +
            std::to_string(GetLastError());
    return false;
  }

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  auto* cursor = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
  auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
  const DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;

  MEMORY_BASIC_INFORMATION mbi;
  while (cursor < end &&
         VirtualQueryEx(process.get(), cursor, &mbi, sizeof(mbi)) == sizeof(mbi)) {
    auto* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    bool wanted = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                  !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & kWritable);

    if (wanted && totalBytes_ + mbi.RegionSize <= kMaxSnapshotBytes) {
      MemoryRegion region;
      region.base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
      region.bytes.resize(mbi.RegionSize);
      SIZE_T read = 0;
      if (!ReadProcessMemory(process.get(), mbi.BaseAddress, region.bytes.data(), mbi.RegionSize,
                             &read) ||
          read != mbi.RegionSize) {
        // Pages can be decommitted while we read: retry page by page, leaving
        // unreadable pages zeroed.
        for (SIZE_T off = 0; off < mbi.RegionSize; off += si.dwPageSize)
          ReadProcessMemory(process.get(), static_cast<uint8_t*>(mbi.BaseAddress) + off,
                            region.bytes.data() + off, si.dwPageSize, nullptr);
      }
      totalBytes_ += region.bytes.size();
      regions_.push_back(std::move(region));
    }
    if (next <= cursor) break;
    cursor = next;
  }

  if (regions_.empty()) {
    error = "no readable memory in process " + std::to_string(pid);
    return false;
  }
  return true;
}

void ProcessSnapshot::AddRegion(uint64_t base, const uint8_t* data, size_t size) {
  MemoryRegion region;
  region.base = base;
  region.bytes.assign(data, data + size);
  totalBytes_ += size;
  regions_.push_back(std::move(region));
}

void ProcessSnapshot::Finalize() {
  std::sort(regions_.begin(), regions_.end(),
            [](const MemoryRegion& a, const MemoryRegion& b) { return a.base < b.base; });
}

const uint8_t* ProcessSnapshot::At(uint64_t address, size_t length) const {
  auto it = std::upper_bound(regions_.begin(), regions_.end(), address,
                             [](uint64_t a, const MemoryRegion& r) { return a < r.base; });
  if (it == regions_.begin()) return nullptr;
  --it;
  if (address + length > it->base + it->bytes.size()) return nullptr;
  return it->bytes.data() + (address - it->base);
}

}  // namespace browsermem
