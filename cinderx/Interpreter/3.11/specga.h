// CPython 3.11.6 interpreter-only specialization.
#pragma once
#include "Python.h"
extern int Ci_SpecGaEnabled_311;
extern int Ci_SplitHintEnabled_311;
int Ci_InitGetattrHooks_311(void);
// GIL/main interpreter required. Process-lifetime semantic-equivalent hooks only.
int Ci_RegisterGetattrHook_311(getattrofunc hook);

int Ci_AttrPathMode311(void);
