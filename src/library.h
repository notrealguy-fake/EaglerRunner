// library.h - the persistent "my clients" library.
//
// Saving a client copies the .html file (plus sibling files that share its base
// name, e.g. client.epk / client_files/) into
//   %LOCALAPPDATA%\EaglerRunner\library\<name>\
// so the client keeps working after the original file is deleted or moved.
#pragma once

#include <string>
#include <vector>

struct LibraryEntry {
  std::wstring name;      // user-visible name
  std::wstring dir;       // library\<name>
  std::wstring htmlFile;  // file name of the html inside dir (usually client.html)
  std::wstring origPath;  // where it was imported from
  std::wstring added;     // timestamp, human readable
  std::wstring sourceSize;// saved size, human readable
};

std::wstring              LibraryRoot();
std::vector<LibraryEntry> LibraryList();
bool                      LibrarySave(const std::wstring& sourceHtml, const std::wstring& name,
                                      std::wstring& errorOut, LibraryEntry* entryOut = nullptr);
// Copy a whole folder (e.g. an unpacked download) into the library.
// htmlRelName is the client HTML path relative to sourceDir.
bool                      LibrarySaveFolder(const std::wstring& sourceDir,
                                            const std::wstring& htmlRelName,
                                            const std::wstring& name,
                                            std::wstring& errorOut,
                                            LibraryEntry* entryOut = nullptr);
bool                      LibraryDelete(const std::wstring& name);
bool                      LibraryFind(const std::wstring& name, LibraryEntry& out);
std::wstring              LibraryHtmlPath(const LibraryEntry& entry);
