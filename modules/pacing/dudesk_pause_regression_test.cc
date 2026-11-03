// Regression probe for the production PacingController (no mock queue).
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "modules/pacing/pacing_controller.h"
#include "system_wrappers/include/clock.h"

namespace {
using namespace webrtc;
class Trials final : public FieldTrialsView {
 public:
  explicit Trials(std::string realtime_extra = "")
      : realtime_extra_(std::move(realtime_extra)) {}
  std::string Lookup(absl::string_view key) const override {
    if (key == "DuDesk-RealtimeVideoPacer") {
      if (realtime_extra_ == "off") return "";
      return "Enabled,max_frame_burst_bytes:131072,"
             "max_packet_group_bytes:24576,packet_group_interval_ms:2,"
             "max_cross_frame_debt_ms:3" + realtime_extra_;
    }
    if (key == "WebRTC-Pacer-KeyframeFlushing") return "Enabled";
    return "";
  }

 private:
  const std::string realtime_extra_;
};

struct Sent {
  uint32_t ssrc;
  uint16_t seq;
  uint32_t timestamp;
  size_t bytes;
  int64_t queue_ms = 0;  // send time - capture time, when a clock is set
  int64_t send_ms = 0;
};
class Sender final : public PacingController::PacketSender {
 public:
  std::vector<Sent> sent;
  SimulatedClock* clock = nullptr;
  void SendPacket(std::unique_ptr<RtpPacketToSend> packet,
                  const PacedPacketInfo&) override {
    Sent s{packet->Ssrc(), packet->SequenceNumber(), packet->Timestamp(),
           packet->payload_size()};
    if (clock) {
      s.send_ms = clock->TimeInMilliseconds();
      s.queue_ms = s.send_ms - packet->capture_time().ms();
    }
    sent.push_back(s);
  }
  std::vector<std::unique_ptr<RtpPacketToSend>> FetchFec() override {
    return {};
  }
  std::vector<std::unique_ptr<RtpPacketToSend>> GeneratePadding(
      DataSize) override { return {}; }
  absl::optional<uint32_t> GetRtxSsrcForMedia(uint32_t ssrc) const override {
    return ssrc == 1 ? absl::optional<uint32_t>(2) : absl::nullopt;
  }
};

std::unique_ptr<RtpPacketToSend> Packet(
    SimulatedClock& clock, RtpPacketMediaType type, uint32_t ssrc,
    uint16_t seq, uint32_t ts, size_t bytes, bool key, bool first) {
  auto packet = std::make_unique<RtpPacketToSend>(nullptr);
  packet->set_packet_type(type);
  packet->SetSsrc(ssrc);
  packet->SetSequenceNumber(seq);
  packet->SetTimestamp(ts);
  packet->set_capture_time(clock.CurrentTime());
  packet->SetPayloadSize(bytes);
  packet->set_is_key_frame(key);
  packet->set_first_packet_of_frame(first);
  return packet;
}

bool Drain(PacingController& pacer, SimulatedClock& clock) {
  for (int i = 0; i < 10000 && pacer.QueueSizePackets(); ++i) {
    const auto next = pacer.NextSendTime();
    if (!next.IsFinite()) return false;
    if (next > clock.CurrentTime()) clock.AdvanceTime(next - clock.CurrentTime());
    pacer.ProcessPackets();
    if (pacer.QueueSizePackets()) clock.AdvanceTimeMilliseconds(1);
  }
  return pacer.QueueSizePackets() == 0;
}

bool PauseThenKeyframe(int pause_ms, bool enqueue_while_paused) {
  SimulatedClock clock(Timestamp::Millis(100000));
  Sender sender;
  Trials trials;
  PacingController pacer(&clock, &sender, trials);
  pacer.SetProbingEnabled(false);
  pacer.SetPacingRates(DataRate::KilobitsPerSec(6000), DataRate::Zero());
  pacer.Pause();
  clock.AdvanceTimeMilliseconds(pause_ms);
  if (!enqueue_while_paused) pacer.Resume();
  // First IDR in the failing real run was 62425 bytes / 54 RTP packets.
  constexpr int count = 54;
  for (int i = 0; i < count; ++i) {
    pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kVideo, 1,
                              i + 1, 9000, i == count - 1 ? 1078 : 1159,
                              true, i == 0));
  }
  const size_t queued = pacer.QueueSizePackets();
  if (enqueue_while_paused) pacer.Resume();
  const bool drained = Drain(pacer, clock);
  bool complete = drained && queued == count && sender.sent.size() == count;
  size_t bytes = 0;
  for (size_t i = 0; i < sender.sent.size(); ++i) {
    complete &= sender.sent[i].ssrc == 1 && sender.sent[i].seq == i + 1 &&
                sender.sent[i].timestamp == 9000;
    bytes += sender.sent[i].bytes;
  }
  complete &= bytes == 62505;  // 53 * 1159 + 1078.
  std::printf("%s pause=%dms enqueue=%s queued=%zu sent=%zu bytes=%zu\n",
              complete ? "PASS" : "FAIL", pause_ms,
              enqueue_while_paused ? "during-pause" : "after-resume",
              queued, sender.sent.size(), bytes);
  return complete;
}

