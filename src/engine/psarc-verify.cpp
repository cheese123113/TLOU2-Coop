#include "engine/psarc-verify.hpp"

// Co-op build: no embedded archive and no game-archive fingerprinting. The co-op
// mod runs the unmodified campaign data, so there is nothing to verify.
namespace engine::verify {
	volatile long g_enabled = 0;
	bool HasManifest() { return false; }
	bool CheckArchive(const char*, const char*) { return true; }
	bool Tampered() { return false; }
}
