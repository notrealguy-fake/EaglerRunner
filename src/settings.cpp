// settings.cpp - load/save the launcher settings.
#include "settings.h"

#include "util.h"

#include <algorithm>
#include <cstdlib>

std::wstring Settings::FilePath() { return util::JoinPath(util::DataDir(), L"settings.ini"); }

void Settings::Load() {
  std::string text;
  if (!util::ReadFileUtf8(FilePath(), text)) return;

  for (const std::string& rawLine : util::Split(text, '\n')) {
    std::string line = util::TrimAscii(rawLine);
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = util::TrimAscii(line.substr(0, eq));
    std::string val = util::TrimAscii(line.substr(eq + 1));

    if (key == "lowEnd") lowEnd = (val == "1" || val == "true");
    else if (key == "askToSave") askToSave = (val == "1" || val == "true");
    else if (key == "showStatusBar") showStatusBar = (val == "1" || val == "true");
    else if (key == "zoom") zoomPercent = atoi(val.c_str());
    else if (key == "theme") theme = util::Utf8ToWide(val);
    else if (key == "winX") winX = atoi(val.c_str());
    else if (key == "winY") winY = atoi(val.c_str());
    else if (key == "winW") winW = atoi(val.c_str());
    else if (key == "winH") winH = atoi(val.c_str());
    else if (key == "recent" && !val.empty()) recents.push_back(util::Utf8ToWide(val));
  }

  if (zoomPercent < 25) zoomPercent = 25;
  if (zoomPercent > 400) zoomPercent = 400;
  if (theme != L"light") theme = L"dark";
  if (winW < 480) winW = 480;
  if (winH < 360) winH = 360;
  if (recents.size() > kMaxRecents) recents.resize(kMaxRecents);
}

void Settings::Save() const {
  std::string text;
  text += "# EaglerRunner settings\n";
  text += std::string("lowEnd=") + (lowEnd ? "1" : "0") + "\n";
  text += std::string("askToSave=") + (askToSave ? "1" : "0") + "\n";
  text += std::string("showStatusBar=") + (showStatusBar ? "1" : "0") + "\n";
  text += "zoom=" + std::to_string(zoomPercent) + "\n";
  text += "theme=" + util::WideToUtf8(theme) + "\n";
  text += "winX=" + std::to_string(winX) + "\n";
  text += "winY=" + std::to_string(winY) + "\n";
  text += "winW=" + std::to_string(winW) + "\n";
  text += "winH=" + std::to_string(winH) + "\n";
  for (const std::wstring& r : recents) {
    text += "recent=" + util::WideToUtf8(r) + "\n";
  }
  util::WriteFileUtf8(FilePath(), text);
}

void Settings::AddRecent(const std::wstring& path) {
  if (path.empty()) return;
  std::wstring norm = util::Normalize(path);
  recents.erase(std::remove_if(recents.begin(), recents.end(),
                               [&](const std::wstring& p) { return util::SamePath(p, norm); }),
                recents.end());
  recents.insert(recents.begin(), norm);
  if (recents.size() > kMaxRecents) recents.resize(kMaxRecents);
  Save();
}

void Settings::RemoveRecent(const std::wstring& path) {
  recents.erase(std::remove_if(recents.begin(), recents.end(),
                               [&](const std::wstring& p) { return util::SamePath(p, path); }),
                recents.end());
  Save();
}
