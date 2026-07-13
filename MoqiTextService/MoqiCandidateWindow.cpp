//
//    Copyright (C) 2026
//

#include "MoqiCandidateWindow.h"
#include "MoqiTextService.h"

#include <LibIME2/src/DebugLogConfig.h>
#include <LibIME2/src/DebugLogFile.h>
#include <LibIME2/src/DrawUtils.h>
#include <LibIME2/src/EditSession.h>
#include <LibIME2/src/TextService.h>
#include <windowsx.h>

#include <algorithm>
#include <cassert>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace {

constexpr COLORREF kWindowBackground = RGB(255, 255, 255);       // panel_background
constexpr COLORREF kLayeredTransparentColor = RGB(255, 0, 255);
constexpr COLORREF kInputBufferBackground = RGB(246, 234, 216);  // input_buffer_background
constexpr COLORREF kInputBufferText = RGB(70, 58, 42);           // input_buffer_text
constexpr COLORREF kWindowBorder = RGB(222, 217, 207);           // panel_border
constexpr COLORREF kDividerColor = RGB(222, 217, 207);
constexpr COLORREF kItemText = RGB(36, 34, 30);                  // text_primary
constexpr COLORREF kSecondaryText = RGB(105, 98, 88);            // text_secondary
constexpr COLORREF kDisabledText = RGB(168, 160, 148);           // disabled_text
constexpr COLORREF kLinkText = RGB(151, 102, 31);                // link_text
constexpr COLORREF kSelectedBackground = RGB(254, 220, 156);     // selection_background
constexpr COLORREF kSelectedText = RGB(36, 34, 30);
constexpr int kDefaultCandidateSpacing = 20;
constexpr int kTypeDuckCandidatePanelRenderer = 1;
constexpr int kPageNavNone = -1;
constexpr int kPageNavPrevious = 0;
constexpr int kPageNavNext = 1;
constexpr int kWindowDpiBaseline = 96;
constexpr const wchar_t* kDefaultCandidateFontName = L"Microsoft JhengHei";
constexpr const wchar_t* kDefaultCommentFontName = L"Segoe UI";
constexpr int kBorderWidth = 1;
constexpr int kInitialCompactPanelPaddingX = 7;
constexpr int kInitialCompactPanelPaddingY = 3;
constexpr int kInitialBorderRadius = 4;
constexpr int kInitialCandidateMinWidth = 200;
constexpr int kPanelPaddingX = 6;
constexpr int kPanelPaddingY = 3;
constexpr int kImmersivePanelPaddingX = 12;
constexpr int kImmersivePanelPaddingY = 8;
constexpr int kCandidateMinWidth = 64;
constexpr int kCandidateBorderRadius = 8;
constexpr int kCandidateLabelGap = 6;
constexpr int kCandidatePreeditGap = 8;
constexpr int kCandidateRowPaddingY = 5;
constexpr int kCandidateRowInnerGap = 6;
constexpr int kCandidateRowCornerRadius = 5;
constexpr int kCandidateBodyLineMinHeight = 25;
constexpr int kCandidateCellGap = 4;
constexpr int kCandidateCellPadX = 6;
constexpr int kCandidateCellMaxTextWidth = 168;
constexpr int kCandidateCellMinTextWidth = 24;
constexpr int kPageNavWidth = 40;
constexpr int kPageNavPreeditlessHeight = 30;
constexpr int kPageNavGlyphPointSize = 28;
constexpr int kPageNavGlyphYOffset = 7;
constexpr int kPageNavHoverRadius = 6;
constexpr int kPreeditExtraHeight = 8;
constexpr int kPreeditTextWidthPadding = 28;
constexpr int kPreeditPlainMargin = 4;
constexpr int kPreeditActivePaddingX = 6;
constexpr int kPreeditActiveCornerRadius = 5;
constexpr int kPreeditCursorWidth = 2;
constexpr int kPreeditCursorVerticalInset = 1;
constexpr const wchar_t* kInputBufferFontName = L"Microsoft JhengHei UI";

Moqi::TextService* productTextService(Ime::TextService* service) {
    return static_cast<Moqi::TextService*>(service);
}

struct DpiPair {
    int x = kWindowDpiBaseline;
    int y = kWindowDpiBaseline;
};

using SetThreadDpiAwarenessContextProc = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
using GetDpiForMonitorProc = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
using GetScaleFactorForMonitorProc = HRESULT(WINAPI*)(HMONITOR, int*);

SetThreadDpiAwarenessContextProc setThreadDpiAwarenessContextProc() {
    static auto proc = reinterpret_cast<SetThreadDpiAwarenessContextProc>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    return proc;
}

class ThreadDpiAwarenessScope {
public:
    ThreadDpiAwarenessScope() {
        auto proc = setThreadDpiAwarenessContextProc();
        if (proc) {
            oldContext_ = proc(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));
        }
    }

    ~ThreadDpiAwarenessScope() {
        auto proc = setThreadDpiAwarenessContextProc();
        if (proc && oldContext_) {
            proc(oldContext_);
        }
    }

private:
    DPI_AWARENESS_CONTEXT oldContext_ = nullptr;
};

DpiPair dpiFromScreenDc() {
    DpiPair dpi;
    HDC hdc = ::GetDC(nullptr);
    if (hdc) {
        dpi.x = (std::max)(kWindowDpiBaseline, ::GetDeviceCaps(hdc, LOGPIXELSX));
        dpi.y = (std::max)(kWindowDpiBaseline, ::GetDeviceCaps(hdc, LOGPIXELSY));
        ::ReleaseDC(nullptr, hdc);
    }
    return dpi;
}

DpiPair dpiFromMonitor(HMONITOR monitor) {
    DpiPair dpi = dpiFromScreenDc();
    if (!monitor) {
        return dpi;
    }

    HMODULE shcore = ::LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        auto getDpiForMonitor = reinterpret_cast<GetDpiForMonitorProc>(
            ::GetProcAddress(shcore, "GetDpiForMonitor"));
        if (getDpiForMonitor) {
            UINT x = 0;
            UINT y = 0;
            if (SUCCEEDED(getDpiForMonitor(monitor, 0, &x, &y)) && x > 0 && y > 0) {
                dpi.x = (std::max)(dpi.x, static_cast<int>(x));
                dpi.y = (std::max)(dpi.y, static_cast<int>(y));
            }
        }

        auto getScaleFactorForMonitor = reinterpret_cast<GetScaleFactorForMonitorProc>(
            ::GetProcAddress(shcore, "GetScaleFactorForMonitor"));
        if (getScaleFactorForMonitor) {
            int scalePercent = 100;
            if (SUCCEEDED(getScaleFactorForMonitor(monitor, &scalePercent)) && scalePercent > 0) {
                const int scaledDpi = ::MulDiv(kWindowDpiBaseline, scalePercent, 100);
                dpi.x = (std::max)(dpi.x, scaledDpi);
                dpi.y = (std::max)(dpi.y, scaledDpi);
            }
        }
        ::FreeLibrary(shcore);
    }
    return dpi;
}

DpiPair dpiForOwnerWindow(HWND owner) {
    DpiPair dpi = dpiFromScreenDc();
    HWND target = owner ? owner : ::GetForegroundWindow();
    if (target) {
        auto getDpiForWindow = reinterpret_cast<GetDpiForWindowProc>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (getDpiForWindow) {
            const UINT windowDpi = getDpiForWindow(target);
            if (windowDpi > 0) {
                dpi.x = (std::max)(dpi.x, static_cast<int>(windowDpi));
                dpi.y = (std::max)(dpi.y, static_cast<int>(windowDpi));
            }
        }
        const DpiPair monitorDpi = dpiFromMonitor(::MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST));
        dpi.x = (std::max)(dpi.x, monitorDpi.x);
        dpi.y = (std::max)(dpi.y, monitorDpi.y);
    }
    return dpi;
}

int fontPointHeightForDpi(int dpiY, int point) {
    return -::MulDiv(point, (std::max)(kWindowDpiBaseline, dpiY), 72);
}

std::wstring trimFontToken(std::wstring value) {
    const size_t stylePos = value.find(L':');
    if (stylePos != std::wstring::npos) {
        value = value.substr(0, stylePos);
    }
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (first >= last) {
        return L"";
    }
    return std::wstring(first, last);
}

