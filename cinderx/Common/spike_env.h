// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

// SPIKE(env): process-level snapshot of the SPIKE_* switches.
//
// Every switch used to be read with getenv() at its first use, and most
// reads sit behind function-local statics that initialize whenever the
// enclosing function first runs.  A program that clears its environment
// (os.environ.clear() in test_os's EnvironTests, for example) therefore
// flips every switch whose first read comes later, leaving the process in
// a mixed configuration: the direct-entry re-entry lookup initialized as
// "off" after the clear handed bound argument arrays to the interpreter's
// entry ("takes 4 positional arguments but 10 were given").  The snapshot
// is taken once, at module import, and every read consults it.

#ifdef __cplusplus
extern "C" {
#endif

// Capture the SPIKE_* variables of the current environment.  Idempotent:
// only the first call records anything.
void Ci_SpikeEnvSnapshot(void);

// The value of a SPIKE_* variable as of the snapshot (taken now if it has
// not been taken yet), or NULL when it was unset.  The returned pointer
// stays valid for the life of the process.
const char* Ci_SpikeGetenv(const char* name);

#ifdef __cplusplus
}
#endif
