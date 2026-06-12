// Force-included into every SeaShieldCore TU (see Build.cs ForceIncludeFiles).
// The repo's std-only sources carry no export annotations, and UE's modular
// Mac builds compile with hidden symbol visibility (-fvisibility-ms-compat),
// so without this the game module cannot link against this module's dylib.
#pragma GCC visibility push(default)
