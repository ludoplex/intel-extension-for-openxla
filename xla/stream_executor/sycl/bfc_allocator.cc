/* Copyright (c) 2021-2022 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/stream_executor/sycl/bfc_allocator.h"

namespace itex {

namespace {
const int32_t XeHPC_id = 0xbd0;
const char* const XeHPC_name = "0x0bd";
const char* const XeHPC_name_new = "Data Center GPU Max";

bool IsXeHPC(sycl::device* device_ptr) {
  if (device_ptr == nullptr) {
    auto platform_list = sycl::platform::get_platforms();
    for (const auto& platform : platform_list) {
      auto device_list = platform.get_devices();
      for (const auto& device : device_list) {
        if (device.is_gpu()) {
#if defined(SYCL_EXT_INTEL_DEVICE_INFO) && (SYCL_EXT_INTEL_DEVICE_INFO >= 5)
          auto id =
              device.get_info<sycl::ext::intel::info::device::device_id>();
          if ((id & 0xff0) == XeHPC_id) {
            return true;
          }
#else
          std::string name = device.get_info<sycl::info::device::name>();
          if (name.find(XeHPC_name) != std::string::npos ||
              name.find(XeHPC_name_new) != std::string::npos) {
            return true;
          }
#endif
        }
      }
    }
  } else {
#if defined(SYCL_EXT_INTEL_DEVICE_INFO) && (SYCL_EXT_INTEL_DEVICE_INFO >= 5)
    auto id = device_ptr->get_info<sycl::ext::intel::info::device::device_id>();
    if ((id & 0xff0) == XeHPC_id) {
      return true;
    }
#else
    std::string name = device_ptr->get_info<sycl::info::device::name>();
    if (name.find(XeHPC_name) != std::string::npos ||
        name.find(XeHPC_name_new) != std::string::npos) {
      return true;
    }
#endif
  }
  return false;
}
}  // namespace

BFCAllocator::BFCAllocator(ITEX_GPUDevice* device) : Allocator() {
  device_ = device;
  memory_limit_ = device_->get_info<sycl::info::device::global_mem_size>();
  size_t _800mb = 800 * 1024 * 1024;
  // Leave 800MB memory for system like proper did.
  memory_limit_ -= _800mb;
  VLOG(1) << "Set memory limit to " << memory_limit_ << " Bytes";
  curr_region_allocation_bytes_ = RoundedBytes(memory_limit_);
  free_chunks_list_ = kInvalidChunkHandle;

  // Create a bunch of bins of various good sizes.

  // We create bins to fit all possible ranges that cover the
  // memory_limit_ starting from allocations up to 256 bytes to
  // allocations up to (and including) the memory limit.
  for (BinNum b = 0; b < kNumBins; b++) {
    size_t bin_size = BinNumToSize(b);
    VLOG(1) << "Creating bin of max chunk size " << bin_size;
    new (BinFromIndex(b)) Bin(this, bin_size);
    CHECK_EQ(BinForSize(bin_size), BinFromIndex(b));
    CHECK_EQ(BinForSize(bin_size + 255), BinFromIndex(b));
    CHECK_EQ(BinForSize(bin_size * 2 - 1), BinFromIndex(b));
    if (b + 1 < kNumBins) {
      CHECK_NE(BinForSize(bin_size * 2), BinFromIndex(b));
    }
  }
}

BFCAllocator::~BFCAllocator() {
  // Return memory back.
  VLOG(2) << "Number of regions allocated: "
          << region_manager_.regions().size();
  for (const auto& region : region_manager_.regions()) {
    if (region.ptr()) {
      ITEX_GPUFree(device_, region.ptr());
    }
  }

  for (BinNum b = 0; b < kNumBins; b++) {
    BinFromIndex(b)->~Bin();
  }
}

void* BFCAllocator::AllocateRaw(size_t num_bytes) {
  if (num_bytes == 0) {
    VLOG(1) << "tried to allocate 0 bytes";
    return nullptr;
  }

  // First, always allocate memory of at least kMinAllocationSize
  // bytes, and always allocate multiples of kMinAllocationSize bytes
  // so all memory addresses are nicely byte aligned.
  size_t rounded_bytes = RoundedBytes(num_bytes);

  // The BFC allocator tries to find the best fit first.
  BinNum bin_num = BinNumForSize(rounded_bytes);

  tsl::mutex_lock l(lock_);

  void* ptr = FindChunkPtr(bin_num, rounded_bytes, num_bytes);
  if (ptr != nullptr) {
    VLOG(2) << "Requested bytes: " << num_bytes
            << ", allocated_bytes: " << rounded_bytes
            << ", allocator_name: " << Name() << ", ptr: " << ptr;
    return ptr;
  }

  // No memory in current memory pool, try to extend from system.
  if (Extend(rounded_bytes)) {
    ptr = FindChunkPtr(bin_num, rounded_bytes, num_bytes);
    if (ptr != nullptr) {
      VLOG(2) << "Requested bytes: " << num_bytes
              << ", allocated_bytes: " << rounded_bytes
              << ", allocator_name: " << Name() << ", ptr: " << ptr;
      return ptr;
    }
  }

  LOG(ERROR) << "Allocator ran out of memory trying "
             << "to allocate " << num_bytes << " Bytes"
             << " (rounded to " << rounded_bytes << " Bytes)";

  return nullptr;
}

void BFCAllocator::DeallocateRaw(void* ptr) {
  if (ptr == nullptr) {
    VLOG(1) << "tried to deallocate nullptr";
    return;
  }
  VLOG(2) << "Deallocate " << ptr;
  tsl::mutex_lock l(lock_);

  // Find the chunk from the ptr.
  BFCAllocator::ChunkHandle h = region_manager_.get_handle(ptr);
  CHECK(h != kInvalidChunkHandle);
  Chunk* chunk = ChunkFromHandle(h);
  // Mark the chunk as no longer in use.
  chunk->allocation_id = -1;
  InsertFreeChunkIntoBin(TryToCoalesce(h));
}

// static
size_t BFCAllocator::RoundedBytes(size_t bytes) {
  size_t rounded_bytes =
      (kMinAllocationSize *
       ((bytes + kMinAllocationSize - 1) / kMinAllocationSize));
  DCHECK_EQ(size_t{0}, rounded_bytes % kMinAllocationSize);
  return rounded_bytes;
}

void* BFCAllocator::FindChunkPtr(BinNum bin_num, size_t rounded_bytes,
                                 size_t num_bytes) {
  // First identify the first bin that could satisfy rounded_bytes.
  for (; bin_num < kNumBins; bin_num++) {
    // Start searching from the first bin for the smallest chunk that fits
    // rounded_bytes.
    Bin* b = BinFromIndex(bin_num);
    for (auto citer = b->free_chunks.begin(); citer != b->free_chunks.end();
         ++citer) {
      const BFCAllocator::ChunkHandle h = (*citer);
      BFCAllocator::Chunk* chunk = ChunkFromHandle(h);
      DCHECK(!chunk->in_use());
      if (chunk->size >= rounded_bytes) {
        // We found an existing chunk that fits us that wasn't in use, so remove
        // it from the free bin structure prior to using.
        RemoveFreeChunkIterFromBin(&b->free_chunks, citer);
        // If we can break the size of the chunk into two reasonably large
        // pieces, do so.  In any case don't waste more than a threshold of
        // kMaxInternalFragmentation bytes on padding this alloc. Use 128MB
        // as the default threshold.
        const int64_t kMaxInternalFragmentation = 128 << 20;
        if (chunk->size >= rounded_bytes * 2 ||
            static_cast<int64_t>(chunk->size) - rounded_bytes >=
                kMaxInternalFragmentation) {
          SplitChunk(h, rounded_bytes);
          chunk = ChunkFromHandle(h);  // Update chunk pointer in case it moved
        }

        // The requested size of the returned chunk is what the user
        // has allocated.
        chunk->requested_size = num_bytes;
        // Currently do not track allocation id, use 0 mark this chunk in use.
        chunk->allocation_id = 0;
        return chunk->ptr;
      }
    }
  }
  return nullptr;
}

void BFCAllocator::RemoveFreeChunkIterFromBin(
    BFCAllocator::Bin::FreeChunkSet* free_chunks,
    const BFCAllocator::Bin::FreeChunkSet::iterator& citer) {
  ChunkHandle h = *citer;
  Chunk* c = ChunkFromHandle(h);
  CHECK(!c->in_use() && (c->bin_num != kInvalidBinNum));
  free_chunks->erase(citer);
  c->bin_num = kInvalidBinNum;
}

void BFCAllocator::SplitChunk(BFCAllocator::ChunkHandle h, size_t num_bytes) {
  // Allocate the new chunk before we do any ChunkFromHandle
  ChunkHandle h_new_chunk = AllocateChunk();

  Chunk* c = ChunkFromHandle(h);
  CHECK(!c->in_use() && (c->bin_num == kInvalidBinNum));

  // Create a new chunk starting num_bytes after c
  BFCAllocator::Chunk* new_chunk = ChunkFromHandle(h_new_chunk);
  new_chunk->ptr = static_cast<void*>(static_cast<char*>(c->ptr) + num_bytes);
  region_manager_.set_handle(new_chunk->ptr, h_new_chunk);

  // Set the new sizes of the chunks.
  new_chunk->size = c->size - num_bytes;
  c->size = num_bytes;

  // The new chunk is not in use.
  new_chunk->allocation_id = -1;

  // Maintain the pointers.
  // c <-> c_neighbor becomes
  // c <-> new_chunk <-> c_neighbor
  BFCAllocator::ChunkHandle h_neighbor = c->next;
  new_chunk->prev = h;
  new_chunk->next = h_neighbor;
  c->next = h_new_chunk;
  if (h_neighbor != kInvalidChunkHandle) {
    Chunk* c_neighbor = ChunkFromHandle(h_neighbor);
    c_neighbor->prev = h_new_chunk;
  }

  // Add the newly free chunk to the free bin.
  InsertFreeChunkIntoBin(h_new_chunk);
}

BFCAllocator::ChunkHandle BFCAllocator::AllocateChunk() {
  if (free_chunks_list_ != kInvalidChunkHandle) {
    ChunkHandle h = free_chunks_list_;
    Chunk* c = ChunkFromHandle(h);
    free_chunks_list_ = c->next;
    return h;
  } else {
    ChunkHandle h = chunks_.size();
    chunks_.resize(h + 1);
    return h;
  }
}

void BFCAllocator::InsertFreeChunkIntoBin(BFCAllocator::ChunkHandle h) {
  Chunk* c = ChunkFromHandle(h);
  CHECK(!c->in_use() && (c->bin_num == kInvalidBinNum));
  BinNum bin_num = BinNumForSize(c->size);
  Bin* new_bin = BinFromIndex(bin_num);
  c->bin_num = bin_num;
  new_bin->free_chunks.insert(h);
}

// This function set the upper bound of memory allocation size, the
// actuall allocation size is the minimal value of this limit size
// and the size want to get from system.
size_t BFCAllocator::GetLimitAlloc() {
  tsl::int64 limit_size = 4 * 1024;  // unit is MB
  if (IsXeHPC(device_)) {
    // Use a big value that means do not set limit.
    limit_size = 1024 * 1024;
  }
  tsl::ReadInt64FromEnvVar("ITEX_LIMIT_MEMORY_SIZE_IN_MB", limit_size,
                           &limit_size);
  return limit_size * 1024 * 1024;
}

size_t BFCAllocator::LimitAlloc() {
  static size_t limit_alloc = GetLimitAlloc();
  return limit_alloc;
}

bool BFCAllocator::Extend(size_t rounded_bytes) {
  size_t available_bytes = memory_limit_ - total_region_allocated_bytes_;
  // Rounds available_bytes down to the nearest multiple of kMinAllocationSize.
  available_bytes = (available_bytes / kMinAllocationSize) * kMinAllocationSize;

  // Do we have enough space to handle the client's request?
  // If not, fail immediately.
  if (rounded_bytes > available_bytes) {
    return false;
  }

  // If curr_region_allocation_bytes_ is not enough to satisfy the
  // allocation, keep multiplying by a power of two until that is
  // sufficient.
  bool increased_allocation = false;
  while (rounded_bytes > curr_region_allocation_bytes_) {
    curr_region_allocation_bytes_ *= 2;
    increased_allocation = true;
  }

  // Try allocating.
  size_t bytes = std::min(curr_region_allocation_bytes_, available_bytes);

  bytes = std::min(bytes, LimitAlloc());
  void* mem_addr = ITEX_GPUMalloc(device_, bytes);
  if (mem_addr == nullptr) {
    static constexpr float kBackpedalFactor = 0.9;

    // Try allocating less memory.
    while (mem_addr == nullptr) {
      bytes = RoundedBytes(bytes * kBackpedalFactor);
      if (bytes < rounded_bytes) break;
      mem_addr = ITEX_GPUMalloc(device_, bytes);
    }
  }

  if (mem_addr == nullptr) {
    return false;
  }

  if (!increased_allocation) {
    // Increase the region size of the next required allocation.
    curr_region_allocation_bytes_ *= 2;
  }

  VLOG(1) << "Extending allocation by " << bytes << " bytes.";

  total_region_allocated_bytes_ += bytes;
  VLOG(1) << "Total allocated bytes: " << total_region_allocated_bytes_;

  VLOG(1) << "Allocated memory at " << mem_addr << " to "
          << static_cast<void*>(static_cast<char*>(mem_addr) + bytes);

  region_manager_.AddAllocationRegion(mem_addr, bytes);

  // Create one large chunk for the whole memory space that will
  // be chunked later.
  ChunkHandle h = AllocateChunk();
  BFCAllocator::Chunk* c = ChunkFromHandle(h);
  c->ptr = mem_addr;
  c->size = bytes;
  c->allocation_id = -1;
  c->prev = kInvalidChunkHandle;
  c->next = kInvalidChunkHandle;

  region_manager_.set_handle(c->ptr, h);

  // Maybe merge adjacent chunks and insert the chunk into the right bin.
  InsertFreeChunkIntoBin(h);

  return true;
}

BFCAllocator::ChunkHandle BFCAllocator::TryToCoalesce(ChunkHandle h) {
  Chunk* c = ChunkFromHandle(h);
  ChunkHandle coalesced_chunk = h;

  // If the next chunk is free, merge it into c and delete it.
  if (c->next != kInvalidChunkHandle && !ChunkFromHandle(c->next)->in_use()) {
    Chunk* n = ChunkFromHandle(c->next);
    VLOG(2) << "Merging c->next " << n->ptr << " with c " << c->ptr;
    RemoveFreeChunkFromBin(c->next);
    Merge(h, c->next);
  }

  // If the previous chunk is free, merge c into it and delete c.
  if (c->prev != kInvalidChunkHandle && !ChunkFromHandle(c->prev)->in_use()) {
    Chunk* n = ChunkFromHandle(c->prev);
    VLOG(2) << "Merging c " << c->ptr << " into c->prev " << n->ptr;
    coalesced_chunk = c->prev;
    RemoveFreeChunkFromBin(c->prev);
    Merge(c->prev, h);
  }

  return coalesced_chunk;
}

void BFCAllocator::RemoveFreeChunkFromBin(BFCAllocator::ChunkHandle h) {
  Chunk* c = ChunkFromHandle(h);
  CHECK(!c->in_use() && (c->bin_num != kInvalidBinNum));
  CHECK_GT(BinFromIndex(c->bin_num)->free_chunks.erase(h), 0)
      << "Could not find chunk in bin";
  c->bin_num = kInvalidBinNum;
}

// Merges h1 and h2 when Chunk(h1)->next is h2 and Chunk(h2)->prev is c1.
// We merge Chunk(h2) into Chunk(h1).
void BFCAllocator::Merge(BFCAllocator::ChunkHandle h1,
                         BFCAllocator::ChunkHandle h2) {
  Chunk* c1 = ChunkFromHandle(h1);
  Chunk* c2 = ChunkFromHandle(h2);
  // We can only merge chunks that are not in use.
  CHECK(!c1->in_use() && !c2->in_use());

  // c1's prev doesn't change, still points to the same ptr, and is
  // still not in use.

  // Fix up neighbor pointers
  //
  // c1 <-> c2 <-> c3 should become
  // c1 <-> c3

  BFCAllocator::ChunkHandle h3 = c2->next;
  c1->next = h3;
  CHECK(c2->prev == h1);
  if (h3 != kInvalidChunkHandle) {
    BFCAllocator::Chunk* c3 = ChunkFromHandle(h3);
    c3->prev = h1;
  }

  // Set the new size
  c1->size += c2->size;

  DeleteChunk(h2);
}

void BFCAllocator::DeleteChunk(ChunkHandle h) {
  // Delete h and cleanup all state
  Chunk* c = ChunkFromHandle(h);
  region_manager_.erase(c->ptr);
  DeallocateChunk(h);
}

void BFCAllocator::DeallocateChunk(ChunkHandle h) {
  Chunk* c = ChunkFromHandle(h);
  c->allocation_id = -1;
  c->bin_num = kInvalidBinNum;
  c->next = free_chunks_list_;
  free_chunks_list_ = h;
}

BFCAllocator::Chunk* BFCAllocator::ChunkFromHandle(ChunkHandle h) {
  DCHECK_GE(h, 0);
  DCHECK_LT(h, static_cast<int>(chunks_.size()));
  return &(chunks_[h]);
}

const BFCAllocator::Chunk* BFCAllocator::ChunkFromHandle(ChunkHandle h) const {
  DCHECK_GE(h, 0);
  DCHECK_LT(h, static_cast<int>(chunks_.size()));
  return &(chunks_[h]);
}

}  // namespace itex
