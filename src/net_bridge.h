// net_bridge.h - native TCP + UDP networking for the loaded HTML page.
//
// The page gets window.eaglerNet.{tcp,udp}; this class owns the actual WinSock
// sockets on a worker thread. Nothing is proxied, filtered or restricted - raw
// sockets straight to whatever host/port the page asks for, IPv4 and IPv6.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class NetBridge {
 public:
  static constexpr UINT kNotifyMessage = WM_APP + 100;

  NetBridge() = default;
  ~NetBridge();
  NetBridge(const NetBridge&) = delete;
  NetBridge& operator=(const NetBridge&) = delete;

  // hwnd/msg are used to wake the UI thread when data arrives.
  void Start(HWND hwnd, UINT msg);
  void Stop();
  bool Running() const { return m_running.load(); }

  // Called on the UI thread with a pipe-separated, percent-encoded command:
  //   tcp.connect|id|host|port        tcp.send|id|base64      tcp.close|id
  //   udp.open|id|bindPort            udp.send|id|host|port|base64
  //   udp.close|id
  void Send(const std::string& line);

  // Called on the UI thread: takes every event queued since the last call,
  // joined by '\n'. Returns an empty string when nothing is pending.
  std::string Drain();

  int OpenConnectionCount() const { return m_openCount.load(); }

 private:
  struct Conn {
    std::string id;
    SOCKET sock = INVALID_SOCKET;
    bool udp = false;
    bool connecting = false;
    bool wantWrite = false;
    bool open = false;
    std::deque<std::vector<uint8_t>> outbox;
    size_t outOffset = 0;
    sockaddr_storage peer{};
    int peerLen = 0;
  };
  using ConnMap = std::unordered_map<std::string, Conn>;

  struct DnsEntry {
    sockaddr_storage addr{};
    int len = 0;
    ULONGLONG stamp = 0;
  };

  void Worker();
  void HandleCommand(const std::string& line, ConnMap& conns);
  void CloseConn(ConnMap::iterator it, ConnMap& conns, const std::string& event,
                 const std::string& detail = "");
  void FlushOutbox(Conn& conn, bool& dead);
  void Emit(const std::string& line);
  bool Resolve(const std::string& host, uint16_t port, int sockType, sockaddr_storage& out,
               int& outLen);
  std::string AddrToString(const sockaddr* sa);

  HWND m_hwnd = nullptr;
  UINT m_msg = 0;
  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::string> m_commands;
  std::deque<std::string> m_events;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_notifyPending{false};
  std::atomic<int> m_openCount{0};
  std::unordered_map<std::string, DnsEntry> m_dnsCache;
};
