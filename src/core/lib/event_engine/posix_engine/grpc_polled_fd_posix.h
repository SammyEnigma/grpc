// Copyright 2023 The gRPC Authors
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

#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_GRPC_POLLED_FD_POSIX_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_GRPC_POLLED_FD_POSIX_H

#include <grpc/event_engine/event_engine.h>
#include <grpc/support/port_platform.h>

#include <memory>

#include "absl/base/thread_annotations.h"
#include "src/core/lib/event_engine/posix_engine/file_descriptor_collection.h"
#include "src/core/lib/event_engine/posix_engine/posix_interface.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/util/sync.h"

#if GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET_ARES_EV_DRIVER)

// IWYU pragma: no_include <ares_build.h>

#include <ares.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <string>
#include <unordered_set>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "src/core/lib/event_engine/grpc_polled_fd.h"
#include "src/core/lib/event_engine/posix_engine/event_poller.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine_closure.h"

namespace grpc_event_engine::experimental {

class GrpcPolledFdPosix : public GrpcPolledFd {
 public:
  GrpcPolledFdPosix(ares_socket_t as, EventHandle* handle)
      : name_(absl::StrCat("c-ares fd: ", static_cast<int>(as))),
        as_(as),
        handle_(handle) {}

  ~GrpcPolledFdPosix() override {
    // c-ares library will close the fd. This fd may be picked up immediately by
    // another thread and should not be closed by the following OrphanHandle.
    FileDescriptor phony_release_fd;
    handle_->OrphanHandle(/*on_done=*/nullptr, &phony_release_fd,
                          "c-ares query finished");
  }

  void RegisterForOnReadableLocked(
      absl::AnyInvocable<void(absl::Status)> read_closure) override {
    handle_->NotifyOnRead(new PosixEngineClosure(std::move(read_closure),
                                                 /*is_permanent=*/false));
  }

  void RegisterForOnWriteableLocked(
      absl::AnyInvocable<void(absl::Status)> write_closure) override {
    handle_->NotifyOnWrite(new PosixEngineClosure(std::move(write_closure),
                                                  /*is_permanent=*/false));
  }

  bool IsFdStillReadableLocked() override {
    size_t bytes_available = 0;
    return ioctl(as_, FIONREAD, &bytes_available) == 0 && bytes_available > 0;
  }

  bool ShutdownLocked(absl::Status error) override {
    handle_->ShutdownHandle(error);
    return true;
  }

  ares_socket_t GetWrappedAresSocketLocked() override { return as_; }

  const char* GetName() const override { return name_.c_str(); }

  bool IsCurrent() const override {
    return handle_->Poller()->posix_interface().generation() ==
           handle_->WrappedFd().generation();
  }

 private:
  const std::string name_;
  const ares_socket_t as_;
  EventHandle* handle_;
};

class GrpcPolledFdFactoryPosix : public GrpcPolledFdFactory {
 public:
  explicit GrpcPolledFdFactoryPosix(PosixEventPoller* poller)
      : poller_(poller) {}

  ~GrpcPolledFdFactoryPosix() override {
    grpc_core::MutexLock lock(&mu_);
    for (auto& fd : owned_fds_) {
      close(fd);
    }
  }

  void Initialize(grpc_core::Mutex*, EventEngine*) override {}

  std::unique_ptr<GrpcPolledFd> NewGrpcPolledFdLocked(
      ares_socket_t as) override {
    grpc_core::MutexLock lock(&mu_);
    owned_fds_.insert(as);
    FileDescriptor fd(as, poller_->posix_interface().generation());
    return std::make_unique<GrpcPolledFdPosix>(
        as,
        poller_->CreateHandle(fd, "c-ares socket", poller_->CanTrackErrors()));
  }

  void ConfigureAresChannelLocked(ares_channel channel) override {
    ares_set_socket_functions(channel, &kSockFuncs, this);
    ares_set_socket_configure_callback(
        channel, &GrpcPolledFdFactoryPosix::ConfigureSocket, this);
  }

  std::unique_ptr<GrpcPolledFdFactory> NewEmptyInstance() const override {
    return std::make_unique<GrpcPolledFdFactoryPosix>(poller_);
  }

 private:
  /// Overridden socket API for c-ares
  static ares_socket_t Socket(int af, int type, int protocol,
                              void* /*user_data*/) {
    return socket(af, type, protocol);
  }

  /// Overridden connect API for c-ares
  static int Connect(ares_socket_t as, const struct sockaddr* target,
                     ares_socklen_t target_len, void* /*user_data*/) {
    return connect(as, target, target_len);
  }

  /// Overridden writev API for c-ares
  static ares_ssize_t WriteV(ares_socket_t as, const struct iovec* iov,
                             int iovec_count, void* /*user_data*/) {
    return writev(as, iov, iovec_count);
  }

  /// Overridden recvfrom API for c-ares
  static ares_ssize_t RecvFrom(ares_socket_t as, void* data, size_t data_len,
                               int flags, struct sockaddr* from,
                               ares_socklen_t* from_len, void* /*user_data*/) {
    return recvfrom(as, data, data_len, flags, from, from_len);
  }

  /// Overridden close API for c-ares
  static int Close(ares_socket_t as, void* user_data) {
    GrpcPolledFdFactoryPosix* self =
        static_cast<GrpcPolledFdFactoryPosix*>(user_data);
    grpc_core::MutexLock lock(&self->mu_);
    if (self->owned_fds_.find(as) == self->owned_fds_.end()) {
      // c-ares owns this fd, grpc has never seen it
      return close(as);
    }
    return 0;
  }

  /// Because we're using socket API overrides, c-ares won't
  /// perform its typical configuration on the socket. See
  /// https://github.com/c-ares/c-ares/blob/bad62225b7f6b278b92e8e85a255600b629ef517/src/lib/ares_process.c#L1018.
  /// So we use the configure socket callback override and copy default
  /// settings that c-ares would normally apply on posix platforms:
  ///   - non-blocking
  ///   - cloexec flag
  ///   - disable nagle
  static int ConfigureSocket(ares_socket_t as, int type,
                             void* polled_fd_factory) {
    auto& posix_interface =
        static_cast<GrpcPolledFdFactoryPosix*>(polled_fd_factory)
            ->poller_->posix_interface();
    return posix_interface.ConfigureSocket({as, posix_interface.generation()},
                                           type);
  }

  const struct ares_socket_functions kSockFuncs = {
      &GrpcPolledFdFactoryPosix::Socket /* socket */,
      &GrpcPolledFdFactoryPosix::Close /* close */,
      &GrpcPolledFdFactoryPosix::Connect /* connect */,
      &GrpcPolledFdFactoryPosix::RecvFrom /* recvfrom */,
      &GrpcPolledFdFactoryPosix::WriteV /* writev */,
  };

  PosixEventPoller* poller_;
  grpc_core::Mutex mu_;
  // fds that are used/owned by grpc - we (grpc) will close them rather than
  // c-ares
  std::unordered_set<ares_socket_t> owned_fds_ ABSL_GUARDED_BY(mu_);
};

}  // namespace grpc_event_engine::experimental

#endif  // GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET_ARES_EV_DRIVER)
#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_GRPC_POLLED_FD_POSIX_H