BOOL CALLBACK markFontFamilyFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) {
    *reinterpret_cast<bool*>(lParam) = true;
    return FALSE;
}

bool isFontFamilyInstalled(const std::wstring& faceName) {
    if (faceName.empty()) {
        return false;
    }
    HDC hdc = ::GetDC(nullptr);
    if (!hdc) {
        return false;
    }
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, _countof(lf.lfFaceName), faceName.c_str(), _TRUNCATE);
    bool found = false;
    ::EnumFontFamiliesExW(hdc, &lf, markFontFamilyFound, reinterpret_cast<LPARAM>(&found), 0);
    ::ReleaseDC(nullptr, hdc);
    return found;
}

std::wstring resolveFontFace(const std::wstring& requested, const wchar_t* fallback) {
    size_t start = 0;
    while (start <= requested.size()) {
        const size_t comma = requested.find(L',', start);
        const std::wstring candidate = trimFontToken(
            requested.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
        if (isFontFamilyInstalled(candidate)) {
            return candidate;
        }
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }
    return fallback;
}

HFONT createPointFontForDpi(int dpiY,
                            const wchar_t* faceName,
                            int pointSize,
                            int weight = FW_NORMAL,
                            bool italic = false) {
    return ::CreateFontW(
        fontPointHeightForDpi(dpiY, pointSize),
        0,
        0,
        0,
        weight,
        italic ? TRUE : FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        faceName);
}

std::wstring currentProcessPath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = ::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    if (len == 0) {
        return L"";
    }
    while (len >= buffer.size() - 1) {
        buffer.resize(buffer.size() * 2);
        len = ::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            return L"";
        }
    }
    buffer.resize(len);
    return buffer;
}

std::wstring processBaseName(const std::wstring& imagePath) {
    const size_t pos = imagePath.find_last_of(L"\\/");
    return pos == std::wstring::npos ? imagePath : imagePath.substr(pos + 1);
}

std::wstring timestampNow() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buffer[64] = {0};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

std::wstring formatCandidateWindowLogLine(const std::wstring& message) {
    const std::wstring exeName = processBaseName(currentProcessPath());
    std::wostringstream line;
    line << L"[" << timestampNow() << L"]"
         << L"[pid=" << ::GetCurrentProcessId() << L"]"
         << L"[tid=" << ::GetCurrentThreadId() << L"]"
         << L"[exe=" << (exeName.empty() ? L"<unknown>" : exeName) << L"] "
         << message;
    return line.str();
}

HFONT createDerivedFont(HFONT baseFont, const wchar_t* faceName) {
    LOGFONTW lf = {};
    if (!baseFont || ::GetObjectW(baseFont, sizeof(lf), &lf) == 0) {
        HFONT defaultFont = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        if (!defaultFont || ::GetObjectW(defaultFont, sizeof(lf), &lf) == 0) {
            return nullptr;
        }
    }
    wcscpy_s(lf.lfFaceName, faceName);
    return ::CreateFontIndirectW(&lf);
}

void appendCandidateWindowLog(const std::wstring& message) {
    if (!Ime::isTraceLoggingEnabled()) {
        return;
    }

    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (!localAppData || !*localAppData) {
        return;
    }

    std::wstring logDir = std::wstring(localAppData) + L"\\TypeDuckIME\\Log";
    std::wstring logPath = Ime::DebugLogFile::prepareDailyLogFilePath(
        logDir, L"candidate-window.log");
    if (logPath.empty()) {
        return;
    }

    std::wofstream stream(logPath, std::ios::app);
    if (!stream.is_open()) {
        return;
    }
    stream << formatCandidateWindowLogLine(message) << L"\n";
}

std::wstring hwndToString(HWND hwnd) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << reinterpret_cast<UINT_PTR>(hwnd) << std::dec;
    return stream.str();
}

void logCandidateWindowState(const std::wstring& tag, HWND hwnd) {
    if (!hwnd) {
        appendCandidateWindowLog(tag + L" hwnd=<null>");
        return;
    }

    RECT rect{};
    ::GetWindowRect(hwnd, &rect);
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtr(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    const HWND owner = reinterpret_cast<HWND>(::GetWindowLongPtr(hwnd, GWLP_HWNDPARENT));
    const HWND gwOwner = ::GetWindow(hwnd, GW_OWNER);
    const HWND parent = ::GetParent(hwnd);
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    const HWND rootOwner = ::GetAncestor(hwnd, GA_ROOTOWNER);

    std::wostringstream log;
    log << tag
        << L" hwnd=" << hwndToString(hwnd)
        << L" visible=" << (::IsWindowVisible(hwnd) ? L"true" : L"false")
        << L" owner=" << hwndToString(owner)
        << L" gw_owner=" << hwndToString(gwOwner)
        << L" parent=" << hwndToString(parent)
        << L" root=" << hwndToString(root)
        << L" root_owner=" << hwndToString(rootOwner)
        << L" style=0x" << std::hex << style
        << L" exstyle=0x" << exStyle << std::dec
        << L" rect=(" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom << L")";
    appendCandidateWindowLog(log.str());
}

HWND normalizeCandidateOwnerWindow(HWND hwnd, bool immersive, const wchar_t* reason) {
    if (hwnd == nullptr) {
        return nullptr;
    }

    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    const HWND rootOwner = ::GetAncestor(hwnd, GA_ROOTOWNER);
    const HWND normalized = immersive ? hwnd : (root != nullptr ? root : hwnd);

    std::wostringstream log;
    log << L"[normalizeCandidateOwnerWindow] reason=" << reason
        << L" immersive=" << (immersive ? L"true" : L"false")
        << L" raw=" << hwndToString(hwnd)
        << L" root=" << hwndToString(root)
        << L" root_owner=" << hwndToString(rootOwner)
        << L" normalized=" << hwndToString(normalized);
    appendCandidateWindowLog(log.str());
    return normalized;
}

void enforceCandidateWindowTopmost(HWND hwnd, bool showWindow, const wchar_t* reason) {
    if (!hwnd) {
        return;
    }

    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (showWindow) {
        flags |= SWP_SHOWWINDOW;
    }

    ::SetLastError(0);
    const BOOL ok = ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    const DWORD error = ok ? 0 : ::GetLastError();

    std::wostringstream log;
    log << L"[enforceCandidateWindowTopmost] reason=" << reason
        << L" hwnd=" << hwndToString(hwnd)
        << L" show=" << (showWindow ? L"true" : L"false")
        << L" ok=" << (ok ? L"true" : L"false");
    if (!ok) {
        log << L" last_error=" << error;
    }
    appendCandidateWindowLog(log.str());
    logCandidateWindowState(L"[CandidateWindow::state]", hwnd);
}

HWND resolveCandidateOwnerWindow(Ime::EditSession* session) {
    HWND hwnd = nullptr;
    const wchar_t* source = L"none";
    if (session != nullptr) {
        if (ITfContext* context = session->context()) {
            ITfContextView* view = nullptr;
            if (SUCCEEDED(context->GetActiveView(&view)) && view != nullptr) {
                view->GetWnd(&hwnd);
                if (hwnd != nullptr) {
                    source = L"context-view";
                }
                view->Release();
            }
        }
    }
    if (hwnd == nullptr) {
        hwnd = ::GetFocus();
        if (hwnd != nullptr) {
            source = L"GetFocus";
        }
    }
    if (hwnd == nullptr) {
        hwnd = ::GetForegroundWindow();
        if (hwnd != nullptr) {
            source = L"GetForegroundWindow";
        }
    }
    std::wostringstream log;
    log << L"[resolveCandidateOwnerWindow] session=" << session
        << L" source=" << source << L" hwnd=" << hwnd;
    appendCandidateWindowLog(log.str());
    return hwnd;
}

} // namespace

