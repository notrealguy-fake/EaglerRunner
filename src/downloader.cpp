// downloader.cpp - Eaglercraft version downloader implementation
#include "downloader.h"

#include "util.h"
#include "library.h"

#include <urlmon.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr const char* kUserAgent = "EaglerRunner/1.0 (+https://github.com/EaglerRunner)";

// Hardcoded manifest as fallback (in case the site structure changes)
const std::vector<DownloaderVersion> kFallbackManifest = {
  {"1.8.8-js", "EaglercraftX 1.8.8 (JS)", "1.8.8", "js",
   "https://cdn.eaglercraft.net/objects/dl/1.8.8/eaglercraft1-8-62da5f39.zip", "~40 MB"},
  {"1.8.8-wasm", "EaglercraftX 1.8.8 (WASM-GC)", "1.8.8", "wasm",
   "https://cdn.eaglercraft.net/objects/dl/1.8.8/eaglercraft1-8-wasm-62da5f39.zip", "~42 MB"},
  {"1.12.2-js", "Eaglercraft 1.12.2 (JS)", "1.12.2", "js",
   "https://cdn.eaglercraft.net/objects/dl/1.12.2/eaglercraft1-12-62da5f39.zip", "~45 MB"},
  {"1.12.2-wasm", "Eaglercraft 1.12.2 (WASM-GC)", "1.12.2", "wasm",
   "https://cdn.eaglercraft.net/objects/dl/1.12.2/eaglercraft1-12-wasm-62da5f39.zip", "~47 MB"},
  {"26.2", "Eaglercraft 26.2 (zeus)", "26.2", "zip",
   "https://cdn.eaglercraft.net/objects/dl/26.2/eaglercraft26-2-e33db4be.zip", "~55 MB"},
  {"1.5.2", "Eaglercraft 1.5.2 (sp2.01)", "1.5.2", "zip",
   "https://cdn.eaglercraft.net/objects/dl/1.5.2/Eaglercraft_1.5.2-sp2.01_Offline.zip", "~30 MB"},
};

bool HttpDownloadToFile(const std::string& url, const std::wstring& destPath,
                        std::function<void(int)> progressCb = nullptr) {
  if (!util::EnsureDir(util::ParentDir(destPath))) return false;

  // Use URLDownloadToFile with a custom bind status callback for progress
  struct BindStatusCallback : IBindStatusCallback {
    std::function<void(int)> progressCb;
    DWORD lastPercent = 0;
    DWORD totalSize = 0;
    DWORD downloaded = 0;
    bool cancelled = false;

    STDMETHOD(OnProgress)(ULONG ulProgress, ULONG ulProgressMax, ULONG ulStatusCode, LPCWSTR) override {
      if (cancelled) return E_ABORT;
      if (ulProgressMax > 0 && progressCb) {
        DWORD pct = (ulProgress * 100) / ulProgressMax;
        if (pct != lastPercent) {
          lastPercent = pct;
          progressCb((int)pct);
        }
      }
      return S_OK;
    }

    STDMETHOD(OnStartBinding)(DWORD, IBinding*) override { return S_OK; }
    STDMETHOD(GetPriority)(LONG*) override { return E_NOTIMPL; }
    STDMETHOD(OnLowResource)(DWORD) override { return S_OK; }
    STDMETHOD(OnStopBinding)(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHOD(GetBindInfo)(DWORD*, BINDINFO*) override { return E_NOTIMPL; }
    STDMETHOD(OnDataAvailable)(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHOD(OnObjectAvailable)(REFIID, IUnknown*) override { return S_OK; }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
      if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
        *ppv = static_cast<IBindStatusCallback*>(this);
        AddRef();
        return S_OK;
      }
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 2; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
  };

  BindStatusCallback callback;
  callback.progressCb = std::move(progressCb);

  HRESULT hr = URLDownloadToFileW(nullptr, util::Utf8ToWide(url).c_str(), destPath.c_str(), 0, &callback);
  return SUCCEEDED(hr);
}

}  // namespace
Downloader::Downloader() {}

Downloader::~Downloader() { Cancel(); }

void Downloader::Cancel() {
  m_cancel = true;
  if (m_thread.joinable()) m_thread.join();
  m_busy = false;
}

void Downloader::FetchManifest(ManifestCallback cb) {
  if (m_busy) { 
    if (cb) cb({}, "busy"); 
    return; 
  }
  // Reap the previous worker first: assigning to a joinable std::thread would
  // call std::terminate() and kill the whole app.
  if (m_thread.joinable()) m_thread.join();
  m_busy = true; 
  m_cancel = false;
  m_thread = std::thread([this, cb = std::move(cb)]() mutable { FetchManifestThread(std::move(cb)); });
}

void Downloader::FetchManifestThread(ManifestCallback cb) {
  // Use fallback manifest (reliable, no JS parsing needed)
  if (cb) cb(kFallbackManifest, "");
  m_busy = false;
}

