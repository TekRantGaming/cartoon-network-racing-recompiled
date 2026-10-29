#pragma once
// VU1 and VU0 micro mode on PCSX2's microVU recompiler (ext/kzvu).
#include <string>

class PS2Runtime;

// Before PS2Runtime::initialize (the runtime adopts kzvu's VU memories). CNR_VU=builtin / CNR_VU0=interp keep the
// runtime's own interpreters.
bool cnrVuInstall(std::string *error);
void cnrVuBindRuntime(PS2Runtime &runtime);
