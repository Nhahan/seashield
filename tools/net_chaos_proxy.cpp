#include "tools/net_chaos_proxy.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "core/logger.h"
#include "net/socket_util.h"

namespace seashield::tools {
namespace {

constexpr std::size_t kRecvBufBytes = 64 * 1024;

}  // namespace

ChaosProxy::ChaosProxy(const ChaosConfig& config) : config_(config), rng_(config.seed) {}

ChaosProxy::~ChaosProxy() { stop(); }

std::uint64_t ChaosProxy::addr_key(const sockaddr_in& addr) {
  return (static_cast<std::uint64_t>(addr.sin_addr.s_addr) << 16) | addr.sin_port;
}

double ChaosProxy::now_s() const {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool ChaosProxy::init() {
  listen_fd_ = net::create_udp_socket(config_.listen_port);
  if (!listen_fd_.valid()) {
    return false;
  }
  bound_port_ = net::local_port(listen_fd_.get());

  upstream_addr_ = {};
  upstream_addr_.sin_family = AF_INET;
  upstream_addr_.sin_port = htons(config_.upstream_port);
  if (::inet_pton(AF_INET, config_.upstream_host.c_str(), &upstream_addr_.sin_addr) != 1) {
    return false;
  }

  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {
    return false;
  }
  stop_read_ = UniqueFd(pipe_fds[0]);
  stop_write_ = UniqueFd(pipe_fds[1]);
  return net::set_nonblocking_cloexec(stop_read_.get());
}

void ChaosProxy::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  if (stop_write_.valid()) {
    const char byte = 1;
    [[maybe_unused]] const ssize_t n = ::write(stop_write_.get(), &byte, 1);
  }
}

void ChaosProxy::schedule(int fd, bool to_client, const sockaddr_in& client_addr,
                          std::vector<std::uint8_t> bytes) {
  if (rng_.next_double() < config_.loss) {
    dropped_.fetch_add(1);
    return;
  }
  const double base = now_s();
  const bool duplicate = rng_.next_double() < config_.dup;
  const double due = base + config_.delay_s + rng_.uniform(0.0, config_.jitter_s);
  if (duplicate) {
    duplicated_.fetch_add(1);
    const double dup_due = base + config_.delay_s + rng_.uniform(0.0, config_.jitter_s);
    pending_.push({dup_due, next_order_++, fd, to_client, client_addr, bytes});
  }
  pending_.push({due, next_order_++, fd, to_client, client_addr, std::move(bytes)});
}

void ChaosProxy::flush_due() {
  const double now = now_s();
  while (!pending_.empty() && pending_.top().due_s <= now) {
    const Scheduled& item = pending_.top();
    if (item.to_client) {
      ::sendto(item.fd, item.bytes.data(), item.bytes.size(), 0,
               reinterpret_cast<const sockaddr*>(&item.client_addr), sizeof(item.client_addr));
    } else {
      ::send(item.fd, item.bytes.data(), item.bytes.size(), 0);
    }
    forwarded_.fetch_add(1);
    pending_.pop();
  }
}

void ChaosProxy::on_listen_readable() {
  std::uint8_t buf[kRecvBufBytes];
  for (;;) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(listen_fd_.get(), buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;  // EAGAIN: drained.
    }
    const std::uint64_t key = addr_key(from);
    auto it = clients_.find(key);
    if (it == clients_.end()) {
      if (clients_.size() >= config_.max_clients) {
        dropped_.fetch_add(1);
        continue;
      }
      // First datagram from this client: open its private upstream socket so
      // the server's replies can be routed back to exactly this client.
      UniqueFd up = net::create_udp_socket(0);
      if (!up.valid() ||
          ::connect(up.get(), reinterpret_cast<const sockaddr*>(&upstream_addr_),
                    sizeof(upstream_addr_)) != 0) {
        SS_LOG_WARN("chaos proxy: failed to open upstream socket (errno=%d)", errno);
        continue;
      }
      it = clients_.emplace(key, Client{from, std::move(up)}).first;
      SS_LOG_DEBUG("chaos proxy: new client, %zu total", clients_.size());
    }
    schedule(it->second.upstream_fd.get(), false, {},
             std::vector<std::uint8_t>(buf, buf + static_cast<std::size_t>(n)));
  }
}

void ChaosProxy::on_upstream_readable(std::uint64_t client_key) {
  auto it = clients_.find(client_key);
  if (it == clients_.end()) {
    return;
  }
  std::uint8_t buf[kRecvBufBytes];
  for (;;) {
    const ssize_t n = ::recv(it->second.upstream_fd.get(), buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    schedule(listen_fd_.get(), true, it->second.addr,
             std::vector<std::uint8_t>(buf, buf + static_cast<std::size_t>(n)));
  }
}

void ChaosProxy::run() {
  while (!stopping_.load()) {
    std::vector<pollfd> fds;
    std::vector<std::uint64_t> keys;  // Parallel to fds from index 2 on.
    fds.push_back({stop_read_.get(), POLLIN, 0});
    fds.push_back({listen_fd_.get(), POLLIN, 0});
    for (const auto& [key, client] : clients_) {
      fds.push_back({client.upstream_fd.get(), POLLIN, 0});
      keys.push_back(key);
    }

    int timeout_ms = 100;
    if (!pending_.empty()) {
      const double wait = (pending_.top().due_s - now_s()) * 1000.0;
      timeout_ms = std::clamp(static_cast<int>(wait) + 1, 0, 100);
    }
    const int ready = ::poll(fds.data(), fds.size(), timeout_ms);
    if (ready < 0 && errno != EINTR) {
      SS_LOG_ERROR("chaos proxy: poll failed (errno=%d)", errno);
      return;
    }
    if (ready > 0) {
      if ((fds[0].revents & POLLIN) != 0) {
        break;  // stop() signalled.
      }
      if ((fds[1].revents & POLLIN) != 0) {
        on_listen_readable();
      }
      for (std::size_t i = 2; i < fds.size(); ++i) {
        if ((fds[i].revents & POLLIN) != 0) {
          on_upstream_readable(keys[i - 2]);
        }
      }
    }
    flush_due();
  }
}

}  // namespace seashield::tools
