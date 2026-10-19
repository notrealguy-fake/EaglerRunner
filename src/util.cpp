// util.cpp - implementation of the small helpers declared in util.h.
#include "util.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <iterator>

namespace util {

// ---------------------------------------------------------------- strings ----

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)need, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], need);
  return out;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  int need = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out((size_t)need, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], need, nullptr, nullptr);
  return out;
}

std::wstring ToLower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

bool IEqualsNoCase(const std::wstring& a, const std::wstring& b) {
  return ToLower(a) == ToLower(b);
}

std::wstring Trim(const std::wstring& s) {
  size_t b = s.find_first_not_of(L" \t\r\n");
  if (b == std::wstring::npos) return L"";
  size_t e = s.find_last_not_of(L" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string TrimAscii(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// -------------------------------------------------------------- encodings ----

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < len; i += 3) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back(kB64[v & 63]);
  }
  if (i < len) {
    uint32_t v = data[i] << 16;
    bool two = (i + 1 < len);
    if (two) v |= data[i + 1] << 8;
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(two ? kB64[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
  return Base64Encode(data.data(), data.size());
}

bool Base64Decode(const std::string& text, std::vector<uint8_t>& out) {
  static int8_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i) table[(uint8_t)kB64[i]] = (int8_t)i;
    init = true;
  }
  out.clear();
  out.reserve(text.size() / 4 * 3 + 3);
  uint32_t acc = 0;
  int nbits = 0;
  for (unsigned char c : text) {
    if (c == '=' ) break;
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    int8_t d = table[c];
    if (d < 0) return false;
    acc = (acc << 6) | (uint32_t)d;
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      out.push_back((uint8_t)((acc >> nbits) & 0xFF));
    }
  }
  return true;
}

std::string PercentEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

std::string PercentDecode(const std::string& s) {
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  return out;
}

std::wstring JsonEscapeW(const std::wstring& s) { return Utf8ToWide(JsonEscape(WideToUtf8(s))); }

// --------------------------------------------------------------- splitting ----

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

// ------------------------------------------------------------------- paths ----

std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
  return std::wstring(buf, n);
}

std::wstring ExeDir() {
  std::wstring p = ExePath();
  size_t pos = p.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? p : p.substr(0, pos);
}

std::wstring LocalAppDataDir() {
  PWSTR path = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
    out = path;
    CoTaskMemFree(path);
  } else {
    wchar_t buf[MAX_PATH * 2];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) out = buf;
  }
  return out;
}

std::wstring DataDir() {
  std::wstring dir = JoinPath(LocalAppDataDir(), L"EaglerRunner");
  EnsureDir(dir);
  return dir;
}

std::wstring JoinPath(std::wstring a, const std::wstring& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() != L'\\' && a.back() != L'/') a.push_back(L'\\');
  std::wstring bb = b;
  while (!bb.empty() && (bb.front() == L'\\' || bb.front() == L'/')) bb.erase(bb.begin());
  return a + bb;
}

std::wstring FileName(const std::wstring& path) {
  size_t pos = path.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring FileStem(const std::wstring& path) {
  std::wstring name = FileName(path);
  size_t pos = name.find_last_of(L'.');
  if (pos == std::wstring::npos || pos == 0) return name;
  return name.substr(0, pos);
}

std::wstring FileExtension(const std::wstring& path) {
  std::wstring name = FileName(path);
  size_t pos = name.find_last_of(L'.');
  if (pos == std::wstring::npos) return L"";
  return ToLower(name.substr(pos));
}

std::wstring ParentDir(const std::wstring& path) {
  std::wstring p = path;
  while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  size_t pos = p.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return L"";
  return p.substr(0, pos);
}

std::wstring Normalize(const std::wstring& path) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetFullPathNameW(path.c_str(), (DWORD)std::size(buf), buf, nullptr);
  if (n == 0 || n >= std::size(buf)) return path;
  std::wstring full(buf, n);
  // Strip trailing separator (except for drive roots like "C:\").
  if (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) full.pop_back();
  return full;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return IEqualsNoCase(Normalize(a), Normalize(b));
}

// ------------------------------------------------------------------- files ----

bool FileExists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t FileSize(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
  return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

uint64_t PathSizeRecursive(const std::wstring& path) {
  if (FileExists(path)) return FileSize(path);
  if (!DirExists(path)) return 0;
  uint64_t total = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(path, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring child = JoinPath(path, name);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += PathSizeRecursive(child);
    else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return total;
}

bool EnsureDir(const std::wstring& path) {
  if (path.empty()) return false;
  if (DirExists(path)) return true;
  std::wstring parent = ParentDir(path);
  if (!parent.empty() && !DirExists(parent)) {
    if (!EnsureDir(parent)) return false;
  }
  if (CreateDirectoryW(path.c_str(), nullptr)) return true;
  return DirExists(path);
}

bool CopyFileTo(const std::wstring& src, const std::wstring& dst) {
  if (!EnsureDir(ParentDir(dst))) return false;
  return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

bool CopyPathDeep(const std::wstring& src, const std::wstring& dst) {
  if (FileExists(src)) return CopyFileTo(src, dst);
  if (!DirExists(src)) return false;
  if (!EnsureDir(dst)) return false;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(src, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring s = JoinPath(src, name);
    std::wstring d = JoinPath(dst, name);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = CopyPathDeep(s, d) && ok;
    else ok = CopyFileTo(s, d) && ok;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return ok;
}

bool RemovePathDeep(const std::wstring& path) {
  if (FileExists(path)) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) != 0;
  }
  if (!DirExists(path)) return true;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(path, L"*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      std::wstring child = JoinPath(path, name);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        RemovePathDeep(child);
      } else {
        SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(child.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return RemoveDirectoryW(path.c_str()) != 0;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& text) {
  if (!EnsureDir(ParentDir(path))) return false;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = text.empty() ||
            (WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) &&
             written == text.size());
  CloseHandle(h);
  return ok;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
  out.clear();
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
  out.resize((size_t)size.QuadPart);
  DWORD read = 0;
  bool ok = size.QuadPart == 0 ||
            (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
  CloseHandle(h);
  // Tolerate a UTF-8 BOM.
  if (out.size() >= 3 && (uint8_t)out[0] == 0xEF && (uint8_t)out[1] == 0xBB &&
      (uint8_t)out[2] == 0xBF) {
    out.erase(0, 3);
  }
  return ok;
}

std::wstring FormatBytes(uint64_t bytes) {
  const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
  double value = (double)bytes;
  int unit = 0;
  while (value >= 1024.0 && unit < 3) { value /= 1024.0; ++unit; }
  wchar_t buf[64];
  if (unit == 0) swprintf(buf, 64, L"%llu %s", (unsigned long long)bytes, units[unit]);
  else swprintf(buf, 64, L"%.1f %s", value, units[unit]);
  return buf;
}

std::wstring SanitizeFileName(const std::wstring& name) {
  std::wstring out;
  out.reserve(name.size());
  for (wchar_t c : name) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
        c == L'<' || c == L'>' || c == L'|' || c < 0x20) {
      continue;
    }
    out.push_back(c);
  }
  out = Trim(out);
  while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
  if (out.size() > 64) out.resize(64);
  return out;
}

}  // namespace util
