// net_bridge.cpp - worker thread that owns all TCP/UDP sockets for the page.
#include "net_bridge.h"

#include "util.h"

#include <ws2tcpip.h>

#include <chrono>
#include <sstream>
#include <vector>

namespace {

constexpr size_t kMaxConnections = 60;   // stay well inside FD_SETSIZE (64)
constexpr int    kRecvBuffer = 65536;
constexpr ULONGLONG kDnsTtlMs = 120000;  // 2 minutes

std::string WsaErrorText(int code) {
  wchar_t* buffer = nullptr;
  DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, (DWORD)code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buffer, 0, nullptr);
  std::wstring text;
  if (n && buffer) {
    text.assign(buffer, n);
    LocalFree(buffer);
  }
  text = util::Trim(text);
  if (text.empty()) text = L"unknown error";
  std::wstring full = text + L" (" + std::to_wstring(code) + L")";
  return util::WideToUtf8(full);
}

uint16_t SockPort(const sockaddr_storage& ss) {
  if (ss.ss_family == AF_INET) return ntohs(((const sockaddr_in*)&ss)->sin_port);
  if (ss.ss_family == AF_INET6) return ntohs(((const sockaddr_in6*)&ss)->sin6_port);
  return 0;
}

}  // namespace

NetBridge::~NetBridge() { Stop(); }

void NetBridge::Start(HWND hwnd, UINT msg) {
  if (m_running) return;
  m_hwnd = hwnd;
  m_msg = msg;
  m_stop = false;
  m_running = true;
  m_thread = std::thread(&NetBridge::Worker, this);
}

void NetBridge::Stop() {
  if (!m_running) return;
  m_stop = true;
  m_cv.notify_all();
  if (m_thread.joinable()) m_thread.join();
  m_running = false;
  m_notifyPending = false;
}

void NetBridge::Send(const std::string& line) {
  if (!m_running) return;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back(line);
  }
  m_cv.notify_one();
}

void NetBridge::Emit(const std::string& line) {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(line);
    if (!m_notifyPending) {
      m_notifyPending = true;
      notify = true;
    }
  }
  if (notify && m_hwnd) PostMessageW(m_hwnd, m_msg, 0, 0);
}

std::string NetBridge::Drain() {
  std::deque<std::string> events;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    events.swap(m_events);
    m_notifyPending = false;
  }
  std::string out;
  for (const std::string& e : events) {
    if (!out.empty()) out.push_back('\n');
    out += e;
  }
  return out;
}

void NetBridge::CloseConn(std::unordered_map<std::string, Conn>::iterator it,
                          std::unordered_map<std::string, Conn>& conns, const std::string& event,
                          const std::string& detail) {
  Conn& c = it->second;
  if (c.sock != INVALID_SOCKET) {
    closesocket(c.sock);
    c.sock = INVALID_SOCKET;
  }
  if (c.open) {
    c.open = false;
    m_openCount.fetch_sub(1);
  }
  std::string line = event + "|" + util::PercentEncode(c.id);
  if (!detail.empty()) line += "|" + detail;
  Emit(line);
  conns.erase(it);
}

