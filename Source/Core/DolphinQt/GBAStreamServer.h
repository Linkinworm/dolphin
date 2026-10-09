// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QImage>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET SocketHandle;
#else
typedef int SocketHandle;
#endif

class GBAStreamServer
{
public:
  explicit GBAStreamServer(int slot);
  ~GBAStreamServer();

  bool Start(int port = 43576);
  void Stop();
  void BroadcastFrame(const QImage& frame);
  bool IsRunning() const { return m_running; }
  QString GetUrl() const;

private:
  void ServerLoop();
  void HandleClient(SocketHandle client);
  void HandleInput(const QString& path);

  enum class ControlID
  {
    A,
    B,
    L,
    R,
    Start,
    Select,
    Up,
    Down,
    Left,
    Right,
    L_Analog,
    R_Analog,
    Count
  };

  int m_slot;
  int m_port = 43576;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::vector<unsigned char> m_current_jpeg;
  std::mutex m_jpeg_mutex;
  std::condition_variable m_frame_cv;
  uint64_t m_frame_count = 0;
  SocketHandle m_listen_sock;
  QString m_local_ip;

  std::mutex m_input_mutex;
  std::map<ControlID, double> m_input_states;
  bool m_input_registered = false;
};
