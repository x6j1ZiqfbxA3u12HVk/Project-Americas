// The console's audio stream processors:
//
// * xapu.dll, the audio decoder: a title connects once for one kind of processing (Opus
//   decoding), gets a block of memory for its packets and output, queues per-stream commands
//   (activate, process, reset, deactivate, notify) and dequeues one result per command, in
//   command order. Each command completes when it is queued.
// * chad.dll, the audio DSP: block transforms (convolution reverb and others) on streams of
//   float blocks. Activation hands back a status block the title polls (submitted, completed,
//   result); a command completes when it is submitted.
//
// Not done yet: the Opus decoder and the DSP's transforms. Process commands and DSP blocks
// write silence of the length asked for, so streams keep moving but play nothing.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::hle {
namespace {

constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057);
constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT kNotImplemented = static_cast<HRESULT>(0x80004001);
constexpr HRESULT kPointer = static_cast<HRESULT>(0x80004003);
constexpr HRESULT kQueueFull = static_cast<HRESULT>(0x8AC81000);
constexpr HRESULT kPendingResults = static_cast<HRESULT>(0x8AC81001);
constexpr HRESULT kInvalidCommand = static_cast<HRESULT>(0x8AC81002);
constexpr HRESULT kInvalidStream = static_cast<HRESULT>(0x8AC81003);
constexpr HRESULT kAlreadyActive = static_cast<HRESULT>(0x8AC81005);

// ---- the decoder (xapu) ----

constexpr uint32_t kDecodeOpus = 1;
constexpr uint32_t kActivate = 1, kDeactivate = 2, kProcess = 3, kNotify = 4, kReset = 5;
constexpr uint32_t kSignalOption = 1;                                // command: signal the result event
constexpr uint32_t kDeinterleavedOutput = 4, kResultEventOption = 8;  // connect

#pragma pack(push, 1)
struct ConnectInput {
  uint32_t process, streams, commands_per_stream, memory, frames_per_output, options;
};
struct ConnectOutput {
  void* memory;
  HANDLE result_event;
};
struct CommandId {
  uint32_t kind, stream, sequence, options;
  uint64_t context;
};
struct Activate {
  CommandId id;
  uint32_t channels, init_length;
  const void* init;
  float start_pitch;
  uint32_t processing_length;
  void* processing;
};
struct Process {
  CommandId id;
  float pitch_ramp, target_pitch;
  uint32_t first_frame, frame_count, input_length;
  const uint8_t* input;
  uint32_t output_length;
  uint8_t* output;
};
struct ApuResult {
  CommandId id;
  uint32_t state;
  uint64_t state_data, usec;
  HRESULT error;
  uint32_t frames, output_length;
  uint8_t* output;
  float pitch;
  uint32_t input_length;
  const uint8_t* input;
};
#pragma pack(pop)
static_assert(sizeof(ApuResult) == 0x50);
static_assert(sizeof(Process) == 64);

struct Connection {
  std::vector<uint8_t> memory;
  HANDLE event = nullptr;
  uint32_t options = 0, per_stream = 0;
  std::mutex lock;
  std::vector<uint32_t> channels;  // per stream; 0: not active
  std::vector<uint32_t> queued;    // results not dequeued yet, per stream
  std::deque<ApuResult> results;
  uint32_t commands = 0, dequeued = 0;
};

HRESULT WINAPI my_XApuConnect(const ConnectInput* in, ConnectOutput* out, Connection** handle) {
  if (!in || !out || !handle) return kInvalidArg;
  if (in->streams < 1 || in->streams > 500 || in->commands_per_stream < 1 || in->commands_per_stream > 16 || !in->memory) {
    return kInvalidArg;
  }
  logf("audio: decoder connect (process %u, %u streams, %u commands each, %u KiB, options 0x%x)", in->process, in->streams,
       in->commands_per_stream, in->memory >> 10, in->options);
  if (in->process != kDecodeOpus) return kNotImplemented;  // rate conversion: not done
  if (in->frames_per_output) return kInvalidArg;
  Connection* c = new (std::nothrow) Connection;
  if (!c) return kOutOfMemory;
  c->memory.assign(in->memory + 16, 0);
  c->options = in->options;
  c->per_stream = in->commands_per_stream;
  c->channels.assign(in->streams, 0);
  c->queued.assign(in->streams, 0);
  if (in->options & kResultEventOption) c->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  out->memory = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(c->memory.data()) + 15) & ~uintptr_t{15});
  out->result_event = c->event;
  *handle = c;
  return S_OK;
}

