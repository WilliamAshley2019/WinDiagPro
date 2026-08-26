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

// Posted (via SendMessageW) to the view's parent whenever the user clicks a
// different node, so the host can update anything it shows alongside the
// diagram (e.g. a "Reverify Gateway" button) right away instead of waiting
// for the next periodic refresh.
constexpr UINT WM_TOPOLOGY_SELECTION_CHANGED = WM_APP + 300;

// Returns the friendly name of the currently-selected adapter node, or an
// empty string if nothing is selected or the selection is the Internet/This
// PC node rather than a real adapter. Call this after receiving
// WM_TOPOLOGY_SELECTION_CHANGED (or any time) to find out what's selected.
std::wstring TopologyView_GetSelectedAdapterName(HWND view);
