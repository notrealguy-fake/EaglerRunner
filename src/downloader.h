// downloader.h - Eaglercraft version downloader (native side)
//
// Fetches the manifest from eaglercraft.com/downloads, downloads selected versions,
// extracts zips, and adds them to the launcher library.

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

struct DownloaderVersion {
  std::string id;           // "1.8.8-js", "1.12.2-wasm", "26.2"
  std::string displayName;  // "EaglercraftX 1.8.8 (JS)"
  std::string mcVersion;    // "1.8.8", "1.12.2", "26.2"
  std::string type;         // "js" or "wasm" or "zip"
  std::string url;          // direct CDN download URL
  std::string size;         // human-readable size (optional)
};

struct DownloadProgress {
  std::string id;
  int percent = 0;
  std::string status;       // "downloading" | "extracting" | "adding" | "done"
  std::string error;
  std::string resultPath;   // final library html path on success
};

class Downloader {
 public:
  using ProgressCallback = std::function<void(const DownloadProgress&)>;
  using ManifestCallback = std::function<void(const std::vector<DownloaderVersion>&, const std::string& error)>;

  Downloader();
  ~Downloader();

  // Fetch the version manifest (async, calls cb when done)
  void FetchManifest(ManifestCallback cb);

  // Download a version by ID (async, calls progressCb on updates)
  void DownloadVersion(const std::string& versionId, ProgressCallback progressCb);

  // Cancel any ongoing operation
  void Cancel();

 private:
  void FetchManifestThread(ManifestCallback cb);
  void DownloadThread(const std::string& versionId, ProgressCallback progressCb);
  bool DownloadFile(const std::string& url, const std::wstring& destPath, ProgressCallback cb);
  bool ExtractZip(const std::wstring& zipPath, const std::wstring& destDir, ProgressCallback cb);
  std::wstring FindHtmlInDir(const std::wstring& dir);
  bool ImportIntoLibrary(const std::wstring& sourceDir, const std::wstring& htmlRelName,
                         const std::string& versionId, std::wstring& outPath, std::string& errorOut);

  std::thread m_thread;
  std::atomic<bool> m_cancel{false};
  std::atomic<bool> m_busy{false};
};