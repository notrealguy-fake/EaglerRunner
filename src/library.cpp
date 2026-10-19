// library.cpp - implementation of the persistent client library.
#include "library.h"

#include "util.h"

#include <shlwapi.h>

#include <algorithm>
#include <cstdio>

std::wstring LibraryRoot() { return util::JoinPath(util::DataDir(), L"library"); }

static std::wstring NowStamp() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[64];
  swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
  return buf;
}

static std::wstring ReadManifestValue(const std::wstring& manifest, const std::string& key) {
  std::string text;
  if (!util::ReadFileUtf8(manifest, text)) return L"";
  for (const std::string& raw : util::Split(text, '\n')) {
    std::string line = util::TrimAscii(raw);
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (util::TrimAscii(line.substr(0, eq)) == key) {
      return util::Utf8ToWide(util::TrimAscii(line.substr(eq + 1)));
    }
  }
  return L"";
}

std::vector<LibraryEntry> LibraryList() {
  std::vector<LibraryEntry> out;
  const std::wstring root = LibraryRoot();
  if (!util::DirExists(root)) return out;

  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(util::JoinPath(root, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;

    LibraryEntry e;
    e.dir = util::JoinPath(root, name);
    e.name = name;
    std::wstring manifest = util::JoinPath(e.dir, L"manifest.txt");
    std::wstring html = ReadManifestValue(manifest, "html");
    if (!html.empty()) {
      e.htmlFile = html;
      e.origPath = ReadManifestValue(manifest, "orig");
      e.added = ReadManifestValue(manifest, "added");
      e.sourceSize = ReadManifestValue(manifest, "size");
      std::wstring customName = ReadManifestValue(manifest, "name");
      if (!customName.empty()) e.name = customName;
    } else {
      // No manifest: pick the first .html in the folder.
      WIN32_FIND_DATAW hf{};
      HANDLE hh = FindFirstFileW(util::JoinPath(e.dir, L"*.html").c_str(), &hf);
      if (hh != INVALID_HANDLE_VALUE) {
        e.htmlFile = hf.cFileName;
        FindClose(hh);
      }
    }
    if (e.htmlFile.empty()) continue;
    if (e.added.empty()) e.added = L"?";
    if (e.sourceSize.empty()) e.sourceSize = util::FormatBytes(util::PathSizeRecursive(e.dir));
    out.push_back(e);
  } while (FindNextFileW(h, &fd));
  FindClose(h);

  std::sort(out.begin(), out.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
    return util::ToLower(a.name) < util::ToLower(b.name);
  });
  return out;
}