bool StalledVideoPreservedUntilKeyframe() {
  SimulatedClock clock(Timestamp::Millis(100000));
  Sender sender;
  Trials trials;
  PacingController pacer(&clock, &sender, trials);
  pacer.SetProbingEnabled(false);
  pacer.SetPacingRates(DataRate::KilobitsPerSec(6000), DataRate::Zero());
  pacer.SetCongested(true);
  for (int i = 1; i <= 10; ++i) {
    pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kVideo, 1,
                              i, 9000, 1000, false, i == 1));
  }
  pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kRetransmission, 2,
                            100, 9000, 1000, false, false));
  pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kAudio, 3,
                            200, 0, 100, false, false));
  clock.AdvanceTimeMilliseconds(3000);
  // Another delta must not delete already-encoded reference fragments.
  pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kVideo, 1,
                            11, 18000, 1000, false, true));
  const bool deltas_preserved = pacer.QueueSizePackets() == 13;
  // A new IDR safely supersedes old media+RTX, but must not flush audio.
  for (int i = 0; i < 3; ++i) {
    pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kVideo, 1,
                              300 + i, 27000, 1000, true, i == 0));
  }
  const bool audio_preserved = pacer.QueueSizePackets() == 4;
  pacer.SetCongested(false);
  bool okay = deltas_preserved && audio_preserved && Drain(pacer, clock);
  bool got_audio = false;
  int key_packets = 0;
  for (const auto& packet : sender.sent) {
    if (packet.ssrc == 3 && packet.seq == 200) got_audio = true;
    else if (packet.ssrc == 1 && packet.timestamp == 27000) ++key_packets;
    else okay = false;
  }
  okay &= got_audio && key_packets == 3 && sender.sent.size() == 4;
  std::printf("%s stalled-reference-chain/new-IDR/audio-preservation\n",
              okay ? "PASS" : "FAIL");
  return okay;
}

// Drives the pacer the way TaskQueuePacedSender does: an enqueue processes at
// once (EnqueuePackets -> MaybeProcessPackets), and a delayed wakeup fires on
// the first multiple of timer_ms at or after the requested time. 15 ms models
// the default Windows timer; the field logs showed ~70 pacer wakeups/s.
void ProcessDue(PacingController& pacer, SimulatedClock& clock) {
  for (int i = 0; i < 1000; ++i) {
    const Timestamp next = pacer.NextSendTime();
    if (!next.IsFinite() || next > clock.CurrentTime()) return;
    pacer.ProcessPackets();
  }
}

