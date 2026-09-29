/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS5StorageProvider.h"

#include "utils/StringUtils.h"
#include "utils/log.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "MediaSource.h"

#include <sys/stat.h>

std::unique_ptr<IStorageProvider> IStorageProvider::CreateInstance()
{
  return std::make_unique<CPS5StorageProvider>();
}

namespace
{
bool Exists(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

void Add(std::vector<CMediaSource>& drives, const char* path, const char* name)
{
  if (!Exists(path))
    return;
  CMediaSource share;
  share.strPath = path;
  share.strName = name;
  share.m_iDriveType = SourceType::LOCAL;
  drives.push_back(share);
}
} // namespace

namespace
{
// The homebrew loader's FTP server (etaHEN / ftpsrv) exposes the whole PS5
// filesystem. It is the way to reach media placed on the console, so it is
// offered as a source when it is actually listening - checked here, so the
// entry appears only while the server is up. etaHEN commonly uses 1337, other
// setups 2121.
bool LocalPortOpen(uint16_t port)
{
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  // non-blocking connect with a short timeout: the server is local, so it
  // answers at once or not at all
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  bool open = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  if (!open && errno == EINPROGRESS)
  {
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    timeval tv{0, 200000}; // 200 ms
    if (select(fd + 1, nullptr, &wr, nullptr, &tv) > 0)
    {
      int err = 0;
      socklen_t len = sizeof(err);
      open = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
    }
  }
  close(fd);
  return open;
}
} // namespace

void CPS5StorageProvider::GetLocalDrives(std::vector<CMediaSource>& localDrives)
{
  // No entries for the app's own areas (/app0, /download0, /data): media is
  // never stored there. The only local source is the PS5 filesystem over the
  // loader's FTP server, when it is running.
  for (const uint16_t port : {static_cast<uint16_t>(1337), static_cast<uint16_t>(2121)})
    if (LocalPortOpen(port))
    {
      CMediaSource share;
      share.strPath = StringUtils::Format("ftp://127.0.0.1:{}/", port);
      share.strName = "PS5 storage (FTP)";
      share.m_iDriveType = SourceType::REMOTE;
      localDrives.push_back(share);
      break;
    }
}
namespace
{
// USB drives and extended storage appear here once the sandbox is open
const char* const kRemovable[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                                  "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
                                  "/mnt/ext0", "/mnt/ext1"};
}

void CPS5StorageProvider::GetRemovableDrives(std::vector<CMediaSource>& removableDrives)
{
  for (const char* path : kRemovable)
    Add(removableDrives, path, path);
}

bool CPS5StorageProvider::PumpDriveChangeEvents(IStorageEventsCallback* callback)
{
  const auto now = std::chrono::steady_clock::now();
  if (m_checked && now - m_lastCheck < std::chrono::seconds(2))
    return false;
  m_lastCheck = now;

  std::set<std::string> present;
  for (const char* path : kRemovable)
    if (Exists(path))
      present.insert(path);
  if (!m_checked)
  {
    m_checked = true;
    m_removable = present; // the initial set: listed, not announced
    return false;
  }

  bool changed = false;
  for (const auto& path : present)
    if (!m_removable.count(path))
    {
      CLog::Log(LOGINFO, "CPS5StorageProvider: {} appeared", path);
      if (callback)
        callback->OnStorageAdded({path, path, MEDIA_DETECT::STORAGE::Type::UNKNOWN});
      changed = true;
    }
  for (const auto& path : m_removable)
    if (!present.count(path))
    {
      CLog::Log(LOGINFO, "CPS5StorageProvider: {} removed", path);
      if (callback)
        callback->OnStorageUnsafelyRemoved({path, path, MEDIA_DETECT::STORAGE::Type::UNKNOWN});
      changed = true;
    }
  m_removable = present;
  return changed;
}

std::vector<std::string> CPS5StorageProvider::GetDiskUsage()
{
  return {};
}
