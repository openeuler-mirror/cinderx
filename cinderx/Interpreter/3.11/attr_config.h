// Attribute-path settings are captured once after native import admission.
// Later environment changes must not alter install/remove behavior.
#pragma once
#include <stdlib.h>
#include <string.h>
static int ci_attr_config_captured_311;
static int ci_attr_mode_311; // 0: unspecified, 1: off, 2: control, 3: full
static int ci_attr_noexc_config_311;
static int ci_attr_diagnostics_config_311;
void Ci_AttrPathConfigSnapshot311(void) {
  if (ci_attr_config_captured_311) return;
  const char* mode = getenv("ATTR_PATH_MODE");
  ci_attr_mode_311 = mode == NULL ? 0 :
      strcmp(mode, "off") == 0 ? 1 :
      strcmp(mode, "cinderx_control") == 0 ? 2 :
      strcmp(mode, "cinderx") == 0 ? 3 : 0;
  const char* enabled = getenv("ATTR_PATH_NOEXC");
  const char* diagnostics = getenv("ATTR_PATH_DIAGNOSTICS");
  ci_attr_noexc_config_311 = enabled == NULL ? ci_attr_mode_311 == 3 : strcmp(enabled, "1") == 0;
  ci_attr_diagnostics_config_311 = diagnostics != NULL && strcmp(diagnostics, "1") == 0;
  ci_attr_config_captured_311 = 1;
}

int Ci_AttrPathMode311(void) {
  Ci_AttrPathConfigSnapshot311();
  return ci_attr_mode_311;
}