bool LibrarySaveFolder(const std::wstring& sourceDir, const std::wstring& htmlRelName,
                       const std::wstring& rawName, std::wstring& errorOut,
                       LibraryEntry* entryOut) {
  errorOut.clear();

  if (!util::DirExists(sourceDir)) {
    errorOut = L"The downloaded folder is missing:\n" + sourceDir;
    return false;
  }
  std::wstring htmlCheck = util::JoinPath(sourceDir, htmlRelName);
  if (htmlRelName.empty() || !util::FileExists(htmlCheck)) {
    errorOut = L"Could not find the client HTML inside the download.";
    return false;
  }

  std::wstring name = util::SanitizeFileName(rawName);
  if (name.empty()) name = L"Downloaded client";

  std::wstring root = LibraryRoot();
  std::wstring destDir = util::JoinPath(root, name);

  if (util::DirExists(destDir)) util::RemovePathDeep(destDir);
  if (!util::EnsureDir(destDir)) {
    errorOut = L"Could not create the library folder:\n" + destDir;
    return false;
  }

  // Copy every entry of the folder so relative assets keep working.
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(util::JoinPath(sourceDir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    errorOut = L"Could not read the downloaded files.";
    util::RemovePathDeep(destDir);
    return false;
  }
  bool copiedAll = true;
  do {
    std::wstring child = fd.cFileName;
    if (child == L"." || child == L"..") continue;
    if (!util::CopyPathDeep(util::JoinPath(sourceDir, child), util::JoinPath(destDir, child))) {
      copiedAll = false;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);

  if (!copiedAll) {
    errorOut = L"Could not copy all downloaded files into the library.";
    util::RemovePathDeep(destDir);
    return false;
  }

  LibraryEntry entry;
  entry.name = name;
  entry.dir = destDir;
  entry.htmlFile = htmlRelName;
  entry.origPath = util::Normalize(sourceDir);
  entry.added = NowStamp();
  entry.sourceSize = util::FormatBytes(util::PathSizeRecursive(destDir));

  std::string manifest;
  manifest += "name=" + util::WideToUtf8(entry.name) + "\n";
  manifest += "html=" + util::WideToUtf8(entry.htmlFile) + "\n";
  manifest += "orig=" + util::WideToUtf8(entry.origPath) + "\n";
  manifest += "added=" + util::WideToUtf8(entry.added) + "\n";
  manifest += "size=" + util::WideToUtf8(entry.sourceSize) + "\n";
  util::WriteFileUtf8(util::JoinPath(destDir, L"manifest.txt"), manifest);

  if (entryOut) *entryOut = entry;
  return true;
}

std::wstring LibraryHtmlPath(const LibraryEntry& entry) {
  return util::JoinPath(entry.dir, entry.htmlFile);
}

bool LibraryFind(const std::wstring& name, LibraryEntry& out) {
  for (const LibraryEntry& e : LibraryList()) {
    if (util::IEqualsNoCase(e.name, name)) { out = e; return true; }
  }
  return false;
}

bool LibraryDelete(const std::wstring& name) {
  LibraryEntry entry;
  if (!LibraryFind(name, entry)) return false;
  if (!util::DirExists(entry.dir)) return false;
  return util::RemovePathDeep(entry.dir);
}

bool LibrarySave(const std::wstring& sourceHtml, const std::wstring& rawName, std::wstring& errorOut,
                 LibraryEntry* entryOut) {
  errorOut.clear();

  if (!util::FileExists(sourceHtml)) {
    errorOut = L"The file no longer exists:\n" + sourceHtml;
    return false;
  }
  std::wstring name = util::SanitizeFileName(rawName);
  if (name.empty()) {
    errorOut = L"Please enter a name for this client.";
    return false;
  }

  std::wstring root = LibraryRoot();
  std::wstring destDir = util::JoinPath(root, name);
  std::wstring srcDir = util::ParentDir(sourceHtml);
  std::wstring htmlName = util::FileName(sourceHtml);
  std::wstring stem = util::FileStem(sourceHtml);

  // The player already has one with this name -> replace it.
  if (util::DirExists(destDir)) util::RemovePathDeep(destDir);
  if (!util::EnsureDir(destDir)) {
    errorOut = L"Could not create the library folder:\n" + destDir;
    return false;
  }

  // 1) the html file itself
  if (!util::CopyFileTo(sourceHtml, util::JoinPath(destDir, htmlName))) {
    errorOut = L"Could not copy the HTML file into the library.";
    util::RemovePathDeep(destDir);
    return false;
  }

  // 2) sibling files/folders that share the same base name
  //    (client.html -> client.epk, client.jar, client_files\, client-assets\, ...)
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(util::JoinPath(srcDir, L"*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    // Avoid copying the library into itself when re-importing a library entry.
    std::wstring srcDirNorm = util::Normalize(srcDir);
    bool sourceInsideLibrary =
        util::ToLower(srcDirNorm).rfind(util::ToLower(util::Normalize(root)), 0) == 0;
    do {
      std::wstring child = fd.cFileName;
      if (child == L"." || child == L"..") continue;
      if (util::IEqualsNoCase(child, htmlName)) continue;
      if (sourceInsideLibrary) continue;
      if (util::ToLower(util::FileName(child)).rfind(util::ToLower(stem), 0) != 0) continue;
      util::CopyPathDeep(util::JoinPath(srcDir, child), util::JoinPath(destDir, child));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }

  LibraryEntry entry;
  entry.name = name;
  entry.dir = destDir;
  entry.htmlFile = htmlName;
  entry.origPath = util::Normalize(sourceHtml);
  entry.added = NowStamp();
  entry.sourceSize = util::FormatBytes(util::PathSizeRecursive(destDir));

  std::string manifest;
  manifest += "name=" + util::WideToUtf8(entry.name) + "\n";
  manifest += "html=" + util::WideToUtf8(entry.htmlFile) + "\n";
  manifest += "orig=" + util::WideToUtf8(entry.origPath) + "\n";
  manifest += "added=" + util::WideToUtf8(entry.added) + "\n";
  manifest += "size=" + util::WideToUtf8(entry.sourceSize) + "\n";
  util::WriteFileUtf8(util::JoinPath(destDir, L"manifest.txt"), manifest);

  if (entryOut) *entryOut = entry;
  return true;
}
