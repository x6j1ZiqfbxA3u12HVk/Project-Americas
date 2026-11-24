#include <string>

#include "core/json.h"
#include "core/paths.h"
#include "core/sha256.h"
#include "core/text.h"
#include "test.h"

using namespace am;

TEST_CASE(paths_inside_is_component_wise_and_ignores_case) {
  CHECK(paths::is_inside(L"E:/Games/Title/data/x.rpf", L"e:\\games\\title"));
  CHECK(paths::is_inside(L"E:\\Games\\Title", L"E:\\Games\\Title\\"));
  CHECK(!paths::is_inside(L"E:\\Games\\TitleX\\a", L"E:\\Games\\Title"));
  CHECK(!paths::is_inside(L"E:\\Games\\Title\\..\\Other", L"E:\\Games\\Title"));
  CHECK(!paths::is_inside(L"E:\\Games", L"E:\\Games\\Title"));
  CHECK(!paths::is_inside(L"C:\\Games\\Title", L"E:\\Games\\Title"));
  CHECK(paths::is_inside(L"C:\\anything", L"C:\\"));
}

TEST_CASE(sha256_matches_the_standard_vector) {
  const std::string abc = "abc";
  const auto digest = sha256({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()});
  CHECK(digest.has_value());
  if (digest) {
    CHECK(hex_bytes(digest->data(), digest->size()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  }
}

TEST_CASE(json_writer_nests_and_escapes) {
  JsonWriter j;
  j.begin_object();
  j.field("name", "a\"b\\c\n");
  j.field("count", 3);
  j.field("big", uint64_t{1} << 40);
  j.field("flag", true);
  j.key("empty").begin_array().end_array();
  j.key("list").begin_array().value(1).value("x").null().end_array();
  j.end_object();
  const std::string expected =
      "{\n"
      "  \"name\": \"a\\\"b\\\\c\\n\",\n"
      "  \"count\": 3,\n"
      "  \"big\": 1099511627776,\n"
      "  \"flag\": true,\n"
      "  \"empty\": [],\n"
      "  \"list\": [\n"
      "    1,\n"
      "    \"x\",\n"
      "    null\n"
      "  ]\n"
      "}";
  CHECK(j.str() == expected);
}

TEST_CASE(json_writer_keeps_utf8_and_escapes_other_high_bytes) {
  std::string out;
  JsonWriter::escape_into(out, "caf\xc3\xa9");
  CHECK(out == "caf\xc3\xa9");
  out.clear();
  JsonWriter::escape_into(out, "bad\xe9");
  CHECK(out == "bad\\u00e9");
}

TEST_CASE(text_helpers) {
  CHECK(iequals_ascii("KERNEL32.dll", "kernel32.DLL"));
  CHECK(istarts_with_ascii("API-MS-WIN-core-file-l1-1-0.dll", "api-ms-win-"));
  CHECK(trim("  a b \r\n") == "a b");
  CHECK(format_bytes(512) == "512 B");
  CHECK(format_bytes(1536) == "1.5 KiB");
  CHECK(hex(0x140000000ull) == "140000000");
  CHECK(hex(0xab, 4) == "00ab");
  CHECK(narrow(widen("Grüße")) == "Grüße");
}