struct LoadResult {
  int64_t p50_ms = 0, p95_ms = 0, max_ms = 0;
  int64_t audio_max_ms = 0;
  double sent_mbps = 0;
  size_t queued_end = 0, sent = 0;
};

LoadResult SustainedLoad(const std::string& extra, int timer_ms,
                         int frame_packets, int seconds,
                         double pacing_factor = 4.0, bool audio = false) {
  SimulatedClock clock(Timestamp::Millis(100000));
  Sender sender;
  sender.clock = &clock;
  Trials trials(extra);
  PacingController pacer(&clock, &sender, trials);
  pacer.SetProbingEnabled(false);
  const int64_t frame_bytes = int64_t{frame_packets} * 1150;
  // Production paces at 4x the target (WebRTC-Video-Pacing/factor:4.0).
  pacer.SetPacingRates(
      DataRate::BitsPerSec(static_cast<int64_t>(frame_bytes * 8 * 60 *
                                                pacing_factor)),
      DataRate::Zero());
  const int64_t start = clock.TimeInMicroseconds();
  const int64_t end = start + int64_t{seconds} * 1000000;
  const int64_t granularity = int64_t{timer_ms} * 1000;
  int64_t next_frame = start;
  int64_t next_audio = audio ? start : INT64_MAX;
  int64_t next_wake = INT64_MAX;
  uint16_t seq = 1;
  uint16_t audio_seq = 1;
  uint32_t ts = 9000;
  while (clock.TimeInMicroseconds() < end) {
    const int64_t event = std::min({next_frame, next_wake, next_audio});
    if (event > clock.TimeInMicroseconds())
      clock.AdvanceTimeMicroseconds(event - clock.TimeInMicroseconds());
    if (clock.TimeInMicroseconds() >= next_audio) {
      pacer.EnqueuePacket(Packet(clock, RtpPacketMediaType::kAudio, 3,
                                 audio_seq++, 0, 120, false, true));
      next_audio += 20000;
    }
    if (clock.TimeInMicroseconds() >= next_frame) {
      for (int i = 0; i < frame_packets; ++i) {
        auto packet = Packet(clock, RtpPacketMediaType::kVideo, 1, seq++, ts,
                             1150, false, i == 0);
        packet->SetMarker(i == frame_packets - 1);
        pacer.EnqueuePacket(std::move(packet));
      }
      ts += 1500;
      next_frame += 16667;
    }
    ProcessDue(pacer, clock);
    const Timestamp next = pacer.NextSendTime();
    next_wake = INT64_MAX;
    if (pacer.QueueSizePackets() > 0 && next.IsFinite()) {
      const int64_t want =
          std::max(next.us(), clock.TimeInMicroseconds() + 1000);
      next_wake = start + ((want - start + granularity - 1) / granularity) *
                              granularity;
    }
  }
  LoadResult result;
  std::vector<int64_t> delays;
  int64_t video_bytes = 0;
  for (const auto& s : sender.sent) {
    if (s.ssrc == 3) {
      result.audio_max_ms = std::max(result.audio_max_ms, s.queue_ms);
      continue;
    }
    delays.push_back(s.queue_ms);
    // Skip the first second while the queue fills.
    if (s.send_ms * 1000 >= start + 1000000) video_bytes += s.bytes;
  }
  result.sent_mbps = video_bytes * 8.0 / (seconds - 1) / 1e6;
  std::sort(delays.begin(), delays.end());
  if (!delays.empty()) {
    result.p50_ms = delays[delays.size() / 2];
    result.p95_ms = delays[delays.size() * 95 / 100];
    result.max_ms = delays.back();
  }
  result.queued_end = pacer.QueueSizePackets();
  result.sent = sender.sent.size();
  return result;
}

