#include "szd/szd_channel_factory.hpp"

#include "szd/szd.h"
#include "szd/szd_channel.hpp"
#include "szd/szd_status.hpp"

#include <cassert>

namespace SIMPLE_ZNS_DEVICE_NAMESPACE {
SZDChannelFactory::SZDChannelFactory(EngineManager *em,
                                     size_t max_channel_count)
    : max_channel_count_(max_channel_count), channel_count_(0),
      em_(em), refs_(0) {}
SZDChannelFactory::~SZDChannelFactory() {}

SZDStatus SZDChannelFactory::register_raw_qpair(QPair **qpair) {
  if (channel_count_ >= max_channel_count_ || qpair == nullptr) {
    SZD_LOG_ERROR("SZD: Channel factory: Too many QPairs\n");
    return SZDStatus::InvalidArguments;
  }
  SZDStatus s = FromStatus(szd_create_qpair(em_, qpair));
  if (s == SZDStatus::Success) {
    channel_count_++;
  }
  return s;
}

SZDStatus SZDChannelFactory::unregister_raw_qpair(QPair *qpair) {
  SZDStatus s = FromStatus(szd_destroy_qpair(em_, qpair));
  if (s == SZDStatus::Success) {
    channel_count_--;
  }
  return s;
}

SZDStatus SZDChannelFactory::register_channel(SZDChannel **channel,
                                              uint64_t min_zone_nr,
                                              uint64_t max_zone_nr,
                                              bool preserve_async_buffer,
                                              uint32_t channel_depth) {
  if (channel_count_ >= max_channel_count_) {
    SZD_LOG_ERROR("SZD: Channel factory: Too many Channels\n");
    return SZDStatus::InvalidArguments;
  }
  SZDStatus s;
  QPair **qpair = new QPair *;
  if ((s = FromStatus(szd_create_qpair(em_, qpair))) !=
      SZDStatus::Success) {
    SZD_LOG_ERROR("SZD: Channel factory: Could not create QPair\n");
    return s;
  }
  *channel =
      new SZDChannel(em_, std::unique_ptr<QPair>(*qpair), em_->manager_->info,
                     min_zone_nr * em_->manager_->info.zone_size,
                     max_zone_nr * em_->manager_->info.zone_size,
                     preserve_async_buffer, channel_depth);

  channel_count_++;
  delete qpair;
  return SZDStatus::Success;
}

SZDStatus SZDChannelFactory::register_channel(SZDChannel **channel,
                                              bool preserve_async_buffer,
                                              uint32_t channel_depth) {
  return register_channel(
      channel, em_->manager_->info.min_lba / em_->manager_->info.zone_size,
      em_->manager_->info.max_lba / em_->manager_->info.zone_size,
      preserve_async_buffer, channel_depth);
}

SZDStatus SZDChannelFactory::unregister_channel(SZDChannel *channel) {
  delete channel;
  channel_count_--;
  return SZDStatus::Success;
}
} // namespace SIMPLE_ZNS_DEVICE_NAMESPACE