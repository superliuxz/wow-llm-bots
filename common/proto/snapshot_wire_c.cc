// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Minimal C ABI around wlb::snapshot BEVE <-> JSON for Python dev/e2e tools
// (tools/snapshot/fake_harness.py, tools/bench/capture_snapshots.py) loaded via
// Python stdlib ctypes — no protobuf runtime or virtualenv needed.

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "proto/snapshot.h"

namespace {

char* DupBytes(const std::string& s, size_t* out_len) {
  *out_len = s.size();
  char* buf = static_cast<char*>(std::malloc(s.size() + 1));
  if (buf == nullptr) {
    return nullptr;
  }
  if (!s.empty()) {
    std::memcpy(buf, s.data(), s.size());
  }
  buf[s.size()] = '\0';
  return buf;
}

}  // namespace

extern "C" {

// Decodes a BEVE ModuleMessage frame into compact JSON. Returns malloc'd buffer
// (free with wlb_wire_free), or nullptr if `bytes` is not a valid
// ModuleMessage.
char* wlb_module_message_beve_to_json(const char* bytes,
                                      size_t len,
                                      int pretty,
                                      size_t* out_len) {
  wlb::snapshot::ModuleMessage msg;
  if (!wlb::snapshot::ReadBeve(std::string_view(bytes, len), &msg)) {
    *out_len = 0;
    return nullptr;
  }
  const std::string json = wlb::snapshot::WriteJson(msg, pretty != 0);
  if (json.empty()) {
    *out_len = 0;
    return nullptr;
  }
  return DupBytes(json, out_len);
}

// Encodes a JSON Command into BEVE wire bytes. Returns malloc'd buffer (free
// with wlb_wire_free), or nullptr if `json` is invalid.
char* wlb_command_json_to_beve(const char* json, size_t len, size_t* out_len) {
  wlb::snapshot::Command cmd;
  if (!wlb::snapshot::ReadJson(std::string_view(json, len), &cmd)) {
    *out_len = 0;
    return nullptr;
  }
  // Tools speak for this checkout's harness: its version.
  cmd.version = wlb::snapshot::kWireVersion;
  return DupBytes(wlb::snapshot::WriteBeve(cmd), out_len);
}

void wlb_wire_free(void* ptr) {
  std::free(ptr);
}

}  // extern "C"