void Downloader::DownloadVersion(const std::string& versionId, ProgressCallback progressCb) {
  if (m_busy) { 
    if (progressCb) {
      DownloadProgress dp; dp.id = versionId; dp.percent = 0; dp.status = "error"; dp.error = "busy";
      progressCb(dp);
    }
    return; 
  }
  // See FetchManifest: never assign over a still-joinable thread.
  if (m_thread.joinable()) m_thread.join();
  m_busy = true; 
  m_cancel = false;
  m_thread = std::thread([this, versionId, progressCb = std::move(progressCb)]() mutable { 
    DownloadThread(versionId, std::move(progressCb)); 
  });
}

void Downloader::DownloadThread(const std::string& versionId, ProgressCallback progressCb) {
  // Find the version
  const DownloaderVersion* ver = nullptr;
  for (const auto& v : kFallbackManifest) {
    if (v.id == versionId) { ver = &v; break; }
  }
  if (!ver) {
    if (progressCb) {
      DownloadProgress dp; dp.id = versionId; dp.percent = 0; dp.status = "error"; dp.error = "unknown version";
      progressCb(dp);
    }
    m_busy = false;
    return;
  }

  auto report = [&](int pct, const std::string& status, const std::string& err = "", const std::string& result = "") {
    if (progressCb && !m_cancel) {
      DownloadProgress dp; dp.id = versionId; dp.percent = pct; dp.status = status; dp.error = err; dp.resultPath = result;
      progressCb(dp);
    }
  };

  report(0, "downloading");
  std::wstring tempDir = util::JoinPath(util::DataDir(), L"downloads");
  util::EnsureDir(tempDir);
  std::wstring zipPath = util::JoinPath(tempDir, util::Utf8ToWide(ver->id + ".zip"));

  if (!HttpDownloadToFile(ver->url, zipPath, [&](int pct) {
    if (!m_cancel) report(pct, "downloading");
  })) {
    report(0, "error", "download failed");
    m_busy = false;
    return;
  }
  if (m_cancel) { m_busy = false; return; }

  // 2) unpack the archive and move the client into the library.
  report(100, "extracting");
  std::wstring extractDir = util::JoinPath(tempDir, util::Utf8ToWide(ver->id) + L"_unpacked");
  if (util::DirExists(extractDir)) util::RemovePathDeep(extractDir);

  if (ExtractZip(zipPath, extractDir, progressCb) && !m_cancel) {
    std::wstring html = FindHtmlInDir(extractDir);
    if (!html.empty()) {
      std::wstring rel = html;
      if (rel.size() > extractDir.size() + 1) rel = rel.substr(extractDir.size() + 1);

      report(100, "adding");
      std::wstring outPath;
      std::string err;
      if (ImportIntoLibrary(extractDir, rel, versionId, outPath, err) && !outPath.empty()) {
        report(100, "done", "", util::WideToUtf8(outPath));
        // The library holds a full copy now - clean up the temporary files.
        util::RemovePathDeep(extractDir);
        DeleteFileW(zipPath.c_str());
        m_busy = false;
        return;
      }
    }
  }

  // Extraction was not possible (or the archive had no HTML in it) - keep the
  // zip on disk so the client can still be unpacked manually.
  report(100, "downloaded", "", util::WideToUtf8(zipPath));
  m_busy = false;
}

std::wstring Downloader::FindHtmlInDir(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(util::JoinPath(dir, L"*.html").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    std::wstring result = util::JoinPath(dir, fd.cFileName);
    FindClose(h);
    return result;
  }
  // Recurse into subdirs
  h = FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring sub = FindHtmlInDir(util::JoinPath(dir, name));
        if (!sub.empty()) { FindClose(h); return sub; }
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return L"";
}

// Windows ships bsdtar (tar.exe) since Windows 10 1803, and bsdtar reads zip
// files, so no third-party unzip library is needed.
bool Downloader::ExtractZip(const std::wstring& zipPath, const std::wstring& destDir,
                            ProgressCallback) {
  if (!util::FileExists(zipPath)) return false;
  if (!util::EnsureDir(destDir)) return false;

  wchar_t systemDir[MAX_PATH] = {0};
  if (!GetSystemDirectoryW(systemDir, MAX_PATH)) return false;
  std::wstring tarExe = std::wstring(systemDir) + L"\\tar.exe";
  if (!util::FileExists(tarExe)) return false;

  std::wstring cmdLine = L"\"" + tarExe + L"\" -xf \"" + zipPath + L"\" -C \"" + destDir + L"\"";
  std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
  buffer.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return exitCode == 0;
}

bool Downloader::ImportIntoLibrary(const std::wstring& sourceDir, const std::wstring& htmlRelName,
                                   const std::string& versionId, std::wstring& outPath,
                                   std::string& errorOut) {
  // Human-readable library name from the manifest entry.
  std::wstring name;
  for (const auto& v : kFallbackManifest) {
    if (v.id == versionId) {
      name = util::Utf8ToWide(v.displayName);
      break;
    }
  }
  if (name.empty()) name = util::Utf8ToWide(versionId);

  LibraryEntry entry;
  std::wstring err;
  if (!LibrarySaveFolder(sourceDir, htmlRelName, name, err, &entry)) {
    errorOut = util::WideToUtf8(err);
    return false;
  }
  outPath = LibraryHtmlPath(entry);
  return true;
}