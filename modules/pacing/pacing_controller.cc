/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/pacing_controller.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/match.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/pacing/bitrate_prober.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"

extern "C" __attribute__((visibility("default"), used)) const char*
DuDeskRealtimeVideoPacerBuildMarker() {
  return "DUDESK_REALTIME_VIDEO_PACER_FULL_FRAME_TIMING_V6";
}

namespace webrtc {
namespace {
constexpr TimeDelta kCongestedPacketInterval = TimeDelta::Millis(500);
// TODO(sprang): Consider dropping this limit.
// The maximum debt level, in terms of time, capped when sending packets.
constexpr TimeDelta kMaxDebtInTime = TimeDelta::Millis(500);
constexpr TimeDelta kMaxElapsedTime = TimeDelta::Seconds(2);

bool IsDisabled(const FieldTrialsView& field_trials, absl::string_view key) {
  return absl::StartsWith(field_trials.Lookup(key), "Disabled");
}

bool IsEnabled(const FieldTrialsView& field_trials, absl::string_view key) {
  return absl::StartsWith(field_trials.Lookup(key), "Enabled");
}

int64_t PercentileMs(std::vector<int64_t> samples, int percentile) {
  if (samples.empty()) {
    return -1;
  }
  std::sort(samples.begin(), samples.end());
  const size_t index =
      (samples.size() - 1) * static_cast<size_t>(percentile) / 100;
  return samples[index];
}

}  // namespace

const TimeDelta PacingController::kPausedProcessInterval =
    kCongestedPacketInterval;
const TimeDelta PacingController::kMinSleepTime = TimeDelta::Millis(1);
const TimeDelta PacingController::kTargetPaddingDuration = TimeDelta::Millis(5);
const TimeDelta PacingController::kMaxPaddingReplayDuration =
    TimeDelta::Millis(50);
const TimeDelta PacingController::kMaxEarlyProbeProcessing =
    TimeDelta::Millis(1);

PacingController::RealtimeVideoPacerConfig
PacingController::ParseRealtimeVideoPacerConfig(
    const FieldTrialsView& field_trials) {
  RealtimeVideoPacerConfig config;
  const std::string trial = field_trials.Lookup("DuDesk-RealtimeVideoPacer");
  config.enabled = absl::StartsWith(trial, "Enabled");
  if (!config.enabled) {
    return config;
  }

  FieldTrialConstrained<int> max_frame_burst_bytes(
      "max_frame_burst_bytes",
      static_cast<int>(config.max_frame_burst_size.bytes()),
      static_cast<int>(kMaxBurstSize.bytes()), 512 * 1024);
  FieldTrialConstrained<int> max_packet_group_bytes(
      "max_packet_group_bytes",
      static_cast<int>(config.max_packet_group_size.bytes()), 16 * 1024,
      static_cast<int>(kMaxBurstSize.bytes()));
  FieldTrialConstrained<int> packet_group_interval_ms(
      "packet_group_interval_ms",
      static_cast<int>(config.packet_group_interval.ms()), 1, 5);
  FieldTrialConstrained<int> max_cross_frame_debt_ms(
      "max_cross_frame_debt_ms",
      static_cast<int>(config.max_cross_frame_debt.ms()), 0, 20);
  const size_t parameter_start = trial.find(',');
  if (parameter_start != std::string::npos) {
    ParseFieldTrial({&max_frame_burst_bytes, &max_packet_group_bytes,
                     &packet_group_interval_ms, &max_cross_frame_debt_ms},
                    trial.substr(parameter_start + 1));
  }
  config.max_frame_burst_size = DataSize::Bytes(max_frame_burst_bytes.Get());
  config.max_packet_group_size = DataSize::Bytes(max_packet_group_bytes.Get());
  config.packet_group_interval =
      TimeDelta::Millis(packet_group_interval_ms.Get());
  config.max_cross_frame_debt =
      TimeDelta::Millis(max_cross_frame_debt_ms.Get());
  return config;
}

PacingController::PacingController(Clock* clock,
                                   PacketSender* packet_sender,
                                   const FieldTrialsView& field_trials,
                                   Configuration configuration)
    : clock_(clock),
      packet_sender_(packet_sender),
      field_trials_(field_trials),
      drain_large_queues_(
          configuration.drain_large_queues &&
          !IsDisabled(field_trials_, "WebRTC-Pacer-DrainQueue")),
      send_padding_if_silent_(
          IsEnabled(field_trials_, "WebRTC-Pacer-PadInSilence")),
      pace_audio_(IsEnabled(field_trials_, "WebRTC-Pacer-BlockAudio")),
      ignore_transport_overhead_(
          IsEnabled(field_trials_, "WebRTC-Pacer-IgnoreTransportOverhead")),
      fast_retransmissions_(
          IsEnabled(field_trials_, "WebRTC-Pacer-FastRetransmissions")),
      keyframe_flushing_(
          configuration.keyframe_flushing ||
          IsEnabled(field_trials_, "WebRTC-Pacer-KeyframeFlushing")),
      realtime_video_pacer_(ParseRealtimeVideoPacerConfig(field_trials_)),
      transport_overhead_per_packet_(DataSize::Zero()),
      send_burst_interval_(configuration.send_burst_interval),
      realtime_video_last_log_time_(clock_->CurrentTime()),
      realtime_video_frame_bursts_(0),
      realtime_video_frame_packets_(0),
      realtime_video_frame_bytes_(0),
      realtime_video_max_frame_bytes_(DataSize::Zero()),
      realtime_video_packet_groups_(0),
      realtime_video_continuation_wakeups_(0),
      realtime_video_frame_cap_hits_(0),
      realtime_video_max_packet_group_bytes_(DataSize::Zero()),
      realtime_video_max_frame_send_span_(TimeDelta::Zero()),
      realtime_video_max_capture_to_first_send_(TimeDelta::Zero()),
      realtime_video_max_capture_to_last_send_(TimeDelta::Zero()),
      realtime_video_debt_clamps_(0),
      realtime_video_debt_reduction_bytes_(0),
      realtime_video_max_debt_before_clamp_(TimeDelta::Zero()),
      realtime_video_max_debt_after_clamp_(TimeDelta::Zero()),
      realtime_video_continuation_pending_(false),
      realtime_video_continuation_ssrc_(0),
      realtime_video_continuation_timestamp_(0),
      realtime_video_continuation_send_time_(Timestamp::MinusInfinity()),
      realtime_video_cross_frame_send_time_(Timestamp::MinusInfinity()),
      realtime_video_active_frame_bytes_(DataSize::Zero()),
      realtime_video_active_frame_packets_(0),
      realtime_video_active_packet_groups_(0),
      realtime_video_active_max_packet_group_bytes_(DataSize::Zero()),
      realtime_video_active_first_send_time_(Timestamp::MinusInfinity()),
      realtime_video_active_capture_time_(Timestamp::MinusInfinity()),
      last_timestamp_(clock_->CurrentTime()),
      paused_(false),
      media_debt_(DataSize::Zero()),
      padding_debt_(DataSize::Zero()),
      pacing_rate_(DataRate::Zero()),
      adjusted_media_rate_(DataRate::Zero()),
      padding_rate_(DataRate::Zero()),
      prober_(field_trials_),
      probing_send_failure_(false),
      last_process_time_(clock->CurrentTime()),
      last_send_time_(last_process_time_),
      seen_first_packet_(false),
      packet_queue_(/*creation_time=*/last_process_time_,
                    configuration.prioritize_audio_retransmission,
                    configuration.packet_queue_ttl),
      congested_(false),
      queue_time_limit_(configuration.queue_time_limit),
      account_for_audio_(false),
      include_overhead_(false),
      circuit_breaker_threshold_(1 << 16) {
  if (!drain_large_queues_) {
    RTC_LOG(LS_WARNING) << "Pacer queues will not be drained,"
                           "pushback experiment must be enabled.";
  }
  if (realtime_video_pacer_.enabled) {
    RTC_LOG(LS_INFO)
        << "DUDESK_REALTIME_VIDEO_PACER enabled=1 mode=controlled-packet-groups"
        << " debt_window_ms=" << send_burst_interval_.ms()
        << " stock_burst_cap_bytes=" << kMaxBurstSize.bytes()
        << " max_frame_burst_bytes="
        << realtime_video_pacer_.max_frame_burst_size.bytes()
        << " max_packet_group_bytes="
        << realtime_video_pacer_.max_packet_group_size.bytes()
        << " packet_group_interval_ms="
        << realtime_video_pacer_.packet_group_interval.ms()
        << " max_cross_frame_debt_ms="
        << realtime_video_pacer_.max_cross_frame_debt.ms();
  }
}

PacingController::~PacingController() = default;

void PacingController::CreateProbeClusters(
    rtc::ArrayView<const ProbeClusterConfig> probe_cluster_configs) {
  for (const ProbeClusterConfig probe_cluster_config : probe_cluster_configs) {
    prober_.CreateProbeCluster(probe_cluster_config);
  }
}

void PacingController::Pause() {
  if (!paused_)
    RTC_LOG(LS_INFO) << "PacedSender paused.";
  paused_ = true;
  packet_queue_.SetPauseState(true, CurrentTime());
}

void PacingController::Resume() {
  if (paused_)
    RTC_LOG(LS_INFO) << "PacedSender resumed.";
  paused_ = false;
  packet_queue_.SetPauseState(false, CurrentTime());
}

bool PacingController::IsPaused() const {
  return paused_;
}

void PacingController::SetCongested(bool congested) {
  if (congested_ && !congested) {
    UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(CurrentTime()));
  }
  congested_ = congested;
}