bool NetBridge::Resolve(const std::string& host, uint16_t port, int sockType,
                        sockaddr_storage& out, int& outLen) {
  const std::string key = host + "#" + std::to_string(sockType);
  ULONGLONG now = GetTickCount64();
  auto cached = m_dnsCache.find(key);
  if (cached != m_dnsCache.end() && now - cached->second.stamp < kDnsTtlMs) {
    out = cached->second.addr;
    outLen = cached->second.len;
    return true;
  }

  // A literal IP address (IPv4 or IPv6) skips DNS entirely.
  {
    sockaddr_storage direct{};
    int directLen = sizeof(direct);
    std::wstring whost = util::Utf8ToWide(host);
    if (InetPtonW(AF_INET, whost.c_str(), &((sockaddr_in*)&direct)->sin_addr) == 1) {
      direct.ss_family = AF_INET;
      ((sockaddr_in*)&direct)->sin_port = htons(port);
      directLen = sizeof(sockaddr_in);
    } else if (InetPtonW(AF_INET6, whost.c_str(), &((sockaddr_in6*)&direct)->sin6_addr) == 1) {
      direct.ss_family = AF_INET6;
      ((sockaddr_in6*)&direct)->sin6_port = htons(port);
      directLen = sizeof(sockaddr_in6);
    } else {
      directLen = 0;
    }
    if (directLen) {
      out = direct;
      outLen = directLen;
      return true;
    }
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = sockType;
  hints.ai_protocol = (sockType == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP;
  addrinfo* result = nullptr;
  std::string portText = std::to_string(port);
  if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || !result) {
    if (result) freeaddrinfo(result);
    return false;
  }
  outLen = (int)result->ai_addrlen;
  if (outLen > (int)sizeof(sockaddr_storage)) outLen = (int)sizeof(sockaddr_storage);
  memcpy(&out, result->ai_addr, (size_t)outLen);
  freeaddrinfo(result);

  DnsEntry entry;
  entry.addr = out;
  entry.len = outLen;
  entry.stamp = now;
  m_dnsCache[key] = entry;
  return true;
}

std::string NetBridge::AddrToString(const sockaddr* sa) {
  char host[NI_MAXHOST] = {0};
  if (sa->sa_family == AF_INET) {
    InetNtopA(AF_INET, &((const sockaddr_in*)sa)->sin_addr, host, NI_MAXHOST);
  } else if (sa->sa_family == AF_INET6) {
    InetNtopA(AF_INET6, &((const sockaddr_in6*)sa)->sin6_addr, host, NI_MAXHOST);
  }
  return host;
}

void NetBridge::FlushOutbox(Conn& c, bool& dead) {
  dead = false;
  while (!c.outbox.empty()) {
    std::vector<uint8_t>& buf = c.outbox.front();
    int remaining = (int)(buf.size() - c.outOffset);
    int sent = send(c.sock, (const char*)buf.data() + c.outOffset, remaining, 0);
    if (sent > 0) {
      c.outOffset += (size_t)sent;
      if (c.outOffset >= buf.size()) {
        c.outbox.pop_front();
        c.outOffset = 0;
      }
      continue;
    }
    if (sent == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) break;
      dead = true;
    }
    break;
  }
  c.wantWrite = !c.outbox.empty();
}