namespace Moqi {

CandidateWindow::CandidateWindow(Ime::TextService* service, Ime::EditSession* session)
    : Ime::ImeWindow(service),
      shown_(false),
      selKeyWidth_(0),
      textWidth_(0),
      commentWidth_(0),
      itemHeight_(0),
      candPerRow_(1),
      candSpacing_(kDefaultCandidateSpacing),
      colSpacing_(0),
      rowSpacing_(0),
      padX_(service->isImmersive() ? kPanelPaddingX : kInitialCompactPanelPaddingX),
      padY_(service->isImmersive() ? kPanelPaddingY : kInitialCompactPanelPaddingY),
      labelGap_(kCandidateLabelGap),
      cellGap_(kCandidateCellGap),
      borderWidth_(kBorderWidth),
      borderRadius_(kInitialBorderRadius),
      minWidth_(kInitialCandidateMinWidth),
      preeditHeight_(0),
      preeditGap_(kCandidatePreeditGap),
      contentTop_(0),
      rowPaddingY_(kCandidateRowPaddingY),
      rowInnerGap_(kCandidateRowInnerGap),
      pageNavWidth_(kPageNavWidth),
      candidatePanelWidth_(0),
      candidatePanelHeight_(0),
      backgroundColor_(kWindowBackground),
      highlightColor_(kSelectedBackground),
      textColor_(kItemText),
      highlightTextColor_(kSelectedText),
      commentColor_(kItemText),
      commentHighlightColor_(kSelectedText),
      preeditCursor_(0),
      preeditSelectionStart_(0),
      preeditSelectionEnd_(0),
      commentFont_(nullptr),
      currentSel_(0),
      pressedSel_(-1),
      pressedPageNavDirection_(kPageNavNone),
      hoveredPageNavDirection_(kPageNavNone),
      dpiX_(kWindowDpiBaseline),
      dpiY_(kWindowDpiBaseline),
      ownedFont_(nullptr),
      ownedCommentFont_(nullptr),
      draggingWindow_(false),
      trackingMouse_(false),
      useCursor_(false) {
    margin_ = 0;

    const HWND rawOwner = resolveCandidateOwnerWindow(session);
    const HWND owner = normalizeCandidateOwnerWindow(rawOwner, service->isImmersive(), L"ctor");
    updateDpiFromOwner(owner);
    {
        ThreadDpiAwarenessScope dpiScope;
        create(owner, WS_POPUP | WS_CLIPCHILDREN,
               WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED);
    }

    std::wostringstream log;
    log << L"[CandidateWindow::ctor] hwnd=" << hwnd_
        << L" raw_owner=" << rawOwner
        << L" owner=" << owner
        << L" dpi=" << dpiX_ << L"x" << dpiY_;
    appendCandidateWindowLog(log.str());
    logCandidateWindowState(L"[CandidateWindow::ctor.state]", hwnd_);
}

CandidateWindow::~CandidateWindow(void) {
    if (ownedFont_) {
        ::DeleteObject(ownedFont_);
        ownedFont_ = nullptr;
    }
    if (ownedCommentFont_) {
        ::DeleteObject(ownedCommentFont_);
        ownedCommentFont_ = nullptr;
    }
}

STDMETHODIMP CandidateWindow::GetDescription(BSTR* pbstrDescription) {
    if (!pbstrDescription) {
        return E_INVALIDARG;
    }
    *pbstrDescription = SysAllocString(L"TypeDuck 候選詞視窗 Candidate window");
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetGUID(GUID* pguid) {
    if (!pguid) {
        return E_INVALIDARG;
    }
    *pguid = {0x89671502, 0x43ab, 0x4939, {0x84, 0x6f, 0xe8, 0x30, 0x2b, 0x73, 0x7d, 0x3c}};
    return S_OK;
}

STDMETHODIMP CandidateWindow::Show(BOOL bShow) {
    shown_ = bShow;
    {
        std::wostringstream log;
        log << L"[CandidateWindow::Show] bShow=" << bShow
            << L" hwnd=" << hwnd_
            << L" owner=" << reinterpret_cast<HWND>(::GetWindowLongPtr(hwnd_, GWLP_HWNDPARENT))
            << L" gw_owner=" << ::GetWindow(hwnd_, GW_OWNER);
        appendCandidateWindowLog(log.str());
    }
    if (shown_) {
        presentLayeredSurface();
        show();
        enforceCandidateWindowTopmost(hwnd_, true, L"Show(TRUE)");
    } else {
        hide();
        logCandidateWindowState(L"[CandidateWindow::hide.state]", hwnd_);
    }
    return S_OK;
}

void CandidateWindow::refresh() {
    if (hwnd_) {
        ::InvalidateRect(hwnd_, NULL, FALSE);
    }
}

STDMETHODIMP CandidateWindow::IsShown(BOOL* pbShow) {
    if (!pbShow) {
        return E_INVALIDARG;
    }
    *pbShow = shown_;
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetUpdatedFlags(DWORD* pdwFlags) {
    if (!pdwFlags) {
        return E_INVALIDARG;
    }
    *pdwFlags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
                TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetDocumentMgr(ITfDocumentMgr** ppdim) {
    if (!textService_ || !textService_->currentContext()) {
        return E_FAIL;
    }
    return textService_->currentContext()->GetDocumentMgr(ppdim);
}

STDMETHODIMP CandidateWindow::GetCount(UINT* puCount) {
    if (!puCount) {
        return E_INVALIDARG;
    }
    const auto* service = productTextService(textService_);
    const int totalCount = service != nullptr ? service->candidateTotalCount() : 0;
    *puCount = totalCount > 0
        ? static_cast<UINT>(totalCount)
        : static_cast<UINT>(items_.size());
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetSelection(UINT* puIndex) {
    if (!puIndex) {
        return E_INVALIDARG;
    }
    assert(currentSel_ >= 0);
    const auto* service = productTextService(textService_);
    const int pageSize = service != nullptr ? service->candidatePageSize() : 0;
    const int pageIndex = service != nullptr ? service->candidatePageIndex() : 0;
    const int pageStart = pageSize > 0 ? pageIndex * pageSize : 0;
    *puIndex = static_cast<UINT>(pageStart + currentSel_);
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetString(UINT uIndex, BSTR* pbstr) {
    if (!pbstr) {
        return E_INVALIDARG;
    }
    const auto* service = productTextService(textService_);
    const int pageSize = service != nullptr ? service->candidatePageSize() : 0;
    const int pageIndex = service != nullptr ? service->candidatePageIndex() : 0;
    const UINT pageStart =
        pageSize > 0 ? static_cast<UINT>(pageIndex * pageSize) : 0;
    const UINT localIndex = pageSize > 0 ? uIndex - pageStart : uIndex;
    if (pageSize > 0 && uIndex < pageStart) {
        return E_INVALIDARG;
    }
    if (localIndex >= items_.size()) {
        return E_INVALIDARG;
    }
    *pbstr = SysAllocString(items_[localIndex].combinedText().c_str());
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetPageIndex(UINT* puIndex, UINT uSize, UINT* puPageCnt) {
    if (!puPageCnt) {
        return E_INVALIDARG;
    }
    const auto* service = productTextService(textService_);
    const int pageSize = service != nullptr ? service->candidatePageSize() : 0;
    const int totalCount = service != nullptr ? service->candidateTotalCount() : 0;
    const UINT effectivePageSize =
        pageSize > 0 ? static_cast<UINT>(pageSize)
                     : (std::max<UINT>)(1, static_cast<UINT>(items_.size()));
    const UINT effectiveTotal =
        totalCount > 0 ? static_cast<UINT>(totalCount)
                       : static_cast<UINT>(items_.size());
    *puPageCnt = (std::max<UINT>)(1, (effectiveTotal + effectivePageSize - 1) /
                                         effectivePageSize);
    if (puIndex) {
        if (uSize < *puPageCnt) {
            return E_INVALIDARG;
        }
        for (UINT i = 0; i < *puPageCnt; ++i) {
            puIndex[i] = i * effectivePageSize;
        }
    }
    return S_OK;
}

STDMETHODIMP CandidateWindow::SetPageIndex(UINT* puIndex, UINT uPageCnt) {
    (void)uPageCnt;
    if (!puIndex) {
        return E_INVALIDARG;
    }
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetCurrentPage(UINT* puPage) {
    if (!puPage) {
        return E_INVALIDARG;
    }
    const auto* service = productTextService(textService_);
    const int pageSize = service != nullptr ? service->candidatePageSize() : 0;
    const int totalCount = service != nullptr ? service->candidateTotalCount() : 0;
    const UINT effectivePageSize =
        pageSize > 0 ? static_cast<UINT>(pageSize)
                     : (std::max<UINT>)(1, static_cast<UINT>(items_.size()));
    const UINT effectiveTotal =
        totalCount > 0 ? static_cast<UINT>(totalCount)
                       : static_cast<UINT>(items_.size());
    const UINT pageCount = (std::max<UINT>)(
        1, (effectiveTotal + effectivePageSize - 1) / effectivePageSize);
    const UINT pageIndex =
        service != nullptr ? static_cast<UINT>(service->candidatePageIndex()) : 0;
    *puPage = (std::min)(pageIndex, pageCount - 1);
    return S_OK;
}

void CandidateWindow::add(CandidateUiItem item, wchar_t selKey) {
    wchar_t label[] = L"?.";
    label[0] = selKey;
    const std::wstring rawComment = item.comment;
    item.diagnosticRawComment = rawComment;
    item.displayPreferences = displayPreferences_;
    item.candidateInfo = TypeDuck::CandidateInfo(
        label, item.text, rawComment, item.inputCode);
    items_.push_back(std::move(item));
    selKeys_.push_back(selKey);
}

void CandidateWindow::clear() {
    items_.clear();
    selKeys_.clear();
    itemTextWidths_.clear();
    itemCommentWidths_.clear();
    itemWidths_.clear();
    itemHeights_.clear();
    candidatePanelWidth_ = 0;
    candidatePanelHeight_ = 0;
    if (::GetCapture() != hwnd_) {
        pressedSel_ = -1;
    }
}

void CandidateWindow::setFont(HFONT) {
    refreshOwnedFonts();
    recalculateSize();
    if (isVisible()) {
        ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::setCandPerRow(int n) {
    n = (std::max)(1, n);
    if (candPerRow_ != n) {
        candPerRow_ = n;
        recalculateSize();
    }
}

void CandidateWindow::setCandSpacing(int spacing) {
    spacing = (std::max)(0, spacing);
    if (candSpacing_ != spacing) {
        candSpacing_ = spacing;
        recalculateSize();
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setCurrentSel(int sel) {
    if (items_.empty()) {
        currentSel_ = 0;
        return;
    }
    if (sel < 0 || sel >= static_cast<int>(items_.size())) {
        sel = 0;
    }
    if (currentSel_ != sel) {
        currentSel_ = sel;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setUseCursor(bool use) {
    useCursor_ = use;
    if (isVisible()) {
        ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::setPreeditText(std::wstring text) {
    if (preedit_ != text) {
        preedit_ = std::move(text);
        if (preeditCursor_ > static_cast<int>(preedit_.length())) {
            preeditCursor_ = static_cast<int>(preedit_.length());
        }
        if (preeditSelectionStart_ > static_cast<int>(preedit_.length())) {
            preeditSelectionStart_ = static_cast<int>(preedit_.length());
        }
        if (preeditSelectionEnd_ > static_cast<int>(preedit_.length())) {
            preeditSelectionEnd_ = static_cast<int>(preedit_.length());
        }
        recalculateSize();
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setPreeditCursor(int cursor) {
    cursor = (std::max)(0, (std::min)(cursor, static_cast<int>(preedit_.length())));
    if (preeditCursor_ != cursor) {
        preeditCursor_ = cursor;
        if (isVisible() && !preedit_.empty()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setPreeditSelection(int start, int end) {
    const int length = static_cast<int>(preedit_.length());
    start = (std::max)(0, (std::min)(start, length));
    end = (std::max)(0, (std::min)(end, length));
    if (end < start) {
        std::swap(start, end);
    }
    if (preeditSelectionStart_ == start && preeditSelectionEnd_ == end) {
        return;
    }
    preeditSelectionStart_ = start;
    preeditSelectionEnd_ = end;
    if (isVisible() && !preedit_.empty()) {
        ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::setCommentFont(HFONT) {
    refreshOwnedFonts();
    recalculateSize();
    if (isVisible()) {
        ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::setBackgroundColor(COLORREF color) {
    if (backgroundColor_ != color) {
        backgroundColor_ = color;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setHighlightColor(COLORREF color) {
    if (highlightColor_ != color) {
        highlightColor_ = color;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setTextColor(COLORREF color) {
    if (textColor_ != color) {
        textColor_ = color;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setHighlightTextColor(COLORREF color) {
    if (highlightTextColor_ != color) {
        highlightTextColor_ = color;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setCommentColor(COLORREF color) {
    if (commentColor_ != color) {
        commentColor_ = color;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setCommentHighlightColor(COLORREF color) {
    if (commentHighlightColor_ != color) {
        commentHighlightColor_ = color;
        if (isVisible()) {
            ::InvalidateRect(hwnd_, NULL, TRUE);
        }
    }
}

void CandidateWindow::setDisplayPreferences(TypeDuck::DisplayPreferences preferences) {
    displayPreferences_ = std::move(preferences);
    for (auto& item : items_) {
        item.displayPreferences = displayPreferences_;
    }
    recalculateSize();
    if (isVisible()) {
        ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::syncOwner(Ime::EditSession* session) {
    if (!hwnd_) {
        return;
    }

    const HWND rawOwner = resolveCandidateOwnerWindow(session);
    const HWND owner = normalizeCandidateOwnerWindow(rawOwner, textService_->isImmersive(), L"syncOwner");
    if (owner == nullptr) {
        std::wostringstream log;
        log << L"[CandidateWindow::syncOwner] owner unavailable hwnd=" << hwnd_
            << L" current_owner="
            << reinterpret_cast<HWND>(::GetWindowLongPtr(hwnd_, GWLP_HWNDPARENT))
            << L" current_gw_owner=" << ::GetWindow(hwnd_, GW_OWNER)
            << L" raw_owner=" << rawOwner
            << L" shown=" << shown_;
        appendCandidateWindowLog(log.str());
        logCandidateWindowState(L"[CandidateWindow::syncOwner.owner_unavailable.state]", hwnd_);
        return;
    }

    const HWND currentOwner =
        reinterpret_cast<HWND>(::GetWindowLongPtr(hwnd_, GWLP_HWNDPARENT));
    const HWND currentGwOwner = ::GetWindow(hwnd_, GW_OWNER);
    bool ownerUpdated = false;
    DWORD ownerError = 0;
    if (currentOwner != owner) {
        ::SetLastError(0);
        ::SetWindowLongPtr(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
        ownerError = ::GetLastError();
        ownerUpdated = ownerError == 0;
    }

    if (shown_) {
        enforceCandidateWindowTopmost(hwnd_, true, L"syncOwner");
    }
    const bool dpiChanged = updateDpiFromOwner(owner);
    if (dpiChanged) {
        refreshOwnedFonts();
        recalculateSize();
    }

    std::wostringstream log;
    log << L"[CandidateWindow::syncOwner] hwnd=" << hwnd_
        << L" old_owner=" << currentOwner
        << L" old_gw_owner=" << currentGwOwner
        << L" raw_owner=" << rawOwner
        << L" new_owner=" << owner
        << L" dpi=" << dpiX_ << L"x" << dpiY_
        << L" shown=" << shown_
        << L" owner_updated=" << (ownerUpdated ? L"true" : L"false");
    if (ownerError != 0) {
        log << L" last_error=" << ownerError;
    }
    appendCandidateWindowLog(log.str());
    logCandidateWindowState(L"[CandidateWindow::syncOwner.state]", hwnd_);
}

bool CandidateWindow::updateDpiFromOwner(HWND owner) {
    const DpiPair dpi = dpiForOwnerWindow(owner ? owner : hwnd_);
    if (dpi.x == dpiX_ && dpi.y == dpiY_) {
        return false;
    }
    dpiX_ = dpi.x;
    dpiY_ = dpi.y;
    return true;
}

void CandidateWindow::refreshOwnedFonts() {
    auto* service = productTextService(textService_);
    const std::wstring candidateFace = service->candFontName().empty()
                                           ? kDefaultCandidateFontName
                                           : resolveFontFace(service->candFontName(), kDefaultCandidateFontName);
    const std::wstring commentFace = service->candCommentFontName().empty()
                                         ? kDefaultCommentFontName
                                         : resolveFontFace(service->candCommentFontName(), kDefaultCommentFontName);

    HFONT nextFont = createPointFontForDpi(dpiY_, candidateFace.c_str(), service->candFontSize());
    HFONT nextCommentFont = createPointFontForDpi(dpiY_, commentFace.c_str(), service->candCommentFontSize());

    if (ownedFont_) {
        ::DeleteObject(ownedFont_);
    }
    if (ownedCommentFont_) {
        ::DeleteObject(ownedCommentFont_);
    }
    ownedFont_ = nextFont;
    ownedCommentFont_ = nextCommentFont;
    font_ = ownedFont_ ? ownedFont_ : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    commentFont_ = ownedCommentFont_;
}

void CandidateWindow::recalculateSize() {
    ThreadDpiAwarenessScope dpiScope;
    if (items_.empty() && preedit_.empty()) {
        selKeyWidth_ = 0;
        textWidth_ = 0;
        commentWidth_ = 0;
        itemHeight_ = 0;
        preeditHeight_ = 0;
        contentTop_ = borderWidth_ + padY_;
        candidatePanelWidth_ = padX_ * 2 + borderWidth_ * 2;
        candidatePanelHeight_ = padY_ * 2 + borderWidth_ * 2;
        resizeForLayout(candidatePanelWidth_, candidatePanelHeight_);
        applyWindowShape();
        return;
    }

    HDC hdc = ::GetWindowDC(hwnd());
    if (!hdc) {
        return;
    }

    const int scaledPadX = scalePx(textService_->isImmersive() ? kImmersivePanelPaddingX : kPanelPaddingX);
    const int scaledPadY = scalePx(textService_->isImmersive() ? kImmersivePanelPaddingY : kPanelPaddingY);
    padX_ = scaledPadX;
    padY_ = scaledPadY;
    labelGap_ = scalePx(kCandidateLabelGap);
    cellGap_ = scalePx(kCandidateCellGap);
    borderWidth_ = (std::max)(kBorderWidth, scalePx(kBorderWidth));
    borderRadius_ = scalePx(kCandidateBorderRadius);
    preeditGap_ = scalePx(kCandidatePreeditGap);
    rowPaddingY_ = scalePx(kCandidateRowPaddingY);
    rowInnerGap_ = scalePx(kCandidateRowInnerGap);
    pageNavWidth_ = scalePx(kPageNavWidth);
    minWidth_ = scalePx(kCandidateMinWidth);

    selKeyWidth_ = 0;
    textWidth_ = 0;
    commentWidth_ = 0;
    itemHeight_ = 0;
    preeditHeight_ = 0;
    itemTextWidths_.assign(items_.size(), 0);
    itemCommentWidths_.assign(items_.size(), 0);
    itemWidths_.assign(items_.size(), 0);
    itemHeights_.assign(items_.size(), 0);
    int preeditWidth = 0;
    int honziContentWidth = 0;

    HGDIOBJ oldFont = ::SelectObject(hdc, font_);
    HFONT rowMetaFont = createPointFontForDpi(dpiY_, L"Segoe UI", 12);
    TEXTMETRICW metrics = {};
    ::GetTextMetricsW(hdc, &metrics);
    const int bodyLineHeight = (std::max)(
        scalePx(kCandidateBodyLineMinHeight),
        static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    for (int i = 0, n = static_cast<int>(items_.size()); i < n; ++i) {
        SIZE selKeySize = {};
        wchar_t selKey[] = L"?.";
        selKey[0] = selKeys_[i];
        HGDIOBJ previousFont = ::SelectObject(hdc, rowMetaFont ? rowMetaFont : font_);
        ::GetTextExtentPoint32W(hdc, selKey, 2, &selKeySize);
        ::SelectObject(hdc, previousFont);
        selKeyWidth_ = (std::max)(selKeyWidth_, static_cast<int>(selKeySize.cx));

        SIZE candidateSize = {};
        const std::wstring itemText = items_[i].displayText();
        ::GetTextExtentPoint32W(hdc, itemText.c_str(), static_cast<int>(itemText.length()), &candidateSize);
        itemTextWidths_[i] = static_cast<int>(candidateSize.cx) + 1;
        textWidth_ = (std::max)(textWidth_, itemTextWidths_[i]);
        honziContentWidth = (std::max)(honziContentWidth, itemTextWidths_[i]);
        itemHeights_[i] = rowPaddingY_ * 2 + bodyLineHeight;
        itemHeight_ = (std::max)(itemHeight_, itemHeights_[i]);
    }
    if (!preedit_.empty()) {
        HFONT inputFont = createDerivedFont(font_, kInputBufferFontName);
        HGDIOBJ previousFont = ::SelectObject(hdc, inputFont ? inputFont : font_);
        SIZE preeditSize = {};
        ::GetTextExtentPoint32W(hdc, preedit_.c_str(), static_cast<int>(preedit_.length()), &preeditSize);
        ::SelectObject(hdc, previousFont);
        if (inputFont) {
            ::DeleteObject(inputFont);
        }
        preeditWidth = static_cast<int>(preeditSize.cx) + scalePx(kPreeditTextWidthPadding);
        textWidth_ = (std::max)(textWidth_, static_cast<int>(preeditSize.cx));
        preeditHeight_ = static_cast<int>(preeditSize.cy);
    }

    if (rowMetaFont) {
        ::DeleteObject(rowMetaFont);
    }
    ::SelectObject(hdc, oldFont);
    ::ReleaseDC(hwnd(), hdc);

    preeditHeight_ = preedit_.empty()
                         ? 0
                         : (std::max)(preeditHeight_, static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));

    const int cellPadX = scalePx(kCandidateCellPadX);
    const int cellMaxTextWidth = scalePx(kCandidateCellMaxTextWidth);
    const int cellMinTextWidth = scalePx(kCandidateCellMinTextWidth);
    const int cellFixedWidth = selKeyWidth_ + labelGap_ + cellPadX * 2;
    const int n = static_cast<int>(items_.size());
    std::vector<int> cellTextWidths(n, 0);
    for (int i = 0; i < n; ++i) {
        cellTextWidths[i] = (std::min)(itemTextWidths_[i], cellMaxTextWidth);
    }

    // Monitor work area is physical pixels while the per-monitor DPI awareness
    // scope is active, matching every scalePx()-derived width below.
    int workAreaWidth = 0;
    {
        const HWND owner = hwnd_
            ? reinterpret_cast<HWND>(::GetWindowLongPtr(hwnd_, GWLP_HWNDPARENT))
            : nullptr;
        HMONITOR monitor = ::MonitorFromWindow(owner ? owner : hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitorInfo)) {
            workAreaWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        }
        if (workAreaWidth <= 0) {
            workAreaWidth = ::GetSystemMetrics(SM_CXFULLSCREEN);
        }
    }

    const int navWidth = n > 0 ? cellGap_ + pageNavWidth_ : 0;
    if (n > 0 && workAreaWidth > 0) {
        const int availableTextWidth = workAreaWidth -
                                       (padX_ * 2 + borderWidth_ * 2) -
                                       navWidth -
                                       n * cellFixedWidth -
                                       (n - 1) * cellGap_;
        int totalTextWidth = 0;
        for (int i = 0; i < n; ++i) {
            totalTextWidth += cellTextWidths[i];
        }
        if (totalTextWidth > availableTextWidth) {
            int cap = cellMaxTextWidth;
            while (cap > cellMinTextWidth) {
                int cappedTotal = 0;
                for (int i = 0; i < n; ++i) {
                    cappedTotal += (std::min)(cellTextWidths[i], cap);
                }
                if (cappedTotal <= availableTextWidth) {
                    break;
                }
                --cap;
            }
            for (int i = 0; i < n; ++i) {
                cellTextWidths[i] = (std::min)(cellTextWidths[i], cap);
            }
        }
    }

    int candidateRowWidth = 0;
    for (int i = 0; i < n; ++i) {
        itemWidths_[i] = cellFixedWidth + cellTextWidths[i];
        if (i > 0) {
            candidateRowWidth += cellGap_;
        }
        candidateRowWidth += itemWidths_[i];
    }
    candidateRowWidth += navWidth;

    const int contentWidth = (std::max)((std::max)(candidateRowWidth, preeditWidth), minWidth_);
    const int candidatePanelWidth = padX_ * 2 + contentWidth + borderWidth_ * 2;
    int candidatePanelHeight = itemHeight_;
    if (!preedit_.empty()) {
        contentTop_ = borderWidth_ + padY_ + preeditHeight_ + preeditGap_;
        candidatePanelHeight += preeditHeight_ + preeditGap_;
    } else {
        contentTop_ = borderWidth_ + padY_;
    }
    candidatePanelHeight += padY_ * 2 + borderWidth_ * 2;
    candidatePanelWidth_ = candidatePanelWidth;
    candidatePanelHeight_ = candidatePanelHeight;
    resizeForLayout(candidatePanelWidth_, candidatePanelHeight_);
    applyWindowShape();

    std::wostringstream log;
    log << L"[CandidateWindow::recalculateSize] items=" << items_.size()
        << L" width=" << candidatePanelWidth_ << L" height=" << candidatePanelHeight_
        << L" honzi_content_width=" << honziContentWidth
        << L" work_area_width=" << workAreaWidth
        << L" perRow=" << candPerRow_;
    appendCandidateWindowLog(log.str());
}

LRESULT CandidateWindow::wndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        onPaint();
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_LBUTTONDOWN:
        onLButtonDown(wp, lp);
        return 0;
    case WM_MOUSEMOVE:
        onMouseMove(wp, lp);
        return 0;
    case WM_LBUTTONUP:
        onLButtonUp(wp, lp);
        return 0;
    case WM_MOUSELEAVE:
        onMouseLeave();
        return 0;
    case WM_MOUSEWHEEL:
        onMouseWheel(wp, lp);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return Window::wndProc(msg, wp, lp);
    }
}

void CandidateWindow::onPaint() {
    PAINTSTRUCT ps = {};
    BeginPaint(hwnd_, &ps);
    HDC hdc = ps.hdc;

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    if (usesLayeredPresentation()) {
        EndPaint(hwnd_, &ps);
        presentLayeredSurface();
        return;
    }

    HDC memdc = ::CreateCompatibleDC(hdc);
    HBITMAP membmp = ::CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
    HGDIOBJ oldBitmap = ::SelectObject(memdc, membmp);
    renderSurface(memdc, rc, false);
    ::BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, memdc, 0, 0, SRCCOPY);

    ::SelectObject(memdc, oldBitmap);
    ::DeleteObject(membmp);
    ::DeleteDC(memdc);
    EndPaint(hwnd_, &ps);
}

void CandidateWindow::renderSurface(HDC hdc, const RECT& rc, bool transparentOutsidePanels) {
    HGDIOBJ oldFont = ::SelectObject(hdc, font_);
    ::SetBkMode(hdc, TRANSPARENT);

    HBRUSH clearBrush = ::CreateSolidBrush(
        transparentOutsidePanels ? kLayeredTransparentColor : backgroundColor_);
    ::FillRect(hdc, &rc, clearBrush);
    ::DeleteObject(clearBrush);

    HBRUSH backgroundBrush = ::CreateSolidBrush(backgroundColor_);
    HBRUSH borderBrush = ::CreateSolidBrush(kWindowBorder);
    const int candidatePanelWidth = candidatePanelWidth_ > 0 ? candidatePanelWidth_ : rc.right - rc.left;
    RECT candidatePanelRc = {rc.left, rc.top, candidatePanelWidth, rc.top + candidatePanelHeight_};
    HRGN windowRgn = ::CreateRoundRectRgn(
        candidatePanelRc.left, candidatePanelRc.top,
        candidatePanelRc.right + 1, candidatePanelRc.bottom + 1,
        borderRadius_ * 2, borderRadius_ * 2);
    ::FillRgn(hdc, windowRgn, backgroundBrush);
    ::FrameRgn(hdc, windowRgn, borderBrush, borderWidth_, borderWidth_);

    paintInputBuffer(hdc, candidatePanelRc);
    if (!items_.empty()) {
        paintPageNavigation(hdc, candidatePanelRc);
    }

    int x = borderWidth_ + padX_;
    for (int i = 0, n = static_cast<int>(items_.size()); i < n; ++i) {
        RECT cellRc = {x, contentTop_, x + itemWidth(i), contentTop_ + itemHeight(i)};
        paintCandidateRow(hdc, i, cellRc);
        x = cellRc.right + cellGap_;
    }

    ::DeleteObject(windowRgn);
    ::DeleteObject(borderBrush);
    ::DeleteObject(backgroundBrush);
    ::SelectObject(hdc, oldFont);
}

void CandidateWindow::presentLayeredSurface() {
    if (!hwnd_ || !usesLayeredPresentation()) {
        return;
    }

    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC screenDc = ::GetDC(nullptr);
    if (!screenDc) {
        return;
    }
    HDC memdc = ::CreateCompatibleDC(screenDc);
    HBITMAP membmp = ::CreateCompatibleBitmap(screenDc, width, height);
    if (!memdc || !membmp) {
        if (membmp) {
            ::DeleteObject(membmp);
        }
        if (memdc) {
            ::DeleteDC(memdc);
        }
        ::ReleaseDC(nullptr, screenDc);
        return;
    }

    HGDIOBJ oldBitmap = ::SelectObject(memdc, membmp);
    renderSurface(memdc, rc, true);

    RECT windowRc = {};
    ::GetWindowRect(hwnd_, &windowRc);
    POINT dst = {windowRc.left, windowRc.top};
    POINT src = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, 0};
    ::SetLastError(0);
    const BOOL ok = ::UpdateLayeredWindow(
        hwnd_,
        screenDc,
        &dst,
        &size,
        memdc,
        &src,
        kLayeredTransparentColor,
        &blend,
        ULW_COLORKEY);
    if (!ok) {
        std::wostringstream log;
        log << L"[CandidateWindow::presentLayeredSurface] UpdateLayeredWindow failed last_error="
            << ::GetLastError();
        appendCandidateWindowLog(log.str());
    }

    ::SelectObject(memdc, oldBitmap);
    ::DeleteObject(membmp);
    ::DeleteDC(memdc);
    ::ReleaseDC(nullptr, screenDc);
}

void CandidateWindow::paintInputBuffer(HDC hdc, const RECT& panelRc) {
    if (preedit_.empty()) {
        return;
    }

    RECT preeditRc = {
        panelRc.left + borderWidth_ + padX_ / 2,
        panelRc.top + borderWidth_ + padY_,
        panelRc.right - borderWidth_ - padX_,
        panelRc.top + borderWidth_ + padY_ + preeditHeight_ + scalePx(kPreeditExtraHeight)};
    HFONT inputFont = createDerivedFont(font_, kInputBufferFontName);
    HGDIOBJ oldFont = ::SelectObject(hdc, inputFont ? inputFont : font_);
    ::SetBkMode(hdc, TRANSPARENT);

    const int length = static_cast<int>(preedit_.length());
    int activeStart = (std::max)(0, (std::min)(preeditSelectionStart_, length));
    int activeEnd = (std::max)(0, (std::min)(preeditSelectionEnd_, length));
    if (activeEnd < activeStart) {
        std::swap(activeStart, activeEnd);
    }
    const std::wstring before = preedit_.substr(0, activeStart);
    const std::wstring active = preedit_.substr(activeStart, activeEnd - activeStart);
    const std::wstring after = preedit_.substr(activeEnd);
    auto textWidth = [&](const std::wstring& text) {
        if (text.empty()) {
            return 0;
        }
        SIZE size = {};
        ::GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.length()), &size);
        return static_cast<int>(size.cx);
    };

    const int plainMargin = scalePx(kPreeditPlainMargin);
    const int activePadX = scalePx(kPreeditActivePaddingX);
    int x = preeditRc.left;

    if (!before.empty()) {
        RECT beforeRc = {x + plainMargin, preeditRc.top, preeditRc.right, preeditRc.bottom};
        ::SetTextColor(hdc, kItemText);
        ::DrawTextW(hdc, before.c_str(), static_cast<int>(before.length()), &beforeRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        x += textWidth(before) + plainMargin * 2;
    }

    const int activeWidth = textWidth(active);
    RECT activeRc = {x, preeditRc.top, x, preeditRc.bottom};
    RECT activeTextRc = activeRc;
    if (!active.empty() && activeWidth > 0) {
        activeRc.right = (std::min)(static_cast<int>(preeditRc.right), x + activeWidth + activePadX * 2);
        HBRUSH inputBrush = ::CreateSolidBrush(kInputBufferBackground);
        HRGN activeRgn = ::CreateRoundRectRgn(activeRc.left, activeRc.top, activeRc.right + 1,
                                              activeRc.bottom + 1,
                                              scalePx(kPreeditActiveCornerRadius) * 2,
                                              scalePx(kPreeditActiveCornerRadius) * 2);
        ::FillRgn(hdc, activeRgn, inputBrush);
        ::DeleteObject(activeRgn);
        ::DeleteObject(inputBrush);

        activeTextRc = activeRc;
        activeTextRc.left += activePadX;
        activeTextRc.right -= activePadX;
        ::SetTextColor(hdc, kInputBufferText);
        ::DrawTextW(hdc, active.c_str(), static_cast<int>(active.length()), &activeTextRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        x = activeRc.right;
    }
    if (!after.empty() && x < preeditRc.right) {
        RECT afterRc = {x + plainMargin, preeditRc.top, preeditRc.right, preeditRc.bottom};
        ::SetTextColor(hdc, kItemText);
        ::DrawTextW(hdc, after.c_str(), static_cast<int>(after.length()), &afterRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    auto cursorXForIndex = [&](int index) {
        index = (std::max)(0, (std::min)(index, length));
        if (index <= activeStart) {
            return preeditRc.left + plainMargin + textWidth(preedit_.substr(0, index));
        }
        if (index <= activeEnd) {
            return activeTextRc.left + textWidth(preedit_.substr(activeStart, index - activeStart));
        }
        return activeRc.right + plainMargin + textWidth(preedit_.substr(activeEnd, index - activeEnd));
    };
    paintPreeditCursor(hdc, preeditRc, cursorXForIndex(preeditCursor_));

    const int dividerY = preeditRc.bottom + preeditGap_ / 2;
    HPEN dividerPen = ::CreatePen(PS_SOLID, 1, kDividerColor);
    HGDIOBJ oldPen = ::SelectObject(hdc, dividerPen);
    ::MoveToEx(hdc, panelRc.left + borderWidth_ + padX_, dividerY, nullptr);
    ::LineTo(hdc, panelRc.right - borderWidth_ - padX_, dividerY);
    ::SelectObject(hdc, oldPen);
    ::DeleteObject(dividerPen);
    ::SelectObject(hdc, oldFont);
    if (inputFont) {
        ::DeleteObject(inputFont);
    }
}

void CandidateWindow::paintPageNavigation(HDC hdc, const RECT& panelRc) {
    const bool hasPrev = isPageNavigationEnabled(false);
    const bool hasNext = isPageNavigationEnabled(true);
    RECT prevRc = {};
    RECT nextRc = {};
    pageNavigationButtonRect(false, prevRc);
    pageNavigationButtonRect(true, nextRc);
    ::OffsetRect(&prevRc, panelRc.left, panelRc.top);
    ::OffsetRect(&nextRc, panelRc.left, panelRc.top);

    auto paintNavBackground = [&](int direction, const RECT& buttonRc, bool enabled) {
        if (!enabled) {
            return;
        }
        if (pressedPageNavDirection_ != direction && hoveredPageNavDirection_ != direction) {
            return;
        }
        HBRUSH brush = ::CreateSolidBrush(kWindowBorder);
        HBRUSH oldBrush = static_cast<HBRUSH>(::SelectObject(hdc, brush));
        HPEN pen = ::CreatePen(PS_SOLID, 1, kWindowBorder);
        HPEN oldPen = static_cast<HPEN>(::SelectObject(hdc, pen));
        ::RoundRect(hdc, buttonRc.left, buttonRc.top, buttonRc.right, buttonRc.bottom,
                    scalePx(kPageNavHoverRadius), scalePx(kPageNavHoverRadius));
        ::SelectObject(hdc, oldPen);
        ::SelectObject(hdc, oldBrush);
        ::DeleteObject(pen);
        ::DeleteObject(brush);
    };
    paintNavBackground(kPageNavPrevious, prevRc, hasPrev);
    paintNavBackground(kPageNavNext, nextRc, hasNext);

    HFONT navFont = createPointFontForDpi(dpiY_, L"Segoe UI Symbol", kPageNavGlyphPointSize);
    HGDIOBJ oldFont = ::SelectObject(hdc, navFont ? navFont : font_);
    auto drawNavGlyph = [&](const wchar_t* glyph, const RECT& glyphRc, COLORREF color) {
        SIZE glyphSize = {};
        ::GetTextExtentPoint32W(hdc, glyph, 1, &glyphSize);
        const int x = glyphRc.left + (glyphRc.right - glyphRc.left - glyphSize.cx) / 2;
        const int y = glyphRc.top + (glyphRc.bottom - glyphRc.top - glyphSize.cy) / 2 -
                      scalePx(kPageNavGlyphYOffset);
        ::SetTextColor(hdc, color);
        ::TextOutW(hdc, x, y, glyph, 1);
    };
    drawNavGlyph(L"‹", prevRc, hasPrev ? kLinkText : kDisabledText);
    drawNavGlyph(L"›", nextRc, hasNext ? kLinkText : kDisabledText);
    ::SelectObject(hdc, oldFont);
    if (navFont) {
        ::DeleteObject(navFont);
    }
}

void CandidateWindow::paintCandidateRow(HDC hdc, int index, const RECT& rowRc) {
    const bool selected = useCursor_ && index == currentSel_;

    if (selected) {
        HBRUSH highlightBrush = ::CreateSolidBrush(highlightColor_);
        HRGN rowRgn = ::CreateRoundRectRgn(rowRc.left, rowRc.top, rowRc.right + 1, rowRc.bottom + 1,
                                           scalePx(kCandidateRowCornerRadius) * 2,
                                           scalePx(kCandidateRowCornerRadius) * 2);
        ::FillRgn(hdc, rowRgn, highlightBrush);
        ::DeleteObject(rowRgn);
        ::DeleteObject(highlightBrush);
    }

    wchar_t selKey[] = L"?.";
    selKey[0] = selKeys_[index];
    HFONT rowMetaFont = createPointFontForDpi(dpiY_, L"Segoe UI", 12);
    HGDIOBJ oldFont = ::SelectObject(hdc, rowMetaFont ? rowMetaFont : font_);
    const COLORREF oldColor = ::SetTextColor(hdc, selected ? commentHighlightColor_ : kSecondaryText);

    const int cellPadX = scalePx(kCandidateCellPadX);
    RECT selRc = rowRc;
    selRc.left += cellPadX;
    selRc.right = selRc.left + selKeyWidth_;
    ::DrawTextW(hdc, selKey, 2, &selRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ::SelectObject(hdc, font_);
    ::SetTextColor(hdc, selected ? highlightTextColor_ : textColor_);
    const std::wstring text = items_[index].displayText();
    RECT textRc = rowRc;
    textRc.left = selRc.right + labelGap_;
    textRc.right = rowRc.right - cellPadX;
    ::DrawTextW(hdc, text.c_str(), static_cast<int>(text.length()), &textRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    ::SelectObject(hdc, oldFont);
    if (rowMetaFont) {
        ::DeleteObject(rowMetaFont);
    }
    ::SetTextColor(hdc, oldColor);
}

void CandidateWindow::paintPreeditCursor(HDC hdc, const RECT& preeditRc, int cursorX) {
    if (preedit_.empty()) {
        return;
    }

    cursorX = (std::max)(static_cast<int>(preeditRc.left),
                         (std::min)(static_cast<int>(preeditRc.right - 1), cursorX));
    const int cursorWidth = kPreeditCursorWidth;
    RECT cursorRc = {
        cursorX,
        preeditRc.top + kPreeditCursorVerticalInset,
        cursorX + cursorWidth,
        preeditRc.bottom - kPreeditCursorVerticalInset};
    HBRUSH cursorBrush = ::CreateSolidBrush(textColor_);
    ::FillRect(hdc, &cursorRc, cursorBrush);
    ::DeleteObject(cursorBrush);
}

int CandidateWindow::hitTestCandidate(POINT pt) const {
    if (items_.empty()) {
        return -1;
    }

    for (int i = 0, n = static_cast<int>(items_.size()); i < n; ++i) {
        RECT rect = {};
        itemRect(i, rect);
        if (::PtInRect(&rect, pt)) {
            return i;
        }
    }
    return -1;
}

void CandidateWindow::pageNavigationButtonRect(bool next, RECT& rect) const {
    const int candidatePanelRight = candidatePanelWidth_ > 0
                                        ? candidatePanelWidth_
                                        : padX_ * 2 + minWidth_ + borderWidth_ * 2;
    const int rowHeight = itemHeight_ > 0 ? itemHeight_ : scalePx(kPageNavPreeditlessHeight);
    rect = {
        candidatePanelRight - borderWidth_ - padX_ - pageNavWidth_,
        contentTop_,
        candidatePanelRight - borderWidth_ - padX_,
        contentTop_ + rowHeight};
    if (next) {
        rect.left += pageNavWidth_ / 2;
    } else {
        rect.right = rect.left + pageNavWidth_ / 2;
    }
}

bool CandidateWindow::isPageNavigationEnabled(bool next) const {
    const auto* service = productTextService(textService_);
    if (!service) {
        return false;
    }
    if (next) {
        if (service->candidateHasNext()) {
            return true;
        }
        const int pageSize = service->candidatePageSize();
        const int totalCount = service->candidateTotalCount();
        return pageSize > 0 && totalCount > (service->candidatePageIndex() + 1) * pageSize;
    }
    return service->candidateHasPrevious() || service->candidatePageIndex() > 0;
}

int CandidateWindow::hitTestPageNavigation(POINT pt) const {
    if (items_.empty() || pageNavWidth_ <= 0 || candidatePanelWidth_ <= 0) {
        return kPageNavNone;
    }
    RECT prevRc = {};
    RECT nextRc = {};
    pageNavigationButtonRect(false, prevRc);
    pageNavigationButtonRect(true, nextRc);
    if (isPageNavigationEnabled(false) && ::PtInRect(&prevRc, pt)) {
        return kPageNavPrevious;
    }
    if (isPageNavigationEnabled(true) && ::PtInRect(&nextRc, pt)) {
        return kPageNavNext;
    }
    return kPageNavNone;
}

void CandidateWindow::onLButtonDown(WPARAM wp, LPARAM lp) {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const int navDirection = hitTestPageNavigation(pt);
    if (navDirection != kPageNavNone) {
        pressedPageNavDirection_ = navDirection;
        hoveredPageNavDirection_ = navDirection;
        pressedSel_ = -1;
        draggingWindow_ = false;
        ::InvalidateRect(hwnd_, NULL, FALSE);
        ::SetCapture(hwnd_);
        return;
    }

    const int hitIndex = hitTestCandidate(pt);
    if (hitIndex >= 0) {
        pressedSel_ = hitIndex;
        pressedPageNavDirection_ = kPageNavNone;
        draggingWindow_ = false;
        ::SetCapture(hwnd_);
        return;
    }

    pressedSel_ = -1;
    pressedPageNavDirection_ = kPageNavNone;
    draggingWindow_ = true;
    Ime::ImeWindow::onLButtonDown(wp, lp);
}

void CandidateWindow::onLButtonUp(WPARAM wp, LPARAM lp) {
    const bool hadCapture = ::GetCapture() == hwnd_;
    if (hadCapture) {
        ::ReleaseCapture();
    }

    if (draggingWindow_) {
        draggingWindow_ = false;
        Ime::ImeWindow::onLButtonUp(wp, lp);
        return;
    }

    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const int navDirection = hitTestPageNavigation(pt);
    if (pressedPageNavDirection_ != kPageNavNone &&
        navDirection == pressedPageNavDirection_) {
        const int direction = pressedPageNavDirection_;
        pressedPageNavDirection_ = kPageNavNone;
        hoveredPageNavDirection_ = navDirection;
        ::InvalidateRect(hwnd_, NULL, FALSE);
        if (auto* textService = static_cast<Moqi::TextService*>(textService_)) {
            textService->changeCandidatePage(direction == kPageNavPrevious);
        }
        pressedSel_ = -1;
        return;
    }
    pressedPageNavDirection_ = kPageNavNone;

    const int hitIndex = hitTestCandidate(pt);
    if (pressedSel_ >= 0 && hitIndex >= 0 &&
        (hitIndex == currentSel_ || hitIndex == pressedSel_)) {
        if (auto* textService = static_cast<Moqi::TextService*>(textService_)) {
            textService->selectCandidate(hitIndex);
        }
    }
    pressedSel_ = -1;
    ::InvalidateRect(hwnd_, NULL, FALSE);
}

void CandidateWindow::onMouseMove(WPARAM wp, LPARAM lp) {
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        if (::TrackMouseEvent(&tme)) {
            trackingMouse_ = true;
        }
    }

    if (draggingWindow_) {
        Ime::ImeWindow::onMouseMove(wp, lp);
        return;
    }

    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const int navDirection = hitTestPageNavigation(pt);
    if (hoveredPageNavDirection_ != navDirection) {
        hoveredPageNavDirection_ = navDirection;
        ::InvalidateRect(hwnd_, NULL, FALSE);
    }
}

void CandidateWindow::onMouseLeave() {
    trackingMouse_ = false;
    hoveredPageNavDirection_ = kPageNavNone;
    pressedPageNavDirection_ = kPageNavNone;
    if (isVisible()) {
        ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::onMouseWheel(WPARAM wp, LPARAM lp) {
    (void)lp;
    const short delta = GET_WHEEL_DELTA_WPARAM(wp);
    if (delta == 0) {
        return;
    }
    if (auto* textService = static_cast<Moqi::TextService*>(textService_)) {
        textService->changeCandidatePage(delta > 0);
    }
}

int CandidateWindow::scalePx(int value) const {
    return ::MulDiv(value, (std::max)(kWindowDpiBaseline, dpiX_), kWindowDpiBaseline);
}

void CandidateWindow::itemRect(int index, RECT& rect) const {
    rect.left = borderWidth_ + padX_;
    for (int i = 0; i < index && i < static_cast<int>(itemWidths_.size()); ++i) {
        rect.left += itemWidths_[i] + cellGap_;
    }
    rect.top = contentTop_;
    rect.right = rect.left + itemWidth(index);
    rect.bottom = rect.top + itemHeight(index);
}

int CandidateWindow::itemWidth(int index) const {
    if (index >= 0 && index < static_cast<int>(itemWidths_.size())) {
        return itemWidths_[index];
    }
    return selKeyWidth_ + labelGap_ + scalePx(kCandidateCellPadX) * 2 + textWidth_;
}

int CandidateWindow::itemHeight(int index) const {
    if (index >= 0 && index < static_cast<int>(itemHeights_.size())) {
        return itemHeights_[index];
    }
    return itemHeight_;
}

int CandidateWindow::itemTextWidth(int index) const {
    if (index >= 0 && index < static_cast<int>(itemTextWidths_.size())) {
        return itemTextWidths_[index];
    }
    return textWidth_;
}

int CandidateWindow::itemCommentWidth(int index) const {
    if (index >= 0 && index < static_cast<int>(itemCommentWidths_.size())) {
        return itemCommentWidths_[index];
    }
    return commentWidth_;
}

void CandidateWindow::applyWindowShape() {
    if (!hwnd_) {
        return;
    }
    if (usesLayeredPresentation()) {
        return;
    }

    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }

    const int candidatePanelWidth = candidatePanelWidth_ > 0 ? candidatePanelWidth_ : rc.right - rc.left;
    HRGN region = ::CreateRoundRectRgn(
        rc.left, rc.top,
        candidatePanelWidth + 1,
        rc.top + candidatePanelHeight_ + 1,
        borderRadius_ * 2,
        borderRadius_ * 2);
    ::SetWindowRgn(hwnd_, region, TRUE);
}

bool CandidateWindow::usesLayeredPresentation() const {
    return hwnd_ &&
           (::GetWindowLongPtr(hwnd_, GWL_EXSTYLE) & WS_EX_LAYERED) == WS_EX_LAYERED;
}

void CandidateWindow::resizeForLayout(int width, int height) {
    UINT flags = SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE;
    if (usesLayeredPresentation()) {
        flags |= SWP_NOREDRAW;
    }
    ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, width, height, flags);
}

} // namespace Moqi