bool BacklogScenario(const char* name, const std::string& extra, int timer_ms,
                     int frame_packets, bool expect_bounded) {
  const LoadResult r = SustainedLoad(extra, timer_ms, frame_packets, 3);
  const double mbps = frame_packets * 1150.0 * 8 * 60 / 1e6;
  // Bounded: no packet waits more than ~3 frame intervals and the queue holds
  // less than two frames at the end.
  const bool bounded = r.max_ms <= 50 &&
                       r.queued_end < static_cast<size_t>(2 * frame_packets);
  const bool ok = expect_bounded ? bounded : true;
  std::printf("%s %-28s timer=%2dms load=%4.1fMbps queue_ms p50=%lld p95=%lld "
              "max=%lld end_queue=%zu sent=%zu%s\n",
              ok ? "PASS" : "FAIL", name, timer_ms, mbps,
              static_cast<long long>(r.p50_ms), static_cast<long long>(r.p95_ms),
              static_cast<long long>(r.max_ms), r.queued_end, r.sent,
              expect_bounded ? "" : (bounded ? " (reference)" : " (reference: grows)"));
  return ok;
}

// Informational: pacing below the encoder output. Neither the realtime pacer
// (it clamps media debt after each frame) nor the stock pacer with its 40 ms
// burst window holds this rate exactly, so the rows document each variant.
// Link capacity is enforced by GCC lowering the encoder target (production
// paces at 4x that target) and by the congestion window, which stops the
// backlog bypass entirely.
bool UnderprovisionedPacingReference() {
  // Pacing at 0.6x the 13 Mbps input: expect ~7.9 Mbps out.
  const LoadResult r = SustainedLoad("", 15, 24, 4, /*pacing_factor=*/0.6);
  const double limit = 24 * 1150.0 * 8 * 60 * 0.6 / 1e6;
  for (const char* variant : {",backlog_ms:0", "off"}) {
    const LoadResult ref = SustainedLoad(variant, 15, 24, 4, 0.6);
    std::printf("INFO  underprovisioned-link reference=%s sent=%.1fMbps "
                "max_queue_ms=%lld\n",
                variant, ref.sent_mbps, static_cast<long long>(ref.max_ms));
  }
  std::printf("INFO  underprovisioned-link fixed sent=%.1fMbps pacing=%.1fMbps "
              "max_queue_ms=%lld\n",
              r.sent_mbps, limit, static_cast<long long>(r.max_ms));
  return true;
}

// The congestion window must still stop the drain: the bypass only applies
// while the transport is allowed to send.
bool CongestionStopsBacklogDrain() {
  SimulatedClock clock(Timestamp::Millis(100000));
  Sender sender;
  sender.clock = &clock;
  Trials trials;
  PacingController pacer(&clock, &sender, trials);
  pacer.SetProbingEnabled(false);
  pacer.SetPacingRates(DataRate::KilobitsPerSec(40000), DataRate::Zero());
  uint16_t seq = 1;
  for (int frame = 0; frame < 6; ++frame) {
    for (int i = 0; i < 20; ++i) {
      auto packet = Packet(clock, RtpPacketMediaType::kVideo, 1, seq++,
                           9000 + frame * 1500, 1150, false, i == 0);
      packet->SetMarker(i == 19);
      pacer.EnqueuePacket(std::move(packet));
    }
  }
  pacer.SetCongested(true);
  clock.AdvanceTimeMilliseconds(100);  // backlog is now 100 ms old
  for (int i = 0; i < 50; ++i) {
    ProcessDue(pacer, clock);
    clock.AdvanceTimeMilliseconds(1);
  }
  const size_t sent_while_congested = sender.sent.size();
  pacer.SetCongested(false);
  for (int i = 0; i < 200 && pacer.QueueSizePackets(); ++i) {
    ProcessDue(pacer, clock);
    clock.AdvanceTimeMilliseconds(1);
  }
  const bool ok = sent_while_congested == 0 && sender.sent.size() == 120;
  std::printf("%s congestion-window-stops-drain sent_while_congested=%zu "
              "sent_after=%zu\n",
              ok ? "PASS" : "FAIL", sent_while_congested, sender.sent.size());
  return ok;
}