void PacingController::SetCircuitBreakerThreshold(int num_iterations) {
  circuit_breaker_threshold_ = num_iterations;
}

void PacingController::RemovePacketsForSsrc(uint32_t ssrc) {
  packet_queue_.RemovePacketsForSsrc(ssrc);
}

bool PacingController::IsProbing() const {
  return prober_.is_probing();
}

Timestamp PacingController::CurrentTime() const {
  Timestamp time = clock_->CurrentTime();
  if (time < last_timestamp_) {
    RTC_LOG(LS_WARNING)
        << "Non-monotonic clock behavior observed. Previous timestamp: "
        << last_timestamp_.ms() << ", new timestamp: " << time.ms();
    RTC_DCHECK_GE(time, last_timestamp_);
    time = last_timestamp_;
  }
  last_timestamp_ = time;
  return time;
}

void PacingController::SetProbingEnabled(bool enabled) {
  RTC_CHECK(!seen_first_packet_);
  prober_.SetEnabled(enabled);
}

void PacingController::SetPacingRates(DataRate pacing_rate,
                                      DataRate padding_rate) {
  RTC_CHECK_GT(pacing_rate, DataRate::Zero());
  RTC_CHECK_GE(padding_rate, DataRate::Zero());
  if (padding_rate > pacing_rate) {
    RTC_LOG(LS_WARNING) << "Padding rate " << padding_rate.kbps()
                        << "kbps is higher than the pacing rate "
                        << pacing_rate.kbps() << "kbps, capping.";
    padding_rate = pacing_rate;
  }

  if (pacing_rate > max_rate || padding_rate > max_rate) {
    RTC_LOG(LS_WARNING) << "Very high pacing rates ( > " << max_rate.kbps()
                        << " kbps) configured: pacing = " << pacing_rate.kbps()
                        << " kbps, padding = " << padding_rate.kbps()
                        << " kbps.";
    max_rate = std::max(pacing_rate, padding_rate) * 1.1;
  }
  pacing_rate_ = pacing_rate;
  padding_rate_ = padding_rate;
  MaybeUpdateMediaRateDueToLongQueue(CurrentTime());

  RTC_LOG(LS_VERBOSE) << "bwe:pacer_updated pacing_kbps=" << pacing_rate_.kbps()
                      << " padding_budget_kbps=" << padding_rate.kbps();
}

