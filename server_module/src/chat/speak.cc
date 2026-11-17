// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/speak.h"

#include <cstdint>
#include <string>

#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"
#include "chat/chat_text.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// GM silence (the server's chat handler checks it by id).
constexpr uint32_t kGmSilenceAura = 1852;

// The server's checks on a whisper from `bot` to `to` that a player's client
// would hit (WorldSession::HandleMessagechatOpcode, CHAT_MSG_WHISPER), in its
// order; empty when it may be sent.
std::string WhyCantWhisper(Player* bot, Player* to, const std::string& name) {
  const auto level_req = static_cast<uint32_t>(
      sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ));
  if (bot->GetLevel() < level_req && !to->IsGameMaster()) {
    return "you must be level " + std::to_string(level_req) + " to whisper";
  }
  WorldSession* session = to->GetSession();
  if (!to->isAcceptWhispers() && session &&
      session->HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) &&
      !to->IsInWhisperWhiteList(bot->GetGUID())) {
    // The client says the same as for nobody there.
    return "no player named '" + name + "' is currently playing";
  }
  const bool bot_is_player =
      !bot->GetSession() || !bot->GetSession()->HasPermission(
                                rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT);
  const bool to_is_player =
      !session ||
      !session->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT);
  if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT) &&
      bot_is_player && to_is_player && bot->GetTeamId() != to->GetTeamId()) {
    return "you can't whisper players of the other faction";
  }
  if (bot->HasAura(kGmSilenceAura) && !to->IsGameMaster()) {
    return "you have been silenced";
  }
  return "";
}

}  // namespace

std::string Speak(Player* bot, const pb::Command::Say& say) {
  const std::string text = CleanChatText(say.text);
  if (text.empty()) {
    return "nothing to say";
  }
  if (!bot->CanSpeak()) {
    return "you can't speak yet (muted)";
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
      if (std::string why = WhyCantWhisper(bot, to, say.to); !why.empty()) {
        return why;
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
