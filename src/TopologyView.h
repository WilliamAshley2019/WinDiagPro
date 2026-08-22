// TopologyView.h - a self-drawn (GDI) network topology diagram control.
// This is the "draw wires between boxes" view requested as an alternative to
// reading text tables: This PC -> each adapter -> (if it has a working
// default gateway) -> Internet, laid out and color-coded automatically from
// live DiagnosticsEngine data. Read-only / auto-generated for this first
// version - not an interactive drag-and-drop editor (see README for that
// distinction and why).
#pragma once
#include "Common.h"
#include "DiagnosticsEngine.h"
#include <vector>

// Registers the topology view window class (idempotent - safe to call once
// at startup) and creates an instance as a child of 'parent'.
HWND CreateTopologyView(HWND parent, HINSTANCE hInst, int controlId);

// Replaces the adapter data the view renders and repaints. Cheap enough to
// call on a timer - no network I/O happens here, just drawing.
void TopologyView_SetData(HWND view, const std::vector<NetworkAdapterInfo>& adapters);