void PacingController::EnqueuePacket(std::unique_ptr<RtpPacketToSend> packet) {
  RTC_DCHECK(pacing_rate_ > DataRate::Zero())
      << "SetPacingRate must be called before InsertPacket.";
  RTC_CHECK(packet->packet_type());

  if (keyframe_flushing_ &&
      packet->packet_type() == RtpPacketMediaType::kVideo &&
      packet->is_key_frame() && packet->is_first_packet_of_frame() &&
      !packet_queue_.HasKeyframePackets(packet->Ssrc())) {
    // First packet of a keyframe (and no keyframe packets currently in the
    // queue). Flush any pending packets currently in the queue for that stream
    // in order to get the new keyframe out as quickly as possible.
    packet_queue_.RemovePacketsForSsrc(packet->Ssrc());
    absl::optional<uint32_t> rtx_ssrc =
        packet_sender_->GetRtxSsrcForMedia(packet->Ssrc());
    if (rtx_ssrc) {
      packet_queue_.RemovePacketsForSsrc(*rtx_ssrc);
    }
  }

  prober_.OnIncomingPacket(DataSize::Bytes(packet->payload_size()));

  const Timestamp now = CurrentTime();
  if (packet_queue_.Empty()) {
    // If queue is empty, we need to "fast-forward" the last process time,
    // so that we don't use passed time as budget for sending the first new
    // packet.
    Timestamp target_process_time = now;
    Timestamp next_send_time = NextSendTime();
    if (next_send_time.IsFinite()) {
      // There was already a valid planned send time, such as a keep-alive.
      // Use that as last process time only if it's prior to now.
      target_process_time = std::min(now, next_send_time);
    }
    UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(target_process_time));
  }
  packet_queue_.Push(now, std::move(packet));
  seen_first_packet_ = true;

  // Queue length has increased, check if we need to change the pacing rate.
  MaybeUpdateMediaRateDueToLongQueue(now);
}

void PacingController::SetAccountForAudioPackets(bool account_for_audio) {
  account_for_audio_ = account_for_audio;
}

void PacingController::SetIncludeOverhead() {
  include_overhead_ = true;
}

void PacingController::SetTransportOverhead(DataSize overhead_per_packet) {
  if (ignore_transport_overhead_)
    return;
  transport_overhead_per_packet_ = overhead_per_packet;
}

void PacingController::SetSendBurstInterval(TimeDelta burst_interval) {
  send_burst_interval_ = burst_interval;
}

void PacingController::SetAllowProbeWithoutMediaPacket(bool allow) {
  prober_.SetAllowProbeWithoutMediaPacket(allow);
}

TimeDelta PacingController::ExpectedQueueTime() const {
  RTC_DCHECK_GT(adjusted_media_rate_, DataRate::Zero());
  return QueueSizeData() / adjusted_media_rate_;
}

size_t PacingController::QueueSizePackets() const {
  return rtc::checked_cast<size_t>(packet_queue_.SizeInPackets());
}

const std::array<int, kNumMediaTypes>&
PacingController::SizeInPacketsPerRtpPacketMediaType() const {
  return packet_queue_.SizeInPacketsPerRtpPacketMediaType();
}

DataSize PacingController::QueueSizeData() const {
  DataSize size = packet_queue_.SizeInPayloadBytes();
  if (include_overhead_) {
    size += static_cast<int64_t>(packet_queue_.SizeInPackets()) *
            transport_overhead_per_packet_;
  }
  return size;
}

DataSize PacingController::CurrentBufferLevel() const {
  return std::max(media_debt_, padding_debt_);
}

absl::optional<Timestamp> PacingController::FirstSentPacketTime() const {
  return first_sent_packet_time_;
}

Timestamp PacingController::OldestPacketEnqueueTime() const {
  return packet_queue_.OldestEnqueueTime();
}

TimeDelta PacingController::UpdateTimeAndGetElapsed(Timestamp now) {
  // If no previous processing, or last process was "in the future" because of
  // early probe processing, then there is no elapsed time to add budget for.
  if (last_process_time_.IsMinusInfinity() || now < last_process_time_) {
    return TimeDelta::Zero();
  }
  TimeDelta elapsed_time = now - last_process_time_;
  last_process_time_ = now;
  if (elapsed_time > kMaxElapsedTime) {
    RTC_LOG(LS_WARNING) << "Elapsed time (" << ToLogString(elapsed_time)
                        << ") longer than expected, limiting to "
                        << ToLogString(kMaxElapsedTime);
    elapsed_time = kMaxElapsedTime;
  }
  return elapsed_time;
}

bool PacingController::ShouldSendKeepalive(Timestamp now) const {
  if (send_padding_if_silent_ || paused_ || congested_ || !seen_first_packet_) {
    // We send a padding packet every 500 ms to ensure we won't get stuck in
    // congested state due to no feedback being received.
    if (now - last_send_time_ >= kCongestedPacketInterval) {
      return true;
    }
  }
  return false;
}

