#include "shader/stage_io.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "shader/container.h"

namespace am::shader {
namespace {

struct Element {
  std::string semantic;
  uint32_t index = 0;
  uint32_t system_value = 0;  // 0: none
  uint32_t reg = 0;
  uint32_t mask = 0;
};

uint32_t u32(std::span<const uint8_t> d, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
  return v;
}

// ISG1 / OSG1: count, 8 (the elements' offset), then 32-byte elements: stream, name offset (from
// the part's start), semantic index, system value, component type, register, mask, precision.
std::vector<Element> read_signature(const Part* part) {
  std::vector<Element> out;
  if (!part) return out;
  const std::span<const uint8_t> d = part->data;
  const uint32_t count = u32(d, 0);
  for (uint32_t k = 0; k < count && k < 64; ++k) {
    const size_t at = 8 + size_t{k} * 32;
    if (at + 32 > d.size()) break;
    Element e;
    const uint32_t name = u32(d, at + 4);
    for (size_t i = name; i < d.size() && d[i]; ++i) e.semantic += static_cast<char>(d[i]);
    e.index = u32(d, at + 8);
    e.system_value = u32(d, at + 12);
    e.reg = u32(d, at + 20);
    e.mask = u32(d, at + 24) & 0xFF;
    out.push_back(e);
  }
  return out;
}

const Element* by_register(const std::vector<Element>& sig, uint32_t reg) {
  for (const Element& e : sig) {
    if (e.reg == reg && e.system_value == 0) return &e;
  }
  return nullptr;
}

}  // namespace

bool read_stage_io(std::span<const uint8_t> container, const NativeBlob& blob, const NativeProgram& program,
                   TranslateInput& in, std::string& error) {
  std::string why;
  const std::optional<Container> c = parse_container(container, why);
  if (!c) {
    error = why;
    return false;
  }
  const std::vector<Element> isg = read_signature(c->find("ISG1"));
  const std::vector<Element> osg = read_signature(c->find("OSG1"));
  const std::span<const uint8_t> record = blob.record(program);
  in.inputs.clear();
  in.outputs.clear();
  if (program.stage == HwStage::Vs) {
    for (size_t at = 0x78; at + 5 <= record.size() && in.inputs.size() < 32; at += 5) {
      const uint8_t type = record[at], reg = record[at + 1], vgpr = record[at + 3], comps = record[at + 4];
      if (!type) break;
      if (type != 6) {
        error = "vertex attribute type " + std::to_string(type);
        return false;
      }
      const Element* e = by_register(isg, reg);
      if (!e) {
        error = "vertex attribute register " + std::to_string(reg) + " not in the signature";
        return false;
      }
      in.inputs.push_back({e->semantic, e->index, static_cast<uint32_t>(comps) + 1, vgpr});
    }
    std::vector<Element> params;
    for (const Element& e : osg) {
      if (e.system_value == 0) params.push_back(e);
    }
    std::sort(params.begin(), params.end(), [](const Element& a, const Element& b) { return a.reg < b.reg; });
    for (const Element& e : params) in.outputs.push_back({e.semantic, e.index});
    return true;
  }
  if (program.stage == HwStage::Ps) {
    for (size_t at = 0x78; at + 8 <= record.size() && in.inputs.size() < 32; at += 8) {
      const uint8_t type = record[at], reg = record[at + 1], attr = record[at + 3];
      if (!type) break;
      const Element* e = by_register(isg, reg);
      if (!e) {
        error = "pixel attribute register " + std::to_string(reg) + " not in the signature";
        return false;
      }
      if (in.inputs.size() <= attr) in.inputs.resize(size_t{attr} + 1, StageInput{"AM_UNUSED", 0, 4, 0});
      in.inputs[attr] = {e->semantic, e->index, 4, 0};
    }
    for (size_t k = 0; k < in.inputs.size(); ++k) {
      if (in.inputs[k].semantic == "AM_UNUSED") in.inputs[k].semantic_index = static_cast<uint32_t>(k);
    }
    for (const Element& e : osg) {
      if (e.semantic != "SV_Target") continue;  // depth and coverage outputs come later
      if (in.outputs.size() <= e.index) in.outputs.resize(size_t{e.index} + 1, StageOutput{"SV_Target", 0});
      in.outputs[e.index] = {"SV_Target", e.index};
    }
    for (size_t k = 0; k < in.outputs.size(); ++k) in.outputs[k].semantic_index = static_cast<uint32_t>(k);
    return true;
  }
  return true;  // other stages have no interface here
}

}  // namespace am::shader
