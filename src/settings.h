// settings.h - tiny persistent settings store (INI-style, UTF-8).
#pragma once

#include <string>
#include <vector>

class Settings {
 public:
  void Load();
  void Save() const;

  void AddRecent(const std::wstring& path);
  void RemoveRecent(const std::wstring& path);

  // ---- options
  bool lowEnd = false;        // software rendering / no GPU (potato PC mode)
  bool askToSave = true;      // offer "add to library" right after opening a file
  bool showStatusBar = true;  // status bar inside the launcher UI
  int  zoomPercent = 100;
  std::wstring theme = L"dark";  // launcher background: "dark" or "light"

  // ---- window geometry
  int winX = -1;
  int winY = -1;
  int winW = 1024;
  int winH = 720;

  // ---- recent files (most recent first)
  std::vector<std::wstring> recents;

  static std::wstring FilePath();

 private:
  static constexpr size_t kMaxRecents = 10;
};
