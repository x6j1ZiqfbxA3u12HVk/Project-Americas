// Console paths onto host folders: what each resolves to, and what is refused.

#include "runtime/files/console_paths.h"
#include "test.h"

namespace {

using namespace am::rt::files;
namespace fs = std::filesystem;

PathMap make() { return PathMap({L"C:\\Game\\Dump", L"C:\\Out\\userdata\\persistent", L"C:\\Out\\userdata\\temp"}); }

}  // namespace

TEST_CASE(files_console_drives_and_relative_paths) {
  const PathMap m = make();
  Resolved r = m.resolve(L"S:\\saves\\slot1.bin");
  CHECK(r.area == Area::Persistent && r.host == fs::path(L"C:\\Out\\userdata\\persistent\\saves\\slot1.bin") && r.writable());
  r = m.resolve(L"t:/cache/x.tmp");
  CHECK(r.area == Area::Temp && r.host == fs::path(L"C:\\Out\\userdata\\temp\\cache\\x.tmp"));
  r = m.resolve(L"G:\\levels.rpf");
  CHECK(r.area == Area::Title && r.host == fs::path(L"C:\\Game\\Dump\\levels.rpf") && !r.writable());
  r = m.resolve(L"data\\ttdb\\a.ttpf");  // relative: from the title's folder
  CHECK(r.area == Area::Title && r.host == fs::path(L"C:\\Game\\Dump\\data\\ttdb\\a.ttpf"));
  r = m.resolve(L"C:\\game\\dump\\Audio.rpf");  // the dump's own host path, any case
  CHECK(r.area == Area::Title);
  r = m.resolve(L"\\\\?\\C:\\Game\\Dump\\x");
  CHECK(r.area == Area::Title);
}

TEST_CASE(files_refusals) {
  const PathMap m = make();
  CHECK(m.resolve(L"C:\\Windows\\System32\\drivers\\etc\\hosts").area == Area::Denied);
  CHECK(m.resolve(L"..\\outside.txt").area == Area::Denied);  // climbs out of the title's folder
  CHECK(m.resolve(L"S:\\..\\..\\x").area == Area::Denied);
  CHECK(m.resolve(L"\\\\server\\share\\x").area == Area::Denied);
  CHECK(m.resolve(L"\\\\.\\PhysicalDrive0").area == Area::Denied);
  CHECK(m.resolve(L"\\rooted").area == Area::Denied);
  CHECK(m.resolve(L"D:\\LocalDumps\\x.dmp").area == Area::Denied);
  CHECK(m.resolve(L"").area == Area::Denied);
  // A final path is checked against the area its name resolved to.
  const Resolved title = m.resolve(L"G:\\a.rpf");
  CHECK(m.inside(title, L"C:\\Game\\Dump\\a.rpf") && !m.inside(title, L"C:\\Elsewhere\\a.rpf"));
}

TEST_CASE(files_current_directory) {
  PathMap m = make();
  CHECK(m.set_current(m.resolve(L"S:\\profile")));
  const Resolved r = m.resolve(L"settings.xml");
  CHECK(r.area == Area::Persistent && r.host == fs::path(L"C:\\Out\\userdata\\persistent\\profile\\settings.xml"));
  CHECK(!m.set_current(m.resolve(L"C:\\Windows")));
}