HRESULT WINAPI my_XApuDisconnect(Connection* c) {
  if (!c) return kInvalidArg;
  if (c->event) CloseHandle(c->event);
  delete c;
  return S_OK;
}

// Silence for the frames the command asks for, as many as its output holds.
void process_silence(uint32_t channels, uint32_t options, const Process& p, ApuResult& r) {
  r.input = p.input;
  r.input_length = p.input_length;
  r.output = p.output;
  if (!p.input || !p.output || !p.input_length) {
    r.error = kInvalidArg;
    return;
  }
  const uint32_t frames = std::min<uint32_t>(p.frame_count, p.output_length / (4 * channels));
  std::memset(p.output, 0, size_t{frames} * channels * 4);
  (void)options;  // interleaved or not, silence looks the same
  r.frames = frames;
  r.output_length = frames * channels * 4;
}

HRESULT WINAPI my_XApuEnqueueCommand(Connection* c, const CommandId* command, UINT32* total) {
  if (!c || !command) return kInvalidArg;
  CommandId id;
  std::memcpy(&id, command, sizeof id);
  std::lock_guard g(c->lock);
  if (id.stream >= c->channels.size()) return kInvalidStream;
  if (c->queued[id.stream] >= c->per_stream) return kQueueFull;
  ApuResult r{};
  r.id = id;
  r.pitch = 1.0f;
  uint32_t& channels = c->channels[id.stream];
  switch (id.kind) {
    case kActivate: {
      Activate a;
      std::memcpy(&a, command, sizeof a);
      if (channels) r.error = kAlreadyActive;
      else if (a.channels < 1 || a.channels > 2) r.error = kInvalidArg;
      else channels = a.channels;
      break;
    }
    case kDeactivate: channels = 0; break;
    case kReset:
    case kNotify: break;
    case kProcess: {
      Process p;
      std::memcpy(&p, command, sizeof p);
      if (!channels) r.error = kInvalidStream;
      else process_silence(channels, c->options, p, r);
      break;
    }
    default: return kInvalidCommand;
  }
  ++c->queued[id.stream];
  ++c->commands;
  c->results.push_back(r);
  if (total) *total = c->commands;
  if ((id.options & kSignalOption) && c->event) SetEvent(c->event);
  return S_OK;
}

HRESULT WINAPI my_XApuDequeueResult(Connection* c, ApuResult* out) {
  if (!c || !out) return kInvalidArg;
  std::lock_guard g(c->lock);
  if (c->results.empty()) return kPendingResults;
  const ApuResult r = c->results.front();
  c->results.pop_front();
  --c->queued[r.id.stream];
  ++c->dequeued;
  std::memcpy(out, &r, sizeof r);
  return S_OK;
}

UINT32 WINAPI my_XApuGetResultCount(Connection* c, UINT32* total) {
  if (!c) return 0;
  std::lock_guard g(c->lock);
  if (total) *total = c->dequeued;
  return static_cast<UINT32>(c->results.size());
}

// ---- the DSP (chad) ----

#pragma pack(push, 1)
struct StreamDesc {
  uint32_t kind, options, frames, channels, ir_floats;
  const float* ir;
};
#pragma pack(pop)
struct DspStatus {
  volatile uint32_t submitted, completed, reserved;
  volatile HRESULT result;
};
struct DspCommand {
  float gains[2];
  const float* input;
  float* output;
  uint64_t reserved;
};
struct Dsp {
  uint32_t max_streams = 0;
  std::mutex lock;
  uint32_t active = 0;
};
struct DspStream {
  Dsp* dsp = nullptr;
  uint32_t frames = 0, channels = 0;
  DspStatus status{};
};