Timestamp PacingController::NextSendTime() const {
  const Timestamp now = CurrentTime();
  Timestamp next_send_time = Timestamp::PlusInfinity();

  if (paused_) {
    return last_send_time_ + kPausedProcessInterval;
  }

  // If probing is active, that always takes priority.
  if (prober_.is_probing() && !probing_send_failure_) {
    Timestamp probe_time = prober_.NextProbeTime(now);
    if (!probe_time.IsPlusInfinity()) {
      return probe_time.IsMinusInfinity() ? now : probe_time;
    }
  }

  // If queue contains a packet which should not be paced, its target send time
  // is the time at which it was enqueued.
  Timestamp unpaced_send_time = NextUnpacedSendTime();
  if (unpaced_send_time.IsFinite()) {
    return unpaced_send_time;
  }

  if (congested_ || !seen_first_packet_) {
    // We need to at least send keep-alive packets with some interval.
    return last_send_time_ + kCongestedPacketInterval;
  }

  if (IsRealtimeVideoFrameContinuationPending()) {
    return realtime_video_continuation_send_time_;
  }

  const RtpPacketToSend* next_packet = packet_queue_.Peek();
  if (realtime_video_pacer_.enabled && next_packet != nullptr &&
      next_packet->packet_type() == RtpPacketMediaType::kVideo &&
      realtime_video_cross_frame_send_time_.IsFinite() &&
      now < realtime_video_cross_frame_send_time_) {
    return realtime_video_cross_frame_send_time_;
  }

  if (adjusted_media_rate_ > DataRate::Zero() && !packet_queue_.Empty()) {
    // If packets are allowed to be sent in a burst, the
    // debt is allowed to grow up to one packet more than what can be sent
    // during 'send_burst_period_'.
    TimeDelta drain_time = media_debt_ / adjusted_media_rate_;
    // Ensure that a burst of sent packet is not larger than kMaxBurstSize in
    // order to not risk overfilling socket buffers at high bitrate.
    TimeDelta send_burst_interval =
        std::min(send_burst_interval_, kMaxBurstSize / adjusted_media_rate_);
    next_send_time =
        last_process_time_ +
        ((send_burst_interval > drain_time) ? TimeDelta::Zero() : drain_time);
  } else if (padding_rate_ > DataRate::Zero() && packet_queue_.Empty()) {
    // If we _don't_ have pending packets, check how long until we have
    // bandwidth for padding packets. Both media and padding debts must
    // have been drained to do this.
    RTC_DCHECK_GT(adjusted_media_rate_, DataRate::Zero());
    TimeDelta drain_time = std::max(media_debt_ / adjusted_media_rate_,
                                    padding_debt_ / padding_rate_);

    if (drain_time.IsZero() &&
        (!media_debt_.IsZero() || !padding_debt_.IsZero())) {
      // We have a non-zero debt, but drain time is smaller than tick size of
      // TimeDelta, round it up to the smallest possible non-zero delta.
      drain_time = TimeDelta::Micros(1);
    }
    next_send_time = last_process_time_ + drain_time;
  } else {
    // Nothing to do.
    next_send_time = last_process_time_ + kPausedProcessInterval;
  }

  if (send_padding_if_silent_) {
    next_send_time =
        std::min(next_send_time, last_send_time_ + kPausedProcessInterval);
  }

  return next_send_time;
}

bool PacingController::IsRealtimeVideoFrameContinuationPending() const {
  if (!realtime_video_pacer_.enabled || !realtime_video_continuation_pending_ ||
      congested_) {
    return false;
  }

  const RtpPacketToSend* next_packet = packet_queue_.Peek();
  return next_packet != nullptr &&
         next_packet->packet_type() == RtpPacketMediaType::kVideo &&
         next_packet->Ssrc() == realtime_video_continuation_ssrc_ &&
         next_packet->Timestamp() == realtime_video_continuation_timestamp_;
}

