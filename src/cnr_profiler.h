#pragma once

// CNR_PROFILE=<start s>,<duration s>: ~1 kHz statistical profile of all threads, written to `path`
// (src/cnr_profiler.cpp). Needs the PDB next to the exe for symbols.
void cnrStartProfiler(double startSeconds, double durationSeconds, const char *path);
