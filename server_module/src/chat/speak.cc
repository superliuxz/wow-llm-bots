// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/speak.h"

#include <cstddef>
#include <string>

#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// The client's own limit for one chat line.
constexpr size_t kMaxChatChars = 255;

}  // namespace

std::string Speak(Player* bot, const pb::Command::Say& say) {
  const std::string text = say.text.substr(0, kMaxChatChars);
  if (text.empty()) {
    return "nothing to say";
  }
  // Out loud a player speaks their race's language; whispers are always
  // readable (Player::Whisper makes them so).
  const Language language =
      bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
  switch (say.channel) {
    case pb::SAY:
      bot->Say(text, language);
      return "";
    case pb::YELL:
      bot->Yell(text, language);
      return "";
    case pb::WHISPER: {
      std::string name = say.to;
      Player* to = normalizePlayerName(name)
                       ? ObjectAccessor::FindPlayerByName(name)
                       : nullptr;
      if (to == nullptr || !to->IsInWorld()) {
        return "no player named '" + say.to + "' is currently playing";
      }
      if (to == bot) {
        return "you can't whisper yourself";
      }
      bot->Whisper(text, LANG_UNIVERSAL, to);
      return "";
    }
    default:
      return "you can't talk on " + std::string(pb::EnumName(say.channel)) +
             " yet";
  }
}

}  // namespace llm_brain
