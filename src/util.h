// util.h - small, dependency-free helpers (strings, base64, percent-encoding, files).
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace util {

// ---- strings ----------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);
std::wstring ToLower(std::wstring s);
bool         IEqualsNoCase(const std::wstring& a, const std::wstring& b);
std::wstring Trim(const std::wstring& s);
std::string  TrimAscii(const std::string& s);

// ---- encodings --------------------------------------------------------------
std::string Base64Encode(const uint8_t* data, size_t len);
std::string Base64Encode(const std::vector<uint8_t>& data);
bool        Base64Decode(const std::string& text, std::vector<uint8_t>& out);
std::string PercentEncode(const std::string& s);
std::string PercentDecode(const std::string& s);
std::string JsonEscape(const std::string& s);
std::wstring JsonEscapeW(const std::wstring& s);

// ---- splitting --------------------------------------------------------------
std::vector<std::string>  Split(const std::string& s, char sep);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);

// ---- paths ------------------------------------------------------------------
std::wstring ExeDir();
std::wstring ExePath();
std::wstring LocalAppDataDir();
std::wstring DataDir();          // %LOCALAPPDATA%\EaglerRunner (created on demand)
std::wstring JoinPath(std::wstring a, const std::wstring& b);
std::wstring FileName(const std::wstring& path);
std::wstring FileStem(const std::wstring& path);
std::wstring FileExtension(const std::wstring& path);
std::wstring ParentDir(const std::wstring& path);
std::wstring Normalize(const std::wstring& path);
bool         SamePath(const std::wstring& a, const std::wstring& b);

// ---- files ------------------------------------------------------------------
bool         FileExists(const std::wstring& path);
bool         DirExists(const std::wstring& path);
uint64_t     FileSize(const std::wstring& path);
uint64_t     PathSizeRecursive(const std::wstring& path);
bool         EnsureDir(const std::wstring& path);
bool         CopyFileTo(const std::wstring& src, const std::wstring& dst);
bool         CopyPathDeep(const std::wstring& src, const std::wstring& dst);
bool         RemovePathDeep(const std::wstring& path);
bool         WriteFileUtf8(const std::wstring& path, const std::string& text);
bool         ReadFileUtf8(const std::wstring& path, std::string& out);
std::wstring FormatBytes(uint64_t bytes);
std::wstring SanitizeFileName(const std::wstring& name);

}  // namespace util
