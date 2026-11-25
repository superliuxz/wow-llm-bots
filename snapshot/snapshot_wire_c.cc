// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Minimal C ABI around wlb::snapshot BEVE <-> JSON for Python dev/e2e tools
// (tools/wlblib/wire.py: `wlb snap fake` / `wlb snap capture`) loaded via
// Python stdlib ctypes — no protobuf runtime or virtualenv needed.

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "snapshot/snapshot.h"

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

// Decodes a BEVE Command frame into compact JSON. Returns malloc'd buffer
// (free with wlb_wire_free), or nullptr if `bytes` is not a valid Command.
char* wlb_command_beve_to_json(const char* bytes, size_t len, size_t* out_len) {
  wlb::snapshot::Command cmd;
  if (!wlb::snapshot::ReadBeve(std::string_view(bytes, len), &cmd)) {
    *out_len = 0;
    return nullptr;
  }
  const std::string json = wlb::snapshot::WriteJson(cmd, /*pretty=*/false);
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

// The visibility errors (ValidateSnapshotVisibility) of a BEVE ModuleMessage
// frame's snapshot, one per line; "" when it has none or carries no snapshot.
// Returns a malloc'd buffer (free with wlb_wire_free), or nullptr if `bytes`
// is not a valid ModuleMessage. The e2e runs check every frame the module
// sends with it.
char* wlb_module_message_visibility_errors(const char* bytes,
                                           size_t len,
                                           size_t* out_len) {
  wlb::snapshot::ModuleMessage msg;
  if (!wlb::snapshot::ReadBeve(std::string_view(bytes, len), &msg)) {
    *out_len = 0;
    return nullptr;
  }
  std::string errors;
  if (msg.snapshot) {
    for (const std::string& e :
         wlb::snapshot::ValidateSnapshotVisibility(*msg.snapshot)) {
      errors += e;
      errors += '\n';
    }
  }
  return DupBytes(errors, out_len);
}

// This build's hello for `role`, as the JSON of a Command that carries only
// it ({"version": .., "hello": {"wire_version", "schema_hash", "role"}}): what
// a tool sends first, and what it checks the module's hello against. Returns
// a malloc'd buffer (free with wlb_wire_free).
char* wlb_hello_command_json(const char* role, size_t len, size_t* out_len) {
  wlb::snapshot::Command cmd;
  cmd.version = wlb::snapshot::kWireVersion;
  cmd.hello = wlb::snapshot::MakeHello(std::string_view(role, len));
  return DupBytes(wlb::snapshot::WriteJson(cmd, /*pretty=*/false), out_len);
}

void wlb_wire_free(void* ptr) {
  std::free(ptr);
}

}  // extern "C"
