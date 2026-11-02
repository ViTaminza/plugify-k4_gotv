// k4_gotv — the map readiness gate of PITFALLS B1/B2, engine-free so the tests can drive every
// transition.
//
// s2sdk dereferences its client list and entity system only while a map is up: before the first
// map, and in the window between OnMapEnd and the next OnMapStart, every client/entity/schema
// query reads a torn-down list and crashes the server. Every such call of this plugin therefore
// asks Ready() first; file, config, worker, HTTP and database work does not.
//
// Wiring (src/plugin.cpp):
//   * OnPluginStart  -> Reset(): a fresh start (including `plg reload`) begins with the gate shut.
//   * OnMapStart     -> OnMapStart(): the map and its client list exist, the gate opens.
//   * OnMapEnd       -> OnMapEnd(): only drops state; no client/entity call may follow it (B2).
//   * OnPluginUpdate -> OnPluginUpdate(GetCurrentMap()): a plugin loaded (or reloaded) into a
//                       running map never saw OnMapStart, so its first update may open the gate -
//                       but only with all four map-listener bindings registered (otherwise
//                       nothing could ever close it again), only when no OnMapEnd was seen since
//                       OnPluginStart, and only when s2sdk answers a real map name ("" and
//                       "<empty>" mean "no map yet"). This is the mid-map latch; it is attempted
//                       exactly once, so a later update in a changelevel window cannot reopen it.
//   * OnPluginEnd    -> Close(): the library is going away, a stale callback must not reach a
//                       client.
#pragma once

#include <string_view>

namespace k4 {

// s2sdk's map queries answer "" or "<empty>" when no map is loaded (the latter is what
// GetCurrentMap() returns through the engine's default map name before the first level).
inline bool IsUsableMapName(std::string_view name) {
	return !name.empty() && name != "<empty>";
}

class MapGate {
public:
	// Whether s2sdk client/entity/schema calls are safe right now.
	[[nodiscard]] bool Ready() const { return m_ready; }

	// Whether the OnMapStart/OnMapEnd listeners are registered (OnPluginStart succeeded).
	[[nodiscard]] bool ListenersRegistered() const { return m_listeners; }

	// Whether the first OnPluginUpdate already ran its mid-map latch attempt.
	[[nodiscard]] bool UpdateAttempted() const { return m_updateAttempted; }

	// OnPluginStart: a fresh start begins with the gate shut.
	void Reset() { *this = MapGate(); }

	void SetListenersRegistered(bool registered) { m_listeners = registered; }

	// OnMapStart listener: the map and its client list are in place.
	void OnMapStart() {
		m_ready = true;
		m_mapStartSeen = true;
		m_mapEndSeen = false;
		m_updateAttempted = false;
	}

	// OnMapEnd listener: from here until the next OnMapStart every client query can crash, so
	// this only clears state - it must not touch a client or an entity (PITFALLS B2).
	void OnMapEnd() {
		m_ready = false;
		m_mapStartSeen = false;
		m_mapEndSeen = true;
	}

	// OnPluginUpdate: the one-shot mid-map latch described above. Returns whether the gate is
	// open afterwards. A gate that is already open, a server between maps and a plugin without
	// listeners never open it here.
	bool OnPluginUpdate(std::string_view currentMap) {
		if (m_updateAttempted) {
			return m_ready;
		}
		m_updateAttempted = true;
		if (m_ready || !m_listeners || m_mapEndSeen || m_mapStartSeen) {
			return m_ready;
		}
		if (!IsUsableMapName(currentMap)) {
			return false;
		}
		m_ready = true;
		return true;
	}

	// OnPluginEnd: nothing of this plugin may answer a callback after the library is unloaded.
	// The end latch also makes any update the host still pumps afterwards inert, even if a
	// listener flag was not cleared first.
	void Close() {
		m_ready = false;
		m_mapEndSeen = true;
	}

private:
	bool m_ready = false;
	bool m_mapStartSeen = false;   // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
	bool m_mapEndSeen = false;     // an OnMapEnd arrived since OnPluginStart
	bool m_listeners = false;      // the four s2sdk map listener bindings are registered
	bool m_updateAttempted = false; // the single mid-map latch attempt has been made
};

} // namespace k4
