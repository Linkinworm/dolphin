// SPDX-License-Identifier: GPL-2.0-or-later
#include "DolphinQt/GBAStreamServer.h"

#include <QBuffer>
#include <QImageWriter>
#include <QUrl>
#include <QUrlQuery>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "Core/HW/GBAPad.h"
#include "Core/HW/GCPad.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

GBAStreamServer::GBAStreamServer(int slot) : m_slot(slot), m_listen_sock(-1)
{
#ifdef _WIN32
  WSADATA wsaData;
  [[maybe_unused]] int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

  // Get local IP using standard sockets instead of QNetworkInterface
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0)
  {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, nullptr, &hints, &res) == 0)
    {
      char addr_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &((struct sockaddr_in*)res->ai_addr)->sin_addr, addr_str,
                sizeof(addr_str));
      m_local_ip = QString::fromLatin1(addr_str);
      freeaddrinfo(res);
    }
  }

  if (m_local_ip.isEmpty())
    m_local_ip = QStringLiteral("127.0.0.1");
}

GBAStreamServer::~GBAStreamServer()
{
  Stop();
#ifdef _WIN32
  WSACleanup();
#endif
}

bool GBAStreamServer::Start(int port)
{
  if (m_running)
    return true;
  m_port = port;

  m_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (m_listen_sock == -1)
    return false;

  int opt = 1;
  setsockopt(m_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(m_port);

  if (bind(m_listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == -1)
  {
    closesocket(m_listen_sock);
    return false;
  }

  if (listen(m_listen_sock, 5) == -1)
  {
    closesocket(m_listen_sock);
    return false;
  }

  m_running = true;

  // Register input override for both GameCube and GBA slots to be safe
  auto register_override = [this](InputConfig* config) {
    if (auto* controller = config->GetController(m_slot))
    {
      controller->SetInputOverrideFunction(
          [this](std::string_view group, std::string_view control,
                 double controller_state) -> std::optional<double> {
            std::lock_guard<std::mutex> lock(m_input_mutex);

            ControlID id = ControlID::Count;
            if (group == "Buttons")
            {
              if (control == "A")
                id = ControlID::A;
              else if (control == "B")
                id = ControlID::B;
              else if (control == "Start")
                id = ControlID::Start;
              else if (control == "Z")
                id = ControlID::Select;
            }
            else if (group == "D-Pad")
            {
              if (control == "Up")
                id = ControlID::Up;
              else if (control == "Down")
                id = ControlID::Down;
              else if (control == "Left")
                id = ControlID::Left;
              else if (control == "Right")
                id = ControlID::Right;
            }
            else if (group == "Triggers")
            {
              if (control == "L")
                id = ControlID::L;
              else if (control == "R")
                id = ControlID::R;
              else if (control == "L-Analog")
                id = ControlID::L_Analog;
              else if (control == "R-Analog")
                id = ControlID::R_Analog;
            }

            if (id != ControlID::Count)
            {
              auto it = m_input_states.find(id);
              if (it != m_input_states.end() && it->second > 0)
                return it->second;
            }

            return std::nullopt;
          });
    }
  };

  register_override(Pad::GetConfig());
  register_override(Pad::GetGBAConfig());

  m_input_registered = true;
  m_thread = std::thread(&GBAStreamServer::ServerLoop, this);
  return true;
}

void GBAStreamServer::Stop()
{
  m_running = false;
  if (m_input_registered)
  {
    if (auto* controller = Pad::GetConfig()->GetController(m_slot))
      controller->ClearInputOverrideFunction();
    if (auto* controller = Pad::GetGBAConfig()->GetController(m_slot))
      controller->ClearInputOverrideFunction();
    m_input_registered = false;
  }
  if (m_listen_sock != -1)
  {
    closesocket(m_listen_sock);
    m_listen_sock = -1;
  }
  if (m_thread.joinable())
    m_thread.join();
}

void GBAStreamServer::BroadcastFrame(const QImage& frame)
{
  if (frame.isNull())
    return;

  QByteArray ba;
  QBuffer buffer(&ba);
  buffer.open(QIODevice::WriteOnly);
  if (!frame.save(&buffer, "JPG", 50))
  {
    if (!frame.save(&buffer, "PNG"))
      return;
  }

  {
    std::lock_guard<std::mutex> lock(m_jpeg_mutex);
    m_current_jpeg.assign(ba.begin(), ba.end());
    m_frame_count++;
  }
  m_frame_cv.notify_all();
}

QString GBAStreamServer::GetUrl() const
{
  return QStringLiteral("http://%1:%2").arg(m_local_ip).arg(m_port);
}

void GBAStreamServer::ServerLoop()
{
  while (m_running)
  {
    sockaddr_in client_addr{};
#ifdef _WIN32
    int addr_len = sizeof(client_addr);
#else
    socklen_t addr_len = sizeof(client_addr);
#endif
    SocketHandle client = accept(m_listen_sock, (struct sockaddr*)&client_addr, &addr_len);
    if (client == -1)
      continue;

    std::thread(&GBAStreamServer::HandleClient, this, client).detach();
  }
}

void GBAStreamServer::HandleClient(SocketHandle client)
{
  char buffer[4096];
  int bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
  if (bytes <= 0)
  {
    closesocket(client);
    return;
  }
  buffer[bytes] = '\0';

  QString request = QString::fromLatin1(buffer);
  QStringList lines = request.split(QStringLiteral("\r\n"));
  if (lines.isEmpty())
  {
    closesocket(client);
    return;
  }

  QStringList requestLine = lines[0].split(QStringLiteral(" "));
  if (requestLine.size() < 2)
  {
    closesocket(client);
    return;
  }

  QString path = requestLine[1];
  QString cleanPath = path.split(u'?')[0];

  if (cleanPath.startsWith(QStringLiteral("/input")))
  {
    HandleInput(path);
    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 0\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Connection: close\r\n\r\n";
    send(client, response.c_str(), (int)response.length(), 0);
  }
  else if (cleanPath == QStringLiteral("/stream"))
  {
    // Standard MJPEG response
    std::string boundary = "dolphin_gba_stream";
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: multipart/x-mixed-replace; boundary=" +
                         boundary +
                         "\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: keep-alive\r\n"
                         "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                         "Pragma: no-cache\r\n\r\n";

    if (send(client, header.c_str(), (int)header.length(), 0) <= 0)
    {
      closesocket(client);
      return;
    }

    uint64_t last_frame = 0;
    while (m_running)
    {
      std::vector<unsigned char> jpeg;
      {
        std::unique_lock<std::mutex> lock(m_jpeg_mutex);
        m_frame_cv.wait(lock,
                        [this, last_frame] { return !m_running || m_frame_count > last_frame; });
        if (!m_running)
          break;

        jpeg = m_current_jpeg;
        last_frame = m_frame_count;
      }

      if (!jpeg.empty())
      {
        std::string part = "--" + boundary +
                           "\r\n"
                           "Content-Type: image/jpeg\r\n"
                           "Content-Length: " +
                           std::to_string(jpeg.size()) + "\r\n\r\n";

        if (send(client, part.c_str(), (int)part.length(), 0) <= 0)
          break;
        if (send(client, (const char*)jpeg.data(), (int)jpeg.size(), 0) <= 0)
          break;
        if (send(client, "\r\n", 2, 0) <= 0)
          break;
      }
    }
  }
  else if (cleanPath == QStringLiteral("/manifest.json"))
  {
    std::string manifest = R"({
            "name": "Dolphin GBA Remote",
            "short_name": "GBA Remote",
            "start_url": "/",
            "display": "standalone",
            "orientation": "landscape",
            "background_color": "#000000",
            "theme_color": "#000000",
            "icons": []
        })";
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " +
                         std::to_string(manifest.length()) +
                         "\r\n"
                         "Connection: close\r\n\r\n";
    send(client, header.c_str(), (int)header.length(), 0);
    send(client, manifest.c_str(), (int)manifest.length(), 0);
  }
  else
  {
    // Serve HTML UI
    std::string html = R"rawhtml(<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
    <meta name="mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <link rel="manifest" href="/manifest.json">
    <style>
        body { margin: 0; background: black; color: white; overflow: hidden; font-family: sans-serif; touch-action: none; -webkit-user-select: none; }
        #stream-container { position: relative; width: 100vw; height: 100vh; display: flex; align-items: center; justify-content: center; background: #111; }
        #stream { max-width: 100%; max-height: 100%; object-fit: contain; pointer-events: none; z-index: 1; }
        #loading { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); color: #555; font-size: 18px; text-align: center; }
        #refresh-hint { margin-top: 10px; font-size: 12px; color: #333; cursor: pointer; text-decoration: underline; }
        .btn { position: absolute; background: rgba(255,255,255,0.1); border: 2px solid rgba(255,255,255,0.3); border-radius: 50%; width: 70px; height: 70px; display: flex; align-items: center; justify-content: center; font-weight: bold; z-index: 10; transition: background 0.1s; }
        .btn:active { background: rgba(255,255,255,0.4); border-color: rgba(255,255,255,0.8); }
        #btn-a { bottom: 80px; right: 20px; width: 100px; height: 100px; font-size: 28px; }
        #btn-b { bottom: 160px; right: 110px; width: 100px; height: 100px; font-size: 28px; }
        #btn-l { top: 20px; left: 20px; border-radius: 10px; width: 140px; height: 60px; }
        #btn-r { top: 20px; right: 20px; border-radius: 10px; width: 140px; height: 60px; }
        #btn-start { bottom: 30px; left: 50%; transform: translateX(30px); border-radius: 20px; width: 100px; height: 40px; font-size: 16px; }
        #btn-select { bottom: 30px; right: 50%; transform: translateX(-30px); border-radius: 20px; width: 100px; height: 40px; font-size: 16px; }
        #dpad { position: absolute; bottom: 60px; left: 30px; width: 210px; height: 210px; z-index: 10; }
        .dbtn { position: absolute; background: rgba(255,255,255,0.1); border: 2px solid rgba(255,255,255,0.3); }
        #up { top: 0; left: 70px; width: 70px; height: 70px; border-radius: 10px 10px 0 0; }
        #down { bottom: 0; left: 70px; width: 70px; height: 70px; border-radius: 0 0 10px 10px; }
        #left { left: 0; top: 70px; width: 70px; height: 70px; border-radius: 10px 0 0 10px; }
        #right { right: 0; top: 70px; width: 70px; height: 70px; border-radius: 0 10px 10px 0; }
        #center { left: 70px; top: 70px; width: 70px; height: 70px; background: rgba(255,255,255,0.05); border: none; }
    </style>
</head>
<body>
    <div id="stream-container">
        <div id="loading">
            <div>Waiting for GBA frames...</div>
            <div id="refresh-hint" onclick="refreshStream()">Click here to refresh manually</div>
        </div>
        <img id="stream" src="/stream" onload="document.getElementById('loading').style.display='none'">
    </div>
    <div id="btn-a" class="btn" ontouchstart="send('A',1)" ontouchend="send('A',0)">A</div>
    <div id="btn-b" class="btn" ontouchstart="send('B',1)" ontouchend="send('B',0)">B</div>
    <div id="btn-l" class="btn" ontouchstart="send('L',1)" ontouchend="send('L',0)">L</div>
    <div id="btn-r" class="btn" ontouchstart="send('R',1)" ontouchend="send('R',0)">R</div>
    <div id="btn-start" class="btn" ontouchstart="send('START',1)" ontouchend="send('START',0)">START</div>
    <div id="btn-select" class="btn" ontouchstart="send('SELECT',1)" ontouchend="send('SELECT',0)">SELECT</div>
    <div id="dpad">
        <div id="up" class="dbtn" ontouchstart="send('UP',1)" ontouchend="send('UP',0)"></div>
        <div id="down" class="dbtn" ontouchstart="send('DOWN',1)" ontouchend="send('DOWN',0)"></div>
        <div id="left" class="dbtn" ontouchstart="send('LEFT',1)" ontouchend="send('LEFT',0)"></div>
        <div id="right" class="dbtn" ontouchstart="send('RIGHT',1)" ontouchend="send('RIGHT',0)"></div>
        <div id="center" class="dbtn"></div>
    </div>
    <script>
        function send(id, val) {
            fetch('/input?id=' + id + '&val=' + val, { mode: 'no-cors' });
        }
        function refreshStream() {
            const img = document.getElementById('stream');
            img.src = '/stream?t=' + Date.now();
        }
        document.addEventListener('touchstart', function(e) {
            if (e.target.classList.contains('btn') || e.target.classList.contains('dbtn')) {
                e.preventDefault();
            }
        }, {passive: false});

        // Auto-refresh if stream doesn't load
        let refreshTimer = setTimeout(refreshStream, 3000);

        const img = document.getElementById('stream');
        img.onerror = function() {
            console.log("Stream error, retrying...");
            setTimeout(refreshStream, 1000);
        };
        img.onload = function() {
            document.getElementById('loading').style.display = 'none';
            clearTimeout(refreshTimer);
        };
    </script>
</body>
</html>)rawhtml";
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html\r\n"
                         "Content-Length: " +
                         std::to_string(html.length()) +
                         "\r\n"
                         "Connection: close\r\n\r\n";
    send(client, header.c_str(), (int)header.length(), 0);
    send(client, html.c_str(), (int)html.length(), 0);
  }

  closesocket(client);
}