void NetBridge::HandleCommand(const std::string& line,
                              std::unordered_map<std::string, Conn>& conns) {
  std::vector<std::string> parts = util::Split(line, '|');
  if (parts.size() < 2) return;

  const std::string& cmd = parts[0];
  const std::string id = util::PercentDecode(parts[1]);
  const bool isUdp = cmd.rfind("udp", 0) == 0;
  auto fail = [&](const std::string& message) {
    Emit(std::string(isUdp ? "udp.error|" : "tcp.error|") + util::PercentEncode(id) + "|" +
         util::PercentEncode(message));
  };

  if (cmd == "tcp.connect") {
    if (parts.size() < 4) return;
    if (conns.size() >= kMaxConnections) return fail("connection limit reached");
    if (conns.count(id)) return fail("duplicate connection id");
    const std::string host = util::PercentDecode(parts[2]);
    const uint16_t port = (uint16_t)atoi(parts[3].c_str());

    sockaddr_storage addr{};
    int addrLen = 0;
    if (!Resolve(host, port, SOCK_STREAM, addr, addrLen)) return fail("could not resolve host");
    SOCKET s = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return fail("could not create socket: " + WsaErrorText(WSAGetLastError()));
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    int bufferSize = 1 << 20;  // 1 MB socket buffers
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferSize, sizeof(bufferSize));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&bufferSize, sizeof(bufferSize));

    int rc = connect(s, (const sockaddr*)&addr, addrLen);
    int err = (rc == SOCKET_ERROR) ? WSAGetLastError() : 0;
    if (rc == SOCKET_ERROR && err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
      closesocket(s);
      return fail("connect failed: " + WsaErrorText(err));
    }

    Conn c;
    c.id = id;
    c.sock = s;
    c.connecting = (rc == SOCKET_ERROR);
    conns.emplace(id, std::move(c));
    if (!conns[id].connecting) {
      conns[id].open = true;
      m_openCount.fetch_add(1);
      Emit("tcp.open|" + util::PercentEncode(id));
    }
    return;
  }

  if (cmd == "tcp.send") {
    auto it = conns.find(id);
    if (it == conns.end()) return fail("no such connection");
    std::vector<uint8_t> data;
    if (parts.size() >= 3) util::Base64Decode(parts[2], data);
    if (data.empty()) return;
    Conn& c = it->second;
    bool dead = false;
    if (!c.connecting && c.outbox.empty()) {
      int sent = send(c.sock, (const char*)data.data(), (int)data.size(), 0);
      if (sent == (int)data.size()) return;
      if (sent > 0) {
        c.outbox.push_back(std::vector<uint8_t>(data.begin() + sent, data.end()));
      } else {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
          return CloseConn(it, conns, "tcp.error", util::PercentEncode("send failed: " + WsaErrorText(err)));
        }
        c.outbox.push_back(std::move(data));
      }
    } else {
      c.outbox.push_back(std::move(data));
    }
    c.wantWrite = true;
    (void)dead;
    return;
  }

  if (cmd == "tcp.close") {
    auto it = conns.find(id);
    if (it == conns.end()) return;
    CloseConn(it, conns, "tcp.close");
    return;
  }

  if (cmd == "udp.open") {
    if (conns.count(id)) return fail("duplicate connection id");
    if (conns.size() >= kMaxConnections) return fail("connection limit reached");
    uint16_t bindPort = (parts.size() >= 3) ? (uint16_t)atoi(parts[2].c_str()) : 0;
    sockaddr_storage any{};
    int anyLen = 0;
    Resolve("0.0.0.0", bindPort, SOCK_DGRAM, any, anyLen);
    if (!anyLen) {
      ((sockaddr_in*)&any)->sin_family = AF_INET;
      ((sockaddr_in*)&any)->sin_addr.s_addr = INADDR_ANY;
      ((sockaddr_in*)&any)->sin_port = htons(bindPort);
      anyLen = sizeof(sockaddr_in);
    }
    SOCKET s = socket(any.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return fail("could not create udp socket");
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    int bufferSize = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufferSize, sizeof(bufferSize));
    if (bind(s, (const sockaddr*)&any, anyLen) == SOCKET_ERROR) {
      int err = WSAGetLastError();
      closesocket(s);
      return fail("udp bind failed: " + WsaErrorText(err));
    }
    sockaddr_storage local{};
    int localLen = sizeof(local);
    uint16_t gotPort = bindPort;
    if (getsockname(s, (sockaddr*)&local, &localLen) != SOCKET_ERROR) gotPort = SockPort(local);

    Conn c;
    c.id = id;
    c.sock = s;
    c.udp = true;
    c.open = true;
    conns.emplace(id, std::move(c));
    m_openCount.fetch_add(1);
    Emit("udp.open|" + util::PercentEncode(id) + "|" + std::to_string(gotPort));
    return;
  }

  if (cmd == "udp.send") {
    auto it = conns.find(id);
    if (it == conns.end()) return fail("no such socket");
    if (parts.size() < 5) return;
    const std::string host = util::PercentDecode(parts[2]);
    const uint16_t port = (uint16_t)atoi(parts[3].c_str());
    std::vector<uint8_t> data;
    util::Base64Decode(parts[4], data);
    sockaddr_storage addr{};
    int addrLen = 0;
    if (!Resolve(host, port, SOCK_DGRAM, addr, addrLen)) return fail("could not resolve host");
    if (sendto(it->second.sock, (const char*)data.data(), (int)data.size(), 0, (sockaddr*)&addr,
               addrLen) == SOCKET_ERROR) {
      return fail("udp send failed: " + WsaErrorText(WSAGetLastError()));
    }
    return;
  }

  if (cmd == "udp.close") {
    auto it = conns.find(id);
    if (it == conns.end()) return;
    CloseConn(it, conns, "udp.close");
    return;
  }
}

