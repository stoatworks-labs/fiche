/**
	The bundle's own translation unit.

	There is no entry point to write. `plugMain` lives in the SDK, the host
	resolves it by name after dlopen, and everything past that follows from the
	`CFFGLPluginInfo` that Fiche.cpp registers at static-initialisation time.
	This file exists so the bundle target has a source of its own, and it
	carries the one thing worth being able to read back out of a shipped
	binary: which build it is.

		strings Fiche.bundle/Contents/MacOS/Fiche | grep fiche
*/

extern "C" const char* FicheBuildStamp()
{
	return "fiche " FICHE_VERSION " built " __DATE__ " " __TIME__;
}