void GBAStreamServer::HandleInput(const QString& path)
{
  QUrl url(path);
  QUrlQuery query(url);
  QString id_str = query.queryItemValue(QStringLiteral("id"));
  double val = query.queryItemValue(QStringLiteral("val")).toDouble();

  ControlID control = ControlID::Count;

  if (id_str == QStringLiteral("A"))
    control = ControlID::A;
  else if (id_str == QStringLiteral("B"))
    control = ControlID::B;
  else if (id_str == QStringLiteral("L"))
    control = ControlID::L;
  else if (id_str == QStringLiteral("R"))
    control = ControlID::R;
  else if (id_str == QStringLiteral("START"))
    control = ControlID::Start;
  else if (id_str == QStringLiteral("SELECT"))
    control = ControlID::Select;
  else if (id_str == QStringLiteral("UP"))
    control = ControlID::Up;
  else if (id_str == QStringLiteral("DOWN"))
    control = ControlID::Down;
  else if (id_str == QStringLiteral("LEFT"))
    control = ControlID::Left;
  else if (id_str == QStringLiteral("RIGHT"))
    control = ControlID::Right;

  if (control != ControlID::Count)
  {
    std::lock_guard<std::mutex> lock(m_input_mutex);
    m_input_states[control] = val;

    // Also handle analog triggers for L/R if needed
    if (control == ControlID::L)
      m_input_states[ControlID::L_Analog] = val;
    if (control == ControlID::R)
      m_input_states[ControlID::R_Analog] = val;
  }
}
