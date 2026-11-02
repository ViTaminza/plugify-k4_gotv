// k4_gotv — the entity-handle value rule, engine-free so the tests can check it.
//
// s2sdk entity handles are int32 views of the engine's uint32 CEntityHandle
// (index | serial << 15): 0 is the world entity, -1 (INVALID_EHANDLE_INDEX) means "no entity",
// and every other value may be a real handle - including a negative one, because once the
// engine's serial counter passes 65535 bit 31 is set. A positive-only test silently drops
// those entities.
//
// src/plugin.cpp applies this rule to PlayerSlotToEntHandle() in addition to
// s2sdk::IsValidEntHandle(), which asks the entity system whether index + serial still exist: the
// generated bindings call their slots unchecked, so the plugin refuses 0/-1 itself first.
#pragma once

#include <cstdint>

namespace k4 {

// 0 (world) and -1 (INVALID_EHANDLE_INDEX) are not player-controller handles; any other value,
// negative or not, is left to s2sdk::IsValidEntHandle().
inline bool IsValidEntityHandleValue(std::int32_t handle) {
	return handle != 0 && handle != -1;
}

} // namespace k4
