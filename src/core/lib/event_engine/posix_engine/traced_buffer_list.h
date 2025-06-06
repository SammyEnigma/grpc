// Copyright 2022 The gRPC Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_TRACED_BUFFER_LIST_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_TRACED_BUFFER_LIST_H

#include <grpc/support/port_platform.h>
#include <grpc/support/time.h>
#include <stdint.h>

#include <optional>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "src/core/lib/event_engine/posix_engine/internal_errqueue.h"
#include "src/core/lib/event_engine/posix_engine/posix_interface.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/util/sync.h"

namespace grpc_event_engine::experimental {

struct ConnectionMetrics {  // Delivery rate in Bytes/s.
  std::optional<uint64_t> delivery_rate;
  // If the delivery rate is limited by the application, this is set to true.
  std::optional<bool> is_delivery_rate_app_limited;
  // Total packets retransmitted.
  std::optional<uint32_t> packet_retx;
  // Total packets retransmitted spuriously. This metric is smaller than or
  // equal to packet_retx.
  std::optional<uint32_t> packet_spurious_retx;
  // Total packets sent.
  std::optional<uint32_t> packet_sent;
  // Total packets delivered.
  std::optional<uint32_t> packet_delivered;
  // Total packets delivered with ECE marked. This metric is smaller than or
  // equal to packet_delivered.
  std::optional<uint32_t> packet_delivered_ce;
  // Total bytes lost so far.
  std::optional<uint64_t> data_retx;
  // Total bytes sent so far.
  std::optional<uint64_t> data_sent;
  // Total bytes in write queue but not sent.
  std::optional<uint64_t> data_notsent;
  // Pacing rate of the connection in Bps
  std::optional<uint64_t> pacing_rate;
  // Minimum RTT observed in usec.
  std::optional<uint32_t> min_rtt;
  // Smoothed RTT in usec
  std::optional<uint32_t> srtt;
  // Send congestion window.
  std::optional<uint32_t> congestion_window;
  // Slow start threshold in packets.
  std::optional<uint32_t> snd_ssthresh;
  // Maximum degree of reordering (i.e., maximum number of packets reodered)
  // on the connection.
  std::optional<uint32_t> reordering;
  // Represents the number of recurring retransmissions of the first sequence
  // that is not acknowledged yet.
  std::optional<uint8_t> recurring_retrans;
  // The cumulative time (in usec) that the transport protocol was busy
  // sending data.
  std::optional<uint64_t> busy_usec;
  // The cumulative time (in usec) that the transport protocol was limited by
  // the receive window size.
  std::optional<uint64_t> rwnd_limited_usec;
  // The cumulative time (in usec) that the transport protocol was limited by
  // the send buffer size.
  std::optional<uint64_t> sndbuf_limited_usec;
};

struct BufferTimestamp {
  gpr_timespec time;
  ConnectionMetrics metrics;  // Metrics collected with this timestamp
};

struct Timestamps {
  BufferTimestamp sendmsg_time;
  BufferTimestamp scheduled_time;
  BufferTimestamp sent_time;
  BufferTimestamp acked_time;

  uint32_t byte_offset;  // byte offset relative to the start of the RPC

#ifdef GRPC_LINUX_ERRQUEUE
  tcp_info info;  // tcp_info collected on sendmsg
#endif            // GRPC_LINUX_ERRQUEUE
};

// TracedBuffer is a class to keep track of timestamps for a specific buffer in
// the TCP layer. We are only tracking timestamps for Linux kernels and hence
// this class would only be used by Linux platforms. For all other platforms,
// TracedBuffer would be an empty class.
// The timestamps collected are according to Timestamps declared above A
// TracedBuffer list is kept track of using the head element of the list. If
// *the head element of the list is nullptr, then the list is empty.
#ifdef GRPC_LINUX_ERRQUEUE

class TracedBufferList {
 public:
  TracedBufferList() = default;
  ~TracedBufferList() = default;
  // Add a new entry in the TracedBuffer list pointed to by head. Also saves
  // sendmsg_time with the current timestamp.
  void AddNewEntry(int32_t seq_no, EventEnginePosixInterface* posix_interface,
                   const FileDescriptor& fd, void* arg);
  // Processes a received timestamp based on sock_extended_err and
  // scm_timestamping structures. It will invoke the timestamps callback if the
  // timestamp type is SCM_TSTAMP_ACK.
  void ProcessTimestamp(struct sock_extended_err* serr,
                        struct cmsghdr* opt_stats,
                        struct scm_timestamping* tss);
  // The Size() operation is slow and is used only in tests.
  int Size() {
    grpc_core::MutexLock lock(&mu_);
    int size = 0;
    TracedBuffer* curr = head_;
    while (curr) {
      ++size;
      curr = curr->next_;
    }
    return size;
  }
  // Cleans the list by calling the callback for each traced buffer in the list
  // with timestamps that it has.
  void Shutdown(void* /*remaining*/, absl::Status /*shutdown_err*/);

 private:
  class TracedBuffer {
   public:
    TracedBuffer(uint32_t seq_no, void* arg) : seq_no_(seq_no), arg_(arg) {}
    // Returns true if the TracedBuffer is considered stale at the given
    // timestamp.
    bool Finished(gpr_timespec ts);

   private:
    friend class TracedBufferList;
    gpr_timespec last_timestamp_;
    TracedBuffer* next_ = nullptr;
    uint32_t seq_no_;  // The sequence number for the last byte in the buffer
    void* arg_;        // The arg to pass to timestamps_callback
    Timestamps ts_;    // The timestamps corresponding to this buffer
  };
  grpc_core::Mutex mu_;
  // TracedBuffers are ordered by sequence number and would need to be processed
  // in a FIFO order starting with the smallest sequence number. To enable this,
  // they are stored in a singly linked with head and tail pointers which allows
  // easy appends and forward iteration operations.
  TracedBuffer* head_ = nullptr;
  TracedBuffer* tail_ = nullptr;
};

#else   // GRPC_LINUX_ERRQUEUE
// TracedBufferList implementation is a no-op for this platform.
class TracedBufferList {
 public:
  void AddNewEntry(int32_t /*seq_no*/, int /*fd*/, void* /*arg*/) {}
  void ProcessTimestamp(struct sock_extended_err* /*serr*/,
                        struct cmsghdr* /*opt_stats*/,
                        struct scm_timestamping* /*tss*/) {}
  int Size() { return 0; }
  void Shutdown(void* /*remaining*/, absl::Status /*shutdown_err*/) {}
};
#endif  // GRPC_LINUX_ERRQUEUE

// Sets the callback function to call when timestamps for a write are collected.
// This is expected to be called atmost once.
void TcpSetWriteTimestampsCallback(
    absl::AnyInvocable<void(void*, Timestamps*, absl::Status)>);

}  // namespace grpc_event_engine::experimental

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_TRACED_BUFFER_LIST_H
