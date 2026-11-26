#pragma once

// BCPack: the console's lossless packing of BC block streams (DirectStorage unpacks it in
// hardware there). Decoding only; BC1, BC3, BC4, BC5, BC6H and BC7 (BC2 not yet).
//
// A packed stream has no header. It is 12 or 16 interleaved rANS lanes. Each lane is a run of
// fragments (a header byte naming one of 128 static models, a length, the payload). A fixed
// schedule decides which lane takes the next input byte. The lanes' symbols form one record per
// block: two back references (to one of the last 256 blocks, or to the format's dictionary
// standing in for blocks before the first), then the residuals of the endpoints and indices
// against them.
//
// The models, rank tables, BC7 control words and dictionaries are constants of the format. They
// are not ours and are not in the repository: Tables::load reads them from a file kept outside
// it (C:\Project Americas\tools\bcpack\tables.bin).

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace am::formats::bcpack {

struct TableData;

class Tables {
 public:
  ~Tables();
  // From the table file's bytes; nullptr (and `error`) when they are not a table file.
  static std::unique_ptr<Tables> load(std::span<const uint8_t> bytes, std::string& error);
  const TableData& data() const { return *data_; }

 private:
  Tables();
  std::unique_ptr<TableData> data_;
};

// The table file's size in bytes.
size_t table_file_size();

enum class Status : uint8_t {
  Ok,
  Unsupported,  // a BC format not done
  BadSize,      // the destination is not a whole number of blocks
  Corrupt,      // the stream ends early or holds codes the format does not have
};
const char* status_name(Status s);

// Unpacks `src` (BC format `bc`: 1, 3, 4, 5, 6, 7) into `dst`, whose size gives the block count.
// Bytes after the last one the lanes need are ignored.
Status decode(int bc, std::span<const uint8_t> src, std::span<uint8_t> dst, const Tables& tables);

}  // namespace am::formats::bcpack