// Audio keeps priority while the video queue is being drained.
bool AudioNotStarvedDuringBacklog() {
  const LoadResult r = SustainedLoad("", 15, 64, 3, 4.0, /*audio=*/true);
  const bool ok = r.audio_max_ms <= 20 && r.max_ms <= 50;
  std::printf("%s audio-during-35Mbps-backlog audio_max_ms=%lld video_max_ms=%lld\n",
              ok ? "PASS" : "FAIL", static_cast<long long>(r.audio_max_ms),
              static_cast<long long>(r.max_ms));
  return ok;
}

// Without a backlog the realtime pacer must still split one large frame into
// spaced packet groups (the reason it exists).
bool SmoothingPreservedWithoutBacklog() {
  SimulatedClock clock(Timestamp::Millis(100000));
  Sender sender;
  sender.clock = &clock;
  Trials trials;
  PacingController pacer(&clock, &sender, trials);
  pacer.SetProbingEnabled(false);
  pacer.SetPacingRates(DataRate::KilobitsPerSec(40000), DataRate::Zero());
  constexpr int count = 52;  // ~60 KB, about 2.5 packet groups
  for (int i = 0; i < count; ++i) {
    auto packet = Packet(clock, RtpPacketMediaType::kVideo, 1, i + 1, 9000,
                         1150, true, i == 0);
    packet->SetMarker(i == count - 1);
    pacer.EnqueuePacket(std::move(packet));
  }
  for (int i = 0; i < 200 && pacer.QueueSizePackets(); ++i) {
    ProcessDue(pacer, clock);
    clock.AdvanceTimeMilliseconds(1);
  }
  std::vector<int64_t> bursts;
  for (const auto& s : sender.sent)
    if (bursts.empty() || bursts.back() != s.send_ms) bursts.push_back(s.send_ms);
  const int64_t span = sender.sent.empty()
                           ? 0
                           : sender.sent.back().send_ms - sender.sent.front().send_ms;
  const bool ok = sender.sent.size() == count && bursts.size() >= 3 &&
                  span >= 4 && span <= 19;
  std::printf("%s single-frame-smoothing groups=%zu span_ms=%lld sent=%zu\n",
              ok ? "PASS" : "FAIL", bursts.size(),
              static_cast<long long>(span), sender.sent.size());
  return ok;
}

}  // namespace

int main() {
  bool okay = true;
  for (int pause : {0, 250, 500, 882, 3000}) {
    okay = PauseThenKeyframe(pause, false) && okay;
  }
  okay = PauseThenKeyframe(882, true) && okay;
  okay = StalledVideoPreservedUntilKeyframe() && okay;
  okay = SmoothingPreservedWithoutBacklog() && okay;
  // Reference rows: the pre-fix behaviour (backlog bypass disabled).
  BacklogScenario("prefix/coarse-timer/13Mbps", ",backlog_ms:0", 15, 24, false);
  BacklogScenario("prefix/coarse-timer/22Mbps", ",backlog_ms:0", 15, 40, false);
  okay = BacklogScenario("fine-timer/13Mbps", "", 1, 24, true) && okay;
  okay = BacklogScenario("fine-timer/22Mbps", "", 1, 40, true) && okay;
  okay = BacklogScenario("coarse-timer/5Mbps", "", 15, 9, true) && okay;
  okay = BacklogScenario("coarse-timer/13Mbps", "", 15, 24, true) && okay;
  okay = BacklogScenario("coarse-timer/22Mbps", "", 15, 40, true) && okay;
  okay = BacklogScenario("coarse-timer/35Mbps", "", 15, 64, true) && okay;
  UnderprovisionedPacingReference();
  okay = CongestionStopsBacklogDrain() && okay;
  okay = AudioNotStarvedDuringBacklog() && okay;
  std::printf("RESULT: %s (16 production pacer scenarios)\n",
              okay ? "PASS" : "FAIL");
  return okay ? 0 : 1;
}
