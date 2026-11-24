#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace am {

// A whole file mapped read-only into memory. The file is opened for reading only and other
// openers may only read it too, so nothing can modify it through or while this view exists.
class FileView {
 public:
  static std::optional<FileView> open(const std::filesystem::path& path, std::string& error);

  FileView(FileView&& other) noexcept;
  FileView& operator=(FileView&& other) noexcept;
  FileView(const FileView&) = delete;
  FileView& operator=(const FileView&) = delete;
  ~FileView();

  std::span<const uint8_t> bytes() const { return {data_, static_cast<size_t>(size_)}; }
  uint64_t size() const { return size_; }

 private:
  FileView() = default;
  void reset();

  void* file_ = nullptr;     // HANDLE
  void* mapping_ = nullptr;  // HANDLE
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
};

// The system's text for a Win32 error code, with the code appended.
std::string win32_error_text(unsigned long code);

}  // namespace am