void PacingController::ProcessPackets() {
  absl::Cleanup cleanup = [packet_sender = packet_sender_] {
    packet_sender->OnBatchComplete();
  };
  const Timestamp now = CurrentTime();
  Timestamp target_send_time = now;

  if (ShouldSendKeepalive(now)) {
    DataSize keepalive_data_sent = DataSize::Zero();
    // We can not send padding unless a normal packet has first been sent. If
    // we do, timestamps get messed up.
    if (seen_first_packet_) {
      std::vector<std::unique_ptr<RtpPacketToSend>> keepalive_packets =
          packet_sender_->GeneratePadding(DataSize::Bytes(1));
      for (auto& packet : keepalive_packets) {
        keepalive_data_sent +=
            DataSize::Bytes(packet->payload_size() + packet->padding_size());
        packet_sender_->SendPacket(std::move(packet), PacedPacketInfo());
        for (auto& packet : packet_sender_->FetchFec()) {
          EnqueuePacket(std::move(packet));
        }
      }
    }
    OnPacketSent(RtpPacketMediaType::kPadding, keepalive_data_sent, now);
  }

  if (paused_) {
    return;
  }

  TimeDelta early_execute_margin =
      prober_.is_probing() ? kMaxEarlyProbeProcessing : TimeDelta::Zero();

  target_send_time = NextSendTime();
  if (now + early_execute_margin < target_send_time) {
    // We are too early, but if queue is empty still allow draining some debt.
    // Probing is allowed to be sent up to kMinSleepTime early.
    UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(now));
    return;
  }

  TimeDelta elapsed_time = UpdateTimeAndGetElapsed(target_send_time);

  if (elapsed_time > TimeDelta::Zero()) {
    UpdateBudgetWithElapsedTime(elapsed_time);
  }

  PacedPacketInfo pacing_info;
  DataSize recommended_probe_size = DataSize::Zero();
  bool is_probing = prober_.is_probing();
  if (is_probing) {
    // Probe timing is sensitive, and handled explicitly by BitrateProber, so
    // use actual send time rather than target.
    pacing_info = prober_.CurrentCluster(now).value_or(PacedPacketInfo());
    if (pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe) {
      recommended_probe_size = prober_.RecommendedMinProbeSize();
      RTC_DCHECK_GT(recommended_probe_size, DataSize::Zero());
    } else {
      // No valid probe cluster returned, probe might have timed out.
      is_probing = false;
    }
  }

  DataSize data_sent = DataSize::Zero();
  int iteration = 0;
  int packets_sent = 0;
  int padding_packets_generated = 0;
  bool realtime_video_frame_burst_extended = false;
  DataSize realtime_video_packet_group_bytes = DataSize::Zero();
  int realtime_video_packet_group_packets = 0;
  Timestamp realtime_video_packet_group_last_send = Timestamp::MinusInfinity();
  bool realtime_video_group_continuation_scheduled = false;
  bool realtime_video_frame_complete = false;

  if (IsRealtimeVideoFrameContinuationPending()) {
    realtime_video_continuation_pending_ = false;
    ++realtime_video_continuation_wakeups_;
  }
  for (; iteration < circuit_breaker_threshold_; ++iteration) {
    // Fetch packet, so long as queue is not empty or budget is not
    // exhausted.
    std::unique_ptr<RtpPacketToSend> rtp_packet =
        GetPendingPacket(pacing_info, target_send_time, now);
    if (rtp_packet == nullptr) {
      // No packet available to send, check if we should send padding.
      if (now - target_send_time > kMaxPaddingReplayDuration) {
        // The target send time is more than `kMaxPaddingReplayDuration` behind
        // the real-time clock. This can happen if the clock is adjusted forward
        // without `ProcessPackets()` having been called at the expected times.
        target_send_time = now - kMaxPaddingReplayDuration;
        last_process_time_ = std::max(last_process_time_, target_send_time);
      }

      DataSize padding_to_add = PaddingToAdd(recommended_probe_size, data_sent);
      if (padding_to_add > DataSize::Zero()) {
        std::vector<std::unique_ptr<RtpPacketToSend>> padding_packets =
            packet_sender_->GeneratePadding(padding_to_add);
        if (!padding_packets.empty()) {
          padding_packets_generated += padding_packets.size();
          for (auto& packet : padding_packets) {
            EnqueuePacket(std::move(packet));
          }
          // Continue loop to send the padding that was just added.
          continue;
        } else {
          // Can't generate padding, still update padding budget for next send
          // time.
          UpdatePaddingBudgetWithSentData(padding_to_add);
        }
      }
      // Can't fetch new packet and no padding to send, exit send loop.
      break;
    } else {
      RTC_DCHECK(rtp_packet);
      RTC_DCHECK(rtp_packet->packet_type().has_value());
      const RtpPacketMediaType packet_type = *rtp_packet->packet_type();
      const uint32_t packet_ssrc = rtp_packet->Ssrc();
      const uint32_t packet_timestamp = rtp_packet->Timestamp();
      const bool packet_marker = rtp_packet->Marker();
      const absl::optional<Timestamp> packet_encode_finish_time =
          rtp_packet->frame_encode_finish_time();
      const Timestamp packet_capture_time = rtp_packet->capture_time();
      DataSize packet_size = DataSize::Bytes(rtp_packet->payload_size() +
                                             rtp_packet->padding_size());

      if (include_overhead_) {
        packet_size += DataSize::Bytes(rtp_packet->headers_size()) +
                       transport_overhead_per_packet_;
      }

      const Timestamp packet_send_start = CurrentTime();
      packet_sender_->SendPacket(std::move(rtp_packet), pacing_info);
      const Timestamp packet_send_end = CurrentTime();
      for (auto& packet : packet_sender_->FetchFec()) {
        EnqueuePacket(std::move(packet));
      }
      data_sent += packet_size;
      ++packets_sent;
      if (packet_type == RtpPacketMediaType::kVideo) {
        if (realtime_video_pacer_.enabled && !is_probing) {
          if (realtime_video_active_frame_ssrc_ != packet_ssrc ||
              realtime_video_active_frame_timestamp_ != packet_timestamp) {
            ResetRealtimeVideoFrameState();
            realtime_video_active_frame_ssrc_ = packet_ssrc;
            realtime_video_active_frame_timestamp_ = packet_timestamp;
            realtime_video_active_first_send_time_ = packet_send_start;
            realtime_video_active_capture_time_ = packet_capture_time;
            if (packet_encode_finish_time) {
              realtime_video_active_encode_finish_time_ =
                  *packet_encode_finish_time;
            }
          }
          realtime_video_active_frame_bytes_ += packet_size;
          realtime_video_active_burst_bytes_ += packet_size;
          ++realtime_video_active_frame_packets_;
          realtime_video_packet_group_bytes += packet_size;
          ++realtime_video_packet_group_packets;
          realtime_video_packet_group_last_send = packet_send_end;
          realtime_video_frame_burst_extended = true;
        }
      }

      // Send done, update send time.
      OnPacketSent(packet_type, packet_size, now);

      if (is_probing) {
        pacing_info.probe_cluster_bytes_sent += packet_size.bytes();
        // If we are currently probing, we need to stop the send loop when we
        // have reached the send target.
        if (data_sent >= recommended_probe_size) {
          break;
        }
      }

      // Update target send time in case that are more packets that we are late
      // in processing.
      target_send_time = NextSendTime();
      if (packet_type == RtpPacketMediaType::kVideo && packet_marker &&
          realtime_video_pacer_.enabled && !is_probing) {
        realtime_video_frame_complete = true;
        break;
      }
      if (realtime_video_pacer_.enabled && !is_probing &&
          realtime_video_active_frame_ssrc_ == packet_ssrc &&
          realtime_video_active_frame_timestamp_ == packet_timestamp) {
        absl::optional<DataSize> next_packet_size =
            NextRealtimeVideoPacketSize(packet_ssrc, packet_timestamp);
        if (!next_packet_size) {
          realtime_video_frame_complete = true;
          break;
        }
        if (realtime_video_active_burst_bytes_ + *next_packet_size >
            realtime_video_pacer_.max_frame_burst_size) {
          realtime_video_active_burst_bytes_ = DataSize::Zero();
          realtime_video_continuation_pending_ = true;
          realtime_video_continuation_ssrc_ = packet_ssrc;
          realtime_video_continuation_timestamp_ = packet_timestamp;
          realtime_video_continuation_send_time_ =
              CurrentTime() + realtime_video_pacer_.max_cross_frame_debt;
          realtime_video_group_continuation_scheduled = true;
          ClampRealtimeVideoMediaDebtAfterFrameBurst();
          ++realtime_video_frame_cap_hits_;
          break;
        }
        if (realtime_video_packet_group_bytes + *next_packet_size >
            realtime_video_pacer_.max_packet_group_size) {
          realtime_video_continuation_pending_ = true;
          realtime_video_continuation_ssrc_ = packet_ssrc;
          realtime_video_continuation_timestamp_ = packet_timestamp;
          realtime_video_continuation_send_time_ =
              CurrentTime() + realtime_video_pacer_.packet_group_interval;
          realtime_video_group_continuation_scheduled = true;
          break;
        }
        if (target_send_time > now) {
          target_send_time = now;
        }
      } else if (target_send_time > now) {
        // Exit loop if not probing.
        if (!is_probing) {
          break;
        } else {
          target_send_time = now;
        }
      }
      UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(target_send_time));
    }
  }

  if (iteration >= circuit_breaker_threshold_) {
    // Circuit break activated. Log warning, adjust send time and return.
    // TODO(sprang): Consider completely clearing state.
    RTC_LOG(LS_ERROR)
        << "PacingController exceeded max iterations in "
           "send-loop. Debug info: "
        << " packets sent = " << packets_sent
        << ", padding packets generated = " << padding_packets_generated
        << ", bytes sent = " << data_sent.bytes()
        << ", probing = " << (is_probing ? "true" : "false")
        << ", recommended_probe_size = " << recommended_probe_size.bytes()
        << ", now = " << now.us()
        << ", target_send_time = " << target_send_time.us()
        << ", last_process_time = " << last_process_time_.us()
        << ", last_send_time = " << last_send_time_.us()
        << ", paused = " << (paused_ ? "true" : "false")
        << ", media_debt = " << media_debt_.bytes()
        << ", padding_debt = " << padding_debt_.bytes()
        << ", pacing_rate = " << pacing_rate_.bps()
        << ", adjusted_media_rate = " << adjusted_media_rate_.bps()
        << ", padding_rate = " << padding_rate_.bps()
        << ", queue size (packets) = " << packet_queue_.SizeInPackets()
        << ", queue size (payload bytes) = "
        << packet_queue_.SizeInPayloadBytes();
    last_send_time_ = now;
    last_process_time_ = now;
    return;
  }

  if (realtime_video_frame_burst_extended) {
    if (realtime_video_packet_group_packets > 0) {
      ++realtime_video_active_packet_groups_;
      realtime_video_active_max_packet_group_bytes_ =
          std::max(realtime_video_active_max_packet_group_bytes_,
                   realtime_video_packet_group_bytes);
    }
    if (realtime_video_frame_complete &&
        !realtime_video_group_continuation_scheduled) {
      ClampRealtimeVideoMediaDebtAfterFrameBurst();
      realtime_video_cross_frame_send_time_ =
          CurrentTime() + realtime_video_pacer_.max_cross_frame_debt;
      const Timestamp frame_last_send =
          realtime_video_packet_group_last_send.IsFinite()
              ? realtime_video_packet_group_last_send
              : CurrentTime();
      const TimeDelta frame_send_span =
          realtime_video_active_first_send_time_.IsFinite()
              ? std::max(
                    TimeDelta::Zero(),
                    frame_last_send - realtime_video_active_first_send_time_)
              : TimeDelta::Zero();
      const TimeDelta capture_to_first_send =
          realtime_video_active_capture_time_.IsFinite() &&
                  realtime_video_active_first_send_time_ >=
                      realtime_video_active_capture_time_
              ? realtime_video_active_first_send_time_ -
                    realtime_video_active_capture_time_
              : TimeDelta::Zero();
      const TimeDelta capture_to_last_send =
          realtime_video_active_capture_time_.IsFinite() &&
                  frame_last_send >= realtime_video_active_capture_time_
              ? frame_last_send - realtime_video_active_capture_time_
              : TimeDelta::Zero();
      absl::optional<TimeDelta> encode_finish_to_last_send;
      if (realtime_video_active_encode_finish_time_.IsFinite() &&
          frame_last_send >= realtime_video_active_encode_finish_time_) {
        encode_finish_to_last_send =
            frame_last_send - realtime_video_active_encode_finish_time_;
      }
      MaybeLogRealtimeVideoFrameBurst(
          CurrentTime(), realtime_video_active_frame_bytes_,
          realtime_video_active_frame_packets_,
          realtime_video_active_packet_groups_,
          realtime_video_active_max_packet_group_bytes_, frame_send_span,
          capture_to_first_send, capture_to_last_send,
          encode_finish_to_last_send);
      ResetRealtimeVideoFrameState();
    }
  }

  if (is_probing) {
    probing_send_failure_ = data_sent == DataSize::Zero();
    if (!probing_send_failure_) {
      prober_.ProbeSent(CurrentTime(), data_sent);
    }
  }

  // Queue length has probably decreased, check if pacing rate needs to updated.
  // Poll the time again, since we might have enqueued new fec/padding packets
  // with a later timestamp than `now`.
  MaybeUpdateMediaRateDueToLongQueue(CurrentTime());
}

