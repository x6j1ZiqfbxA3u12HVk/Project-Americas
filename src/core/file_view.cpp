#include "core/file_view.h"

#include <windows.h>

#include <utility>

#include "core/text.h"

namespace am {

std::string win32_error_text(unsigned long code) {
  wchar_t* buf = nullptr;
  const DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
      reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::string text = n ? narrow(std::wstring_view(buf, n)) : std::string("unknown error");
  if (buf) LocalFree(buf);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
    text.pop_back();
  }
  return text + " (error " + std::to_string(code) + ")";
}

std::optional<FileView> FileView::open(const std::filesystem::path& path, std::string& error) {
  FileView view;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = win32_error_text(GetLastError());
    return std::nullopt;
  }
  view.file_ = file;

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) {
    error = win32_error_text(GetLastError());
    return std::nullopt;
  }
  view.size_ = static_cast<uint64_t>(size.QuadPart);
  if (view.size_ == 0) return std::optional<FileView>(std::move(view));  // nothing to map

  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) {
    error = win32_error_text(GetLastError());
    return std::nullopt;
  }
  view.mapping_ = mapping;

  const void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!data) {
    error = win32_error_text(GetLastError());
    return std::nullopt;
  }
  view.data_ = static_cast<const uint8_t*>(data);
  return std::optional<FileView>(std::move(view));
}

FileView::FileView(FileView&& other) noexcept { *this = std::move(other); }

FileView& FileView::operator=(FileView&& other) noexcept {
  if (this != &other) {
    reset();
    file_ = std::exchange(other.file_, nullptr);
    mapping_ = std::exchange(other.mapping_, nullptr);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

FileView::~FileView() { reset(); }

void FileView::reset() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(mapping_);
  if (file_) CloseHandle(file_);
  data_ = nullptr;
  mapping_ = nullptr;
  file_ = nullptr;
  size_ = 0;
}

}  // namespace am
