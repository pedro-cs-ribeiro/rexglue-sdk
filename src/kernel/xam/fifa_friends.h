// FIFA Street: helpers for the in-game friend picker (XamShowFriendsUI).
//
// The picker runs in the host runtime and talks to the FIFA Street online
// server's private /fsr/ control channel to list who is online and to send a
// game invite. Kept in its own translation unit so the raw Winsock include does
// not clash with the SDK's socket abstraction.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex {
namespace kernel {
namespace xam {

// One invitable player as returned by the server.
struct FriendEntry {
  uint32_t id = 0;       // server-side player id (invite target)
  std::string name;      // gamertag shown in the picker
  uint64_t xuid = 0;     // online xuid
};

// An invite this player has received.
struct InviteInfo {
  uint32_t from_id = 0;   // inviter's server-side player id
  std::string from_name;  // inviter's gamertag
  uint32_t game_id = 0;   // host's game to join (0 = none open yet)
};

// GET /fsr/players: everyone online except `self_xuid`. Empty on any failure.
std::vector<FriendEntry> FsrFetchPlayers(uint64_t self_xuid);

// POST /fsr/invite: ask the server to deliver a game invite from `from_xuid`
// to the player with id `to_id`.
void FsrSendInvite(uint64_t from_xuid, uint32_t to_id);

// GET /fsr/invites: game invites waiting for `self_xuid`.
std::vector<InviteInfo> FsrFetchInvites(uint64_t self_xuid);

// POST /fsr/accept: accept the invite from `from_id` and join that game.
void FsrAccept(uint64_t self_xuid, uint32_t from_id);

// Start the background thread that watches for received invites and prompts the
// player to accept. Safe to call repeatedly; only the first call starts it.
void StartInvitePollThread();

}  // namespace xam
}  // namespace kernel
}  // namespace rex