DataSize PacingController::PaddingToAdd(DataSize recommended_probe_size,
                                        DataSize data_sent) const {
  if (!packet_queue_.Empty()) {
    // Actual payload available, no need to add padding.
    return DataSize::Zero();
  }

  if (congested_) {
    // Don't add padding if congested, even if requested for probing.
    return DataSize::Zero();
  }

  if (!recommended_probe_size.IsZero()) {
    if (recommended_probe_size > data_sent) {
      return recommended_probe_size - data_sent;
    }
    return DataSize::Zero();
  }

  if (padding_rate_ > DataRate::Zero() && padding_debt_ == DataSize::Zero()) {
    return kTargetPaddingDuration * padding_rate_;
  }
  return DataSize::Zero();
}

std::unique_ptr<RtpPacketToSend> PacingController::GetPendingPacket(
    const PacedPacketInfo& pacing_info,
    Timestamp target_send_time,
    Timestamp now) {
  const bool is_probe =
      pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe;
  // If first packet in probe, insert a small padding packet so we have a
  // more reliable start window for the rate estimation.
  if (is_probe && pacing_info.probe_cluster_bytes_sent == 0) {
    auto padding = packet_sender_->GeneratePadding(DataSize::Bytes(1));
    // If no RTP modules sending media are registered, we may not get a
    // padding packet back.
    if (!padding.empty()) {
      // We should never get more than one padding packets with a requested
      // size of 1 byte.
      RTC_DCHECK_EQ(padding.size(), 1u);
      return std::move(padding[0]);
    }
  }

  if (packet_queue_.Empty()) {
    return nullptr;
  }

  // First, check if there is any reason _not_ to send the next queued packet.
  // Unpaced packets and probes are exempted from send checks.
  if (NextUnpacedSendTime().IsInfinite() && !is_probe) {
    if (congested_) {
      // Don't send anything if congested.
      return nullptr;
    }

    if (now <= target_send_time && send_burst_interval_.IsZero()) {
      // We allow sending slightly early if we think that we would actually
      // had been able to, had we been right on time - i.e. the current debt
      // is not more than would be reduced to zero at the target sent time.
      // If we allow packets to be sent in a burst, packet are allowed to be
      // sent early.
      TimeDelta flush_time = media_debt_ / adjusted_media_rate_;
      if (now + flush_time > target_send_time) {
        return nullptr;
      }
    }
  }

  return packet_queue_.Pop();
}