constexpr uint32_t kConvolution = 1;

HRESULT WINAPI my_XDspConnectWithMaximumStreamLimit(void* base, UINT32 base_length, UINT32 ir_seconds, UINT32 max_streams,
                                                    UINT32 max_channels, Dsp** out) {
  if (!base || base_length < 0x10000 || base_length > 0x1F400000 || ir_seconds > 0x80 || max_streams > 0x100 || max_channels > 2) {
    return kInvalidArg;
  }
  if (!out) return kPointer;
  logf("audio: DSP connect (%u KiB, %u s of impulse response, %u streams, %u channels)", base_length >> 10, ir_seconds,
       max_streams, max_channels);
  Dsp* d = new Dsp;
  d->max_streams = max_streams;
  *out = d;
  return S_OK;
}

HRESULT WINAPI my_XDspDisconnect(Dsp* d) {
  if (!d) return kInvalidArg;
  delete d;
  return S_OK;
}

bool valid_stream(const StreamDesc& s) {
  bool shape = s.kind == kConvolution ? (s.channels >= 1 && s.channels <= 2 && (s.frames == 0x200 || s.frames == 0x400) && !(s.options & 4))
                                      : (s.channels == 1 && (s.frames == 0x200 || s.frames == 0x400 || s.frames == 0x800));
  return shape && (!(s.options & (2 | 8)) || s.channels == 2);  // those options need a stereo stream
}

HRESULT WINAPI my_XDspActivate(Dsp* d, const StreamDesc* desc, const DspStatus** status, DspStream** stream) {
  if (!status || !stream) return kPointer;
  if (!d || !desc) return kInvalidArg;
  StreamDesc s;
  std::memcpy(&s, desc, sizeof s);
  if (!valid_stream(s)) return kInvalidArg;
  {
    std::lock_guard g(d->lock);
    if (d->active >= d->max_streams) return kOutOfMemory;
    ++d->active;
  }
  DspStream* n = new DspStream;
  n->dsp = d;
  n->frames = s.frames;
  n->channels = s.channels;
  *status = &n->status;
  *stream = n;
  return S_OK;
}

HRESULT WINAPI my_XDspDeactivate(DspStream* s) {
  if (!s) return kInvalidArg;
  {
    std::lock_guard g(s->dsp->lock);
    --s->dsp->active;
  }
  delete s;
  return S_OK;
}

// Completes at once; the block written is silence (no transform yet).
HRESULT WINAPI my_XDspSubmitCommand(DspStream* s, const DspCommand* command, UINT32* ticket) {
  if (!s || !command) return kInvalidArg;
  if (command->output) std::memset(command->output, 0, size_t{s->frames} * s->channels * sizeof(float));
  const uint32_t next = s->status.submitted + 1;
  s->status.submitted = next;
  s->status.result = S_OK;
  s->status.completed = next;
  if (ticket) *ticket = next;
  return S_OK;
}

#define AM_XAPU(name) Function{"xapu.dll", #name, reinterpret_cast<void*>(&my_##name)}
#define AM_CHAD(name) Function{"chad.dll", #name, reinterpret_cast<void*>(&my_##name)}

}  // namespace

void add_audio_streams(std::vector<Function>& out) {
  out.insert(out.end(), {
                            AM_XAPU(XApuConnect),
                            AM_XAPU(XApuDisconnect),
                            AM_XAPU(XApuEnqueueCommand),
                            AM_XAPU(XApuDequeueResult),
                            AM_XAPU(XApuGetResultCount),
                            AM_CHAD(XDspConnectWithMaximumStreamLimit),
                            AM_CHAD(XDspDisconnect),
                            AM_CHAD(XDspActivate),
                            AM_CHAD(XDspDeactivate),
                            AM_CHAD(XDspSubmitCommand),
                        });
}

}  // namespace am::rt::hle