void NetBridge::Worker() {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Emit("fatal|0|" + util::PercentEncode("Could not start Winsock"));
    return;
  }

  std::unordered_map<std::string, Conn> conns;

  while (!m_stop) {
    std::deque<std::string> commands;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_for(lock, std::chrono::milliseconds(100),
                    [&] { return m_stop.load() || !m_commands.empty(); });
      commands.swap(m_commands);
    }
    if (m_stop) break;

    for (const std::string& c : commands) HandleCommand(c, conns);

    if (conns.empty()) continue;

    fd_set readable, writable, except;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_ZERO(&except);
    SOCKET maxSocket = 0;
    for (auto& kv : conns) {
      Conn& c = kv.second;
      FD_SET(c.sock, &readable);
      if (c.connecting || c.wantWrite) FD_SET(c.sock, &writable);
      FD_SET(c.sock, &except);
      if (c.sock > maxSocket) maxSocket = c.sock;
    }

    timeval timeout{0, 10000};  // 10 ms
    int ready = select((int)maxSocket + 1, &readable, &writable, &except, &timeout);
    if (ready <= 0) continue;

    std::vector<std::string> ids;
    ids.reserve(conns.size());
    for (auto& kv : conns) ids.push_back(kv.first);

    for (const std::string& id : ids) {
      auto it = conns.find(id);
      if (it == conns.end()) continue;
      Conn& c = it->second;

      if (FD_ISSET(c.sock, &except)) {
        CloseConn(it, conns, c.udp ? "udp.error" : "tcp.error", util::PercentEncode("socket error"));
        continue;
      }

      if (FD_ISSET(c.sock, &writable)) {
        if (c.connecting) {
          int err = 0;
          int len = sizeof(err);
          getsockopt(c.sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
          if (err != 0) {
            CloseConn(it, conns, "tcp.error", util::PercentEncode("connect failed: " + WsaErrorText(err)));
            continue;
          }
          c.connecting = false;
          c.open = true;
          m_openCount.fetch_add(1);
          Emit("tcp.open|" + util::PercentEncode(c.id));
        }
        if (c.wantWrite) {
          bool dead = false;
          FlushOutbox(c, dead);
          if (dead) {
            CloseConn(it, conns, "tcp.error", util::PercentEncode("send failed"));
            continue;
          }
        }
      }

      if (FD_ISSET(c.sock, &readable)) {
        if (c.udp) {
          for (;;) {
            uint8_t buffer[kRecvBuffer];
            sockaddr_storage from{};
            int fromLen = sizeof(from);
            int r = recvfrom(c.sock, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &fromLen);
            if (r <= 0) break;
            Emit("udp.data|" + util::PercentEncode(c.id) + "|" +
                 util::PercentEncode(AddrToString((sockaddr*)&from)) + "|" +
                 std::to_string(SockPort(from)) + "|" + util::Base64Encode(buffer, (size_t)r));
          }
        } else {
          for (;;) {
            uint8_t buffer[kRecvBuffer];
            int r = recv(c.sock, (char*)buffer, sizeof(buffer), 0);
            if (r > 0) {
              Emit("tcp.data|" + util::PercentEncode(c.id) + "|" +
                   util::Base64Encode(buffer, (size_t)r));
              continue;
            }
            if (r == 0) {
              CloseConn(it, conns, "tcp.close");
              break;
            }
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            CloseConn(it, conns, "tcp.error",
                      util::PercentEncode("receive failed: " + WsaErrorText(err)));
            break;
          }
        }
      }
    }
  }

  for (auto& kv : conns) {
    if (kv.second.sock != INVALID_SOCKET) closesocket(kv.second.sock);
  }
  conns.clear();
  m_openCount = 0;
  WSACleanup();
}