void PacingController::OnPacketSent(RtpPacketMediaType packet_type,
                                    DataSize packet_size,
                                    Timestamp send_time) {
  if (!first_sent_packet_time_ && packet_type != RtpPacketMediaType::kPadding) {
    first_sent_packet_time_ = send_time;
  }

  bool audio_packet = packet_type == RtpPacketMediaType::kAudio;
  if ((!audio_packet || account_for_audio_) && packet_size > DataSize::Zero()) {
    UpdateBudgetWithSentData(packet_size);
  }

  last_send_time_ = send_time;
}

void PacingController::UpdateBudgetWithElapsedTime(TimeDelta delta) {
  media_debt_ -= std::min(media_debt_, adjusted_media_rate_ * delta);
  padding_debt_ -= std::min(padding_debt_, padding_rate_ * delta);
}

void PacingController::UpdateBudgetWithSentData(DataSize size) {
  media_debt_ += size;
  media_debt_ = std::min(media_debt_, adjusted_media_rate_ * kMaxDebtInTime);
  UpdatePaddingBudgetWithSentData(size);
}

void PacingController::UpdatePaddingBudgetWithSentData(DataSize size) {
  padding_debt_ += size;
  padding_debt_ = std::min(padding_debt_, padding_rate_ * kMaxDebtInTime);
}

void PacingController::SetQueueTimeLimit(TimeDelta limit) {
  queue_time_limit_ = limit;
}

void PacingController::MaybeUpdateMediaRateDueToLongQueue(Timestamp now) {
  adjusted_media_rate_ = pacing_rate_;
  if (!drain_large_queues_) {
    return;
  }

  DataSize queue_size_data = QueueSizeData();
  if (queue_size_data > DataSize::Zero()) {
    // Assuming equal size packets and input/output rate, the average packet
    // has avg_time_left_ms left to get queue_size_bytes out of the queue, if
    // time constraint shall be met. Determine bitrate needed for that.
    packet_queue_.UpdateAverageQueueTime(now);
    TimeDelta avg_time_left =
        std::max(TimeDelta::Millis(1),
                 queue_time_limit_ - packet_queue_.AverageQueueTime());
    DataRate min_rate_needed = queue_size_data / avg_time_left;
    if (min_rate_needed > pacing_rate_) {
      adjusted_media_rate_ = min_rate_needed;
      RTC_LOG(LS_VERBOSE) << "bwe:large_pacing_queue pacing_rate_kbps="
                          << pacing_rate_.kbps();
    }
  }
}

Timestamp PacingController::NextUnpacedSendTime() const {
  if (!pace_audio_) {
    Timestamp leading_audio_send_time =
        packet_queue_.LeadingPacketEnqueueTime(RtpPacketMediaType::kAudio);
    if (leading_audio_send_time.IsFinite()) {
      return leading_audio_send_time;
    }
  }
  if (fast_retransmissions_) {
    Timestamp leading_retransmission_send_time =
        packet_queue_.LeadingPacketEnqueueTimeForRetransmission();
    if (leading_retransmission_send_time.IsFinite()) {
      return leading_retransmission_send_time;
    }
  }
  return Timestamp::MinusInfinity();
}

absl::optional<DataSize> PacingController::NextRealtimeVideoPacketSize(
    uint32_t ssrc,
    uint32_t rtp_timestamp) const {
  const RtpPacketToSend* next_packet = packet_queue_.Peek();
  if (next_packet == nullptr ||
      next_packet->packet_type() != RtpPacketMediaType::kVideo ||
      next_packet->Ssrc() != ssrc ||
      next_packet->Timestamp() != rtp_timestamp) {
    return absl::nullopt;
  }

  DataSize next_packet_size = DataSize::Bytes(next_packet->payload_size() +
                                              next_packet->padding_size());
  if (include_overhead_) {
    next_packet_size += DataSize::Bytes(next_packet->headers_size()) +
                        transport_overhead_per_packet_;
  }
  return next_packet_size;
}

void PacingController::ClampRealtimeVideoMediaDebtAfterFrameBurst() {
  if (adjusted_media_rate_ <= DataRate::Zero()) {
    return;
  }

  const DataSize max_cross_frame_debt = std::min(
      adjusted_media_rate_ * realtime_video_pacer_.max_cross_frame_debt,
      kMaxBurstSize);
  if (media_debt_ <= max_cross_frame_debt) {
    return;
  }

  const DataSize debt_before_clamp = media_debt_;
  const TimeDelta debt_before_time = media_debt_ / adjusted_media_rate_;
  media_debt_ = max_cross_frame_debt;
  const TimeDelta debt_after_time = media_debt_ / adjusted_media_rate_;

  ++realtime_video_debt_clamps_;
  realtime_video_debt_reduction_bytes_ +=
      (debt_before_clamp - media_debt_).bytes();
  realtime_video_max_debt_before_clamp_ =
      std::max(realtime_video_max_debt_before_clamp_, debt_before_time);
  realtime_video_max_debt_after_clamp_ =
      std::max(realtime_video_max_debt_after_clamp_, debt_after_time);
}

