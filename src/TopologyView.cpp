#include "TopologyView.h"
#include <windowsx.h>
#include <algorithm>
#include <sstream>

namespace {

constexpr wchar_t kTopoClassName[] = L"WinDiagProTopologyView";
constexpr int kDetailsPanelHeight = 120;
constexpr int kMargin = 16;
constexpr int kNodeH = 56;

struct TopoNode {
    RECT rect{};
    std::wstring title;
    std::wstring subtitle;
    std::wstring details;
    COLORREF borderColor = RGB(120, 120, 120);
    COLORREF fillColor = RGB(245, 245, 245);
};

struct TopoLine {
    POINT from{}, to{};
    COLORREF color = RGB(120, 120, 120);
    bool dashed = false;
    std::wstring label;
};

struct TopoState {
    std::vector<NetworkAdapterInfo> adapters;
    std::vector<TopoNode> nodes;
    std::vector<TopoLine> lines;
    int selected = -1;
    std::wstring selectedKey; // node title of the selected node - stable across
                                // rebuilds, so the periodic auto-refresh doesn't
                                // wipe out what the user clicked
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    int lastWidth = 0, lastHeight = 0;
};

std::wstring RoleColorLabel(AdapterRole role) {
    switch (role) {
        case AdapterRole::Routed: return L"Routed (internet path)";
        case AdapterRole::GatewayScopeMismatch: return L"INVALID: APIPA + gateway";
        case AdapterRole::DirectLink: return L"Direct link (APIPA only)";
        case AdapterRole::Isolated: return L"Isolated (no gateway)";
        case AdapterRole::Disconnected: return L"Disconnected";
        case AdapterRole::Virtual: return L"Virtual switch";
    }
    return L"?";
}

void RoleColors(AdapterRole role, COLORREF& border, COLORREF& fill) {
    switch (role) {
        case AdapterRole::Routed:               border = RGB(39, 174, 96);  fill = RGB(224, 247, 234); break;
        case AdapterRole::GatewayScopeMismatch:  border = RGB(192, 57, 43); fill = RGB(253, 226, 221); break;
        case AdapterRole::DirectLink:            border = RGB(41, 128, 185);fill = RGB(217, 237, 247); break;
        case AdapterRole::Isolated:              border = RGB(211, 84, 0); fill = RGB(250, 229, 211); break;
        case AdapterRole::Disconnected:          border = RGB(127, 140, 141); fill = RGB(236, 240, 241); break;
        case AdapterRole::Virtual:               border = RGB(142, 68, 173); fill = RGB(232, 218, 239); break;
        default:                                 border = RGB(120,120,120); fill = RGB(245,245,245); break;
    }
}

std::wstring BuildAdapterDetails(const NetworkAdapterInfo& a) {
    std::wstring d;
    d += L"Adapter: " + a.name + L" (" + a.description + L")\r\n";
    d += L"Role: " + RoleColorLabel(a.role) + L"\r\n";
    d += L"IPv4: " + (a.ipAddress.empty() ? L"(none)" : a.ipAddress) +
         (a.dhcpEnabled ? L" [DHCP]" : L" [static]") + L"\r\n";
    d += L"Gateway: " + (a.gateway.empty() ? L"(none)" : a.gateway);
    if (!a.gatewayNudState.empty()) d += L"  -  state: " + a.gatewayNudState;
    d += L"\r\n";
    d += L"Route metric: " + std::to_wstring(a.metric) + L" (lower = more preferred)\r\n";
    if (!a.dnsServers.empty()) {
        d += L"DNS servers: ";
        for (size_t i = 0; i < a.dnsServers.size(); ++i) {
            d += a.dnsServers[i];
            if (i + 1 < a.dnsServers.size()) d += L", ";
        }
        d += L"\r\n";
    }
    d += L"MAC: " + (a.macAddress.empty() ? L"(n/a)" : a.macAddress);
    return d;
}

void Layout(TopoState& st, int width, int height) {
    st.nodes.clear();
    st.lines.clear();

    int diagramHeight = height - kDetailsPanelHeight - kMargin;
    if (diagramHeight < 150) diagramHeight = 150;

    // --- Internet node (top, centered) ---
    TopoNode internetNode;
    internetNode.title = L"INTERNET";
    internetNode.subtitle = L"External network";
    internetNode.details = L"The public internet, reached through whichever adapter below has a "
                             L"working default gateway. Lines to this box are only drawn for "
                             L"adapters that currently have one.";
    internetNode.borderColor = RGB(52, 73, 94);
    internetNode.fillColor = RGB(225, 231, 236);
    int netW = 170;
    internetNode.rect = { width / 2 - netW / 2, kMargin, width / 2 + netW / 2, kMargin + kNodeH };
    st.nodes.push_back(internetNode);
    int internetIdx = (int)st.nodes.size() - 1;

    // --- This PC node (middle, centered) ---
    TopoNode pcNode;
    pcNode.title = L"THIS PC";
    int routedCount = 0;
    for (auto& a : st.adapters) if (a.role == AdapterRole::Routed || a.role == AdapterRole::GatewayScopeMismatch) ++routedCount;
    pcNode.subtitle = std::to_wstring(st.adapters.size()) + L" adapter(s), " + std::to_wstring(routedCount) + L" with a gateway";
    pcNode.details = L"This computer. Each box below is one network adapter Windows currently "
                      L"sees (physical or virtual).";
    pcNode.borderColor = RGB(44, 62, 80);
    pcNode.fillColor = RGB(214, 224, 232);
    int pcY = kMargin + kNodeH + (diagramHeight - kNodeH * 3) / 3;
    if (pcY < internetNode.rect.bottom + 40) pcY = internetNode.rect.bottom + 40;
    pcNode.rect = { width / 2 - netW / 2, pcY, width / 2 + netW / 2, pcY + kNodeH };
    st.nodes.push_back(pcNode);
    int pcIdx = (int)st.nodes.size() - 1;

    // --- Adapter nodes (bottom row) ---
    int n = (int)st.adapters.size();
    if (n == 0) return;
    int boxW = std::min(190, std::max(120, (width - kMargin * (n + 1)) / n));
    int totalW = boxW * n + kMargin * (n - 1);
    int startX = (width - totalW) / 2;
    int adapterY = std::max((int)pcNode.rect.bottom + 50, diagramHeight - kNodeH - kMargin);

    for (int i = 0; i < n; ++i) {
        const auto& a = st.adapters[i];
        TopoNode node;
        node.title = a.name;
        node.subtitle = a.ipAddress.empty() ? RoleColorLabel(a.role) : a.ipAddress;
        node.details = BuildAdapterDetails(a);
        RoleColors(a.role, node.borderColor, node.fillColor);
        int x = startX + i * (boxW + kMargin);
        node.rect = { x, adapterY, x + boxW, adapterY + kNodeH };
        st.nodes.push_back(node);
        int adapterNodeIdx = (int)st.nodes.size() - 1;

        POINT adapterTop{ (x + x + boxW) / 2, adapterY };
        POINT pcBottom{ (pcNode.rect.left + pcNode.rect.right) / 2, pcNode.rect.bottom };

        // This PC -> adapter: always drawn (physical presence in the machine).
        TopoLine pcLine;
        pcLine.from = pcBottom;
        pcLine.to = adapterTop;
        pcLine.color = RGB(160, 160, 160);
        pcLine.dashed = (a.role == AdapterRole::Virtual || a.role == AdapterRole::Disconnected);
        st.lines.push_back(pcLine);

        // Adapter -> Internet: only when there's an actual (even if invalid) gateway.
        if (!a.gateway.empty() && (a.role == AdapterRole::Routed || a.role == AdapterRole::GatewayScopeMismatch)) {
            TopoLine netLine;
            netLine.from = adapterTop;
            netLine.to = { (internetNode.rect.left + internetNode.rect.right) / 2, internetNode.rect.bottom };
            netLine.color = node.borderColor;
            netLine.dashed = (a.role == AdapterRole::GatewayScopeMismatch);
            netLine.label = a.gateway;
            st.lines.push_back(netLine);
        }

        (void)adapterNodeIdx;
    }

    (void)internetIdx;
    (void)pcIdx;

    // Re-resolve the selection by identity (title), not by index - the node
    // list was just rebuilt from scratch, so any previously-selected index
    // would now point at a different (or nonexistent) node. This is what
    // makes a click's details panel survive the periodic auto-refresh
    // instead of reverting a few seconds later.
    st.selected = -1;
    if (!st.selectedKey.empty()) {
        for (size_t i = 0; i < st.nodes.size(); ++i) {
            if (st.nodes[i].title == st.selectedKey) {
                st.selected = (int)i;
                break;
            }
        }
    }
}

void PaintNode(HDC hdc, const TopoNode& node, bool selected, HFONT fontTitle, HFONT fontBody) {
    HBRUSH fillBrush = CreateSolidBrush(node.fillColor);
    HPEN borderPen = CreatePen(PS_SOLID, selected ? 3 : 2, node.borderColor);
    HGDIOBJ oldBrush = SelectObject(hdc, fillBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    RoundRect(hdc, node.rect.left, node.rect.top, node.rect.right, node.rect.bottom, 10, 10);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);

    SetBkMode(hdc, TRANSPARENT);
    RECT titleRect = node.rect;
    titleRect.top += 4;
    titleRect.bottom = titleRect.top + 18;
    titleRect.left += 4; titleRect.right -= 4;
    HGDIOBJ oldFont = SelectObject(hdc, fontTitle);
    SetTextColor(hdc, RGB(20, 20, 20));
    DrawTextW(hdc, node.title.c_str(), -1, &titleRect, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT subRect = node.rect;
    subRect.top = titleRect.bottom;
    subRect.bottom -= 4;
    subRect.left += 4; subRect.right -= 4;
    SelectObject(hdc, fontBody);
    SetTextColor(hdc, RGB(60, 60, 60));
    DrawTextW(hdc, node.subtitle.c_str(), -1, &subRect, DT_CENTER | DT_WORDBREAK);
    SelectObject(hdc, oldFont);
}

void PaintLine(HDC hdc, const TopoLine& line, HFONT fontBody) {
    HPEN pen = CreatePen(line.dashed ? PS_DASH : PS_SOLID, 2, line.color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, line.from.x, line.from.y, nullptr);
    LineTo(hdc, line.to.x, line.to.y);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    if (!line.label.empty()) {
        int mx = (line.from.x + line.to.x) / 2;
        int my = (line.from.y + line.to.y) / 2;
        RECT r{ mx - 70, my - 9, mx + 70, my + 9 };
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(255, 255, 255));
        SetTextColor(hdc, line.color);
        HGDIOBJ oldFont = SelectObject(hdc, fontBody);
        DrawTextW(hdc, line.label.c_str(), -1, &r, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        SetBkMode(hdc, TRANSPARENT);
    }
}

void PaintAll(HWND hwnd, TopoState& st, HDC targetDc, int width, int height) {
    HDC memDc = CreateCompatibleDC(targetDc);
    HBITMAP memBmp = CreateCompatibleBitmap(targetDc, width, height);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    RECT full{ 0, 0, width, height };
    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(memDc, &full, bg);
    DeleteObject(bg);

    for (auto& line : st.lines) PaintLine(memDc, line, st.fontBody);
    for (size_t i = 0; i < st.nodes.size(); ++i) PaintNode(memDc, st.nodes[i], (int)i == st.selected, st.fontTitle, st.fontBody);

    // Details panel
    int panelY = height - kDetailsPanelHeight;
    HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    HGDIOBJ oldPen = SelectObject(memDc, sepPen);
    MoveToEx(memDc, 0, panelY, nullptr);
    LineTo(memDc, width, panelY);
    SelectObject(memDc, oldPen);
    DeleteObject(sepPen);

    RECT detailsRect{ kMargin, panelY + 8, width - kMargin, height - 8 };
    HGDIOBJ oldFont = SelectObject(memDc, st.fontBody);
    SetTextColor(memDc, RGB(30, 30, 30));
    std::wstring text = (st.selected >= 0 && st.selected < (int)st.nodes.size())
        ? st.nodes[st.selected].details
        : L"Click any box (This PC, Internet, or an adapter) for its full details. "
          L"Green = healthy routed adapter. Red = invalid APIPA+gateway combination "
          L"(fixable with \"Release & renew DHCP on this adapter\" from the Network "
          L"tab's right-click menu). Blue = direct link (no DHCP server on this "
          L"cable, e.g. point-to-point to another PC). Orange = isolated (no "
          L"gateway). Gray = disconnected. Purple = virtual switch (Hyper-V/WSL).";
    DrawTextW(memDc, text.c_str(), -1, &detailsRect, DT_WORDBREAK | DT_TOP | DT_LEFT);
    SelectObject(memDc, oldFont);

    BitBlt(targetDc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
    (void)hwnd;
}

int HitTestNode(TopoState& st, POINT pt) {
    for (int i = (int)st.nodes.size() - 1; i >= 0; --i) {
        if (PtInRect(&st.nodes[i].rect, pt)) return i;
    }
    return -1;
}

LRESULT CALLBACK TopoWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto st = reinterpret_cast<TopoState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE:
            return DefWindowProcW(hwnd, msg, wparam, lparam);

        case WM_CREATE: {
            auto newSt = new TopoState();
            newSt->fontTitle = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            newSt->fontBody = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newSt));
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // we paint the whole client area ourselves in WM_PAINT

        case WM_SIZE:
            if (st) {
                st->lastWidth = LOWORD(lparam);
                st->lastHeight = HIWORD(lparam);
                Layout(*st, st->lastWidth, st->lastHeight);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            if (st) {
                POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                int hit = HitTestNode(*st, pt);
                st->selected = hit;
                st->selectedKey = (hit >= 0) ? st->nodes[hit].title : L"";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st) {
                RECT rc; GetClientRect(hwnd, &rc);
                PaintAll(hwnd, *st, hdc, rc.right - rc.left, rc.bottom - rc.top);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            if (st) {
                if (st->fontTitle) DeleteObject(st->fontTitle);
                if (st->fontBody) DeleteObject(st->fontBody);
                delete st;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void EnsureClassRegistered(HINSTANCE hInst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = TopoWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // WM_ERASEBKGND handles this
    wc.lpszClassName = kTopoClassName;
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

HWND CreateTopologyView(HWND parent, HINSTANCE hInst, int controlId) {
    EnsureClassRegistered(hInst);
    return CreateWindowExW(WS_EX_CLIENTEDGE, kTopoClassName, L"", WS_CHILD | WS_TABSTOP,
                            0, 0, 0, 0, parent, (HMENU)(INT_PTR)controlId, hInst, nullptr);
}

void TopologyView_SetData(HWND view, const std::vector<NetworkAdapterInfo>& adapters) {
    auto st = reinterpret_cast<TopoState*>(GetWindowLongPtrW(view, GWLP_USERDATA));
    if (!st) return;
    st->adapters = adapters;
    // Note: selection is intentionally NOT cleared here - Layout() below
    // re-resolves st->selected from st->selectedKey against the freshly
    // rebuilt node list, so a click survives the periodic auto-refresh
    // instead of reverting to the default hint text a few seconds later.
    RECT rc; GetClientRect(view, &rc);
    Layout(*st, rc.right - rc.left, rc.bottom - rc.top);
    InvalidateRect(view, nullptr, FALSE);
}
