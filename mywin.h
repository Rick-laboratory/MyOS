#pragma once
/*
 * Legacy compatibility wrapper.
 *
 * New code should include <windows.h> for the MSDN contract and add
 * "myos_private.h" / "myos_diag.h" only when it explicitly needs myOS
 * runtime or inspection hooks.
 */
#include <windows.h>
#include "myos_private.h"
#include "myos_diag.h"