void PacingController::ResetRealtimeVideoFrameState() {
  realtime_video_continuation_pending_ = false;
  realtime_video_continuation_ssrc_ = 0;
  realtime_video_continuation_timestamp_ = 0;
  realtime_video_continuation_send_time_ = Timestamp::MinusInfinity();
  realtime_video_active_frame_ssrc_.reset();
  realtime_video_active_frame_timestamp_.reset();
  realtime_video_active_frame_bytes_ = DataSize::Zero();
  realtime_video_active_burst_bytes_ = DataSize::Zero();
  realtime_video_active_frame_packets_ = 0;
  realtime_video_active_packet_groups_ = 0;
  realtime_video_active_max_packet_group_bytes_ = DataSize::Zero();
  realtime_video_active_first_send_time_ = Timestamp::MinusInfinity();
  realtime_video_active_capture_time_ = Timestamp::MinusInfinity();
  realtime_video_active_encode_finish_time_ = Timestamp::MinusInfinity();
}

void PacingController::MaybeLogRealtimeVideoFrameBurst(
    Timestamp now,
    DataSize frame_bytes,
    int frame_packets,
    int packet_groups,
    DataSize max_packet_group_bytes,
    TimeDelta frame_send_span,
    TimeDelta capture_to_first_send,
    TimeDelta capture_to_last_send,
    absl::optional<TimeDelta> encode_finish_to_last_send) {
  ++realtime_video_frame_bursts_;
  realtime_video_frame_packets_ += frame_packets;
  realtime_video_frame_bytes_ += frame_bytes.bytes();
  realtime_video_max_frame_bytes_ =
      std::max(realtime_video_max_frame_bytes_, frame_bytes);
  realtime_video_packet_groups_ += packet_groups;
  realtime_video_max_packet_group_bytes_ =
      std::max(realtime_video_max_packet_group_bytes_, max_packet_group_bytes);
  realtime_video_max_frame_send_span_ =
      std::max(realtime_video_max_frame_send_span_, frame_send_span);
  realtime_video_max_capture_to_first_send_ = std::max(
      realtime_video_max_capture_to_first_send_, capture_to_first_send);
  realtime_video_max_capture_to_last_send_ =
      std::max(realtime_video_max_capture_to_last_send_, capture_to_last_send);
  if (encode_finish_to_last_send) {
    realtime_video_max_encode_finish_to_last_send_ =
        std::max(realtime_video_max_encode_finish_to_last_send_,
                 *encode_finish_to_last_send);
    realtime_video_encode_to_last_samples_ms_.push_back(
        encode_finish_to_last_send->ms());
  }

  if (now - realtime_video_last_log_time_ < TimeDelta::Seconds(1)) {
    return;
  }

  RTC_LOG(LS_INFO)
      << "DUDESK_REALTIME_VIDEO_PACER frame_bursts="
      << realtime_video_frame_bursts_
      << " last_rtp_ts="
      << (realtime_video_active_frame_timestamp_.has_value()
              ? std::to_string(*realtime_video_active_frame_timestamp_)
              : std::string("-1"))
      << " frame_packets=" << realtime_video_frame_packets_
      << " frame_bytes=" << realtime_video_frame_bytes_
      << " max_frame_bytes=" << realtime_video_max_frame_bytes_.bytes()
      << " packet_groups=" << realtime_video_packet_groups_
      << " continuation_wakeups=" << realtime_video_continuation_wakeups_
      << " frame_cap_hits=" << realtime_video_frame_cap_hits_
      << " max_group_bytes=" << realtime_video_max_packet_group_bytes_.bytes()
      << " max_frame_send_span_ms=" << realtime_video_max_frame_send_span_.ms()
      << " max_capture_to_first_send_ms="
      << realtime_video_max_capture_to_first_send_.ms()
      << " max_capture_to_last_send_ms="
      << realtime_video_max_capture_to_last_send_.ms()
      << " encode_to_last_samples="
      << realtime_video_encode_to_last_samples_ms_.size()
      << " encode_to_last_p50_ms="
      << PercentileMs(realtime_video_encode_to_last_samples_ms_, 50)
      << " encode_to_last_p90_ms="
      << PercentileMs(realtime_video_encode_to_last_samples_ms_, 90)
      << " encode_to_last_p99_ms="
      << PercentileMs(realtime_video_encode_to_last_samples_ms_, 99)
      << " max_encode_to_last_ms="
      << realtime_video_max_encode_finish_to_last_send_.ms()
      << " debt_clamps=" << realtime_video_debt_clamps_
      << " debt_reduction_bytes=" << realtime_video_debt_reduction_bytes_
      << " max_debt_before_ms=" << realtime_video_max_debt_before_clamp_.ms()
      << " max_debt_after_ms=" << realtime_video_max_debt_after_clamp_.ms()
      << " queue_packets=" << packet_queue_.SizeInPackets()
      << " queue_bytes=" << packet_queue_.SizeInPayloadBytes().bytes()
      << " congested=" << (congested_ ? 1 : 0);
  realtime_video_last_log_time_ = now;
  realtime_video_frame_bursts_ = 0;
  realtime_video_frame_packets_ = 0;
  realtime_video_frame_bytes_ = 0;
  realtime_video_max_frame_bytes_ = DataSize::Zero();
  realtime_video_packet_groups_ = 0;
  realtime_video_continuation_wakeups_ = 0;
  realtime_video_frame_cap_hits_ = 0;
  realtime_video_max_packet_group_bytes_ = DataSize::Zero();
  realtime_video_max_frame_send_span_ = TimeDelta::Zero();
  realtime_video_max_capture_to_first_send_ = TimeDelta::Zero();
  realtime_video_max_capture_to_last_send_ = TimeDelta::Zero();
  realtime_video_max_encode_finish_to_last_send_ = TimeDelta::Zero();
  realtime_video_encode_to_last_samples_ms_.clear();
  realtime_video_debt_clamps_ = 0;
  realtime_video_debt_reduction_bytes_ = 0;
  realtime_video_max_debt_before_clamp_ = TimeDelta::Zero();
  realtime_video_max_debt_after_clamp_ = TimeDelta::Zero();
}

}  // namespace webrtc
