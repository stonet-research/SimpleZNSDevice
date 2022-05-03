#include "szd/cpp/szd_channel.h"
#include "szd/cpp/szd_status.h"
#include "szd/szd.h"

#include <cassert>
#include <cstring>
#include <string>

namespace SimpleZNSDeviceNamespace {

SZDChannel::SZDChannel(std::unique_ptr<QPair> qpair, const DeviceInfo &info,
                       uint64_t min_lba, uint64_t max_lba)
    : qpair_(qpair.release()), lba_size_(info.lba_size),
      zone_size_(info.zone_size), min_lba_(min_lba), max_lba_(max_lba),
      can_access_all_(true), backed_memory_spill_(nullptr),
      lba_msb_(msb(info.lba_size)) {
  assert(min_lba_ <= max_lba_);
  // If true, there is a creeping bug not catched during debug? block all IO.
  if (min_lba_ > max_lba) {
    min_lba_ = max_lba_;
  }
  backed_memory_spill_ = szd_calloc(lba_size_, 1, lba_size_);
}

SZDChannel::SZDChannel(std::unique_ptr<QPair> qpair, const DeviceInfo &info)
    : SZDChannel(std::move(qpair), info, 0, info.lba_cap) {
  can_access_all_ = false;
}

SZDChannel::~SZDChannel() {
  if (backed_memory_spill_ != nullptr) {
    szd_free(backed_memory_spill_);
    backed_memory_spill_ = nullptr;
  }
  if (qpair_ != nullptr) {
    szd_destroy_qpair(qpair_);
  }
}

SZDStatus SZDChannel::FlushBufferSection(const SZDBuffer &buffer, uint64_t *lba,
                                         uint64_t addr, uint64_t size,
                                         bool alligned) {
  uint64_t alligned_size = alligned ? size : allign_size(size);
  uint64_t available_size = buffer.GetBufferSize();
  if (addr + alligned_size > available_size ||
      *lba + size / lba_size_ > max_lba_) {
    return SZDStatus::InvalidArguments;
  }
  void *cbuffer;
  SZDStatus s = SZDStatus::Success;
  if ((s = buffer.GetBuffer(&cbuffer)) != SZDStatus::Success) {
    return s;
  }
  if (alligned_size != size) {
    if (backed_memory_spill_ == nullptr) {
      return SZDStatus::IOError;
    }
    uint64_t postfix_size = lba_size_ - (alligned_size - size);
    alligned_size -= lba_size_;
    int rc = 0;
    if (alligned_size > 0) {
      rc = szd_append(qpair_, lba, (char *)cbuffer + addr, alligned_size);
    }
    memset((char *)backed_memory_spill_ + postfix_size, '\0',
           lba_size_ - postfix_size);
    memcpy(backed_memory_spill_, (char *)cbuffer + addr + alligned_size,
           postfix_size);
    rc = rc | szd_append(qpair_, lba, backed_memory_spill_, lba_size_);
    return FromStatus(rc);
  } else {
    return FromStatus(
        szd_append(qpair_, lba, (char *)cbuffer + addr, alligned_size));
  }
}

SZDStatus SZDChannel::FlushBuffer(const SZDBuffer &buffer, uint64_t *lba) {
  return FlushBufferSection(buffer, lba, 0, buffer.GetBufferSize(), true);
}

SZDStatus SZDChannel::ReadIntoBuffer(SZDBuffer *buffer, uint64_t lba,
                                     size_t addr, size_t size, bool alligned) {
  uint64_t alligned_size = alligned ? size : allign_size(size);
  uint64_t available_size = buffer->GetBufferSize();
  if (addr + alligned_size > available_size ||
      lba + size / lba_size_ > max_lba_) {
    return SZDStatus::InvalidArguments;
  }
  void *cbuffer;
  SZDStatus s = SZDStatus::Success;
  if ((s = buffer->GetBuffer(&cbuffer)) != SZDStatus::Success) {
    return s;
  }
  if (alligned_size != size) {
    if (backed_memory_spill_ == nullptr) {
      return SZDStatus::IOError;
    }
    uint64_t postfix_size = lba_size_ - (alligned_size - size);
    alligned_size -= lba_size_;
    int rc = 0;
    if (alligned_size > 0) {
      rc = szd_read(qpair_, lba, (char *)cbuffer + addr, alligned_size);
    }
    rc = rc | szd_read(qpair_, lba + alligned_size / lba_size_,
                       (char *)backed_memory_spill_, lba_size_);
    s = FromStatus(rc);
    if (s == SZDStatus::Success) {
      memcpy((char *)buffer + addr + alligned_size, backed_memory_spill_,
             postfix_size);
    }
    return s;
  } else {
    return FromStatus(
        szd_read(qpair_, lba, (char *)cbuffer + addr, alligned_size));
  }
}

SZDStatus SZDChannel::DirectAppend(uint64_t *lba, void *buffer,
                                   const uint64_t size, bool alligned) const {
  uint64_t alligned_size = alligned ? size : allign_size(size);
  if (*lba + alligned_size / lba_size_ > max_lba_) {
    return SZDStatus::InvalidArguments;
  }
  void *dma_buffer = szd_calloc(lba_size_, 1, alligned_size);
  if (dma_buffer == nullptr) {
    return SZDStatus::IOError;
  }
  memcpy(dma_buffer, buffer, size);
  SZDStatus s = FromStatus(szd_append(qpair_, lba, dma_buffer, alligned_size));
  szd_free(dma_buffer);
  return s;
}

SZDStatus SZDChannel::DirectRead(void *buffer, uint64_t lba, uint64_t size,
                                 bool alligned) const {
  uint64_t alligned_size = alligned ? size : allign_size(size);
  void *buffer_dma = szd_calloc(lba_size_, 1, alligned_size);
  if (buffer_dma == nullptr) {
    return SZDStatus::IOError;
  }
  SZDStatus s = FromStatus(szd_read(qpair_, lba, buffer_dma, alligned_size));
  if (s == SZDStatus::Success) {
    memcpy(buffer, buffer_dma, size);
  }
  szd_free(buffer_dma);
  return s;
}

SZDStatus SZDChannel::ResetZone(uint64_t slba) const {
  if (slba < min_lba_ || slba > max_lba_) {
    return SZDStatus::InvalidArguments;
  }
  return FromStatus(szd_reset(qpair_, slba));
}

SZDStatus SZDChannel::ResetAllZones() const {
  SZDStatus s = SZDStatus::Success;
  // There is no partial reset, reset the partial zones one by one.
  if (!can_access_all_) {
    for (uint64_t slba = min_lba_; slba != max_lba_; slba += zone_size_) {
      if ((s = ResetZone(slba)) != SZDStatus::Success) {
        return s;
      }
    }
    return s;
  } else {
    return FromStatus(szd_reset_all(qpair_));
  }
}

SZDStatus SZDChannel::ZoneHead(uint64_t slba, uint64_t *zone_head) const {
  if (slba < min_lba_ || slba > max_lba_) {
    return SZDStatus::InvalidArguments;
  }
  return FromStatus(szd_get_zone_head(qpair_, slba, zone_head));
}

} // namespace SimpleZNSDeviceNamespace
