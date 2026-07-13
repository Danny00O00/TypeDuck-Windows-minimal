//
//    Copyright (C) 2026
//

#pragma once

#include <LibIME2/src/ComObject.h>
#include <LibIME2/src/ImeWindow.h>
#include "TypeDuckCandidateInfo.h"

#include <string>
#include <vector>

namespace Ime {
class EditSession;
}

namespace Moqi {

struct CandidateUiItem {
    std::wstring text;
    std::wstring comment;
    std::wstring inputCode;
    std::wstring diagnosticRawComment;
    TypeDuck::CandidateInfo candidateInfo;
    TypeDuck::DisplayPreferences displayPreferences;

    std::wstring displayText() const {
        return candidateInfo.displayCandidateText(displayPreferences);
    }

    std::wstring displayComment() const {
        return candidateInfo.displayComment(displayPreferences);
    }

    bool operator==(const CandidateUiItem& other) const {
        return text == other.text && comment == other.comment &&
               inputCode == other.inputCode &&
               diagnosticRawComment == other.diagnosticRawComment;
    }

    bool operator!=(const CandidateUiItem& other) const {
        return !(*this == other);
    }

    std::wstring combinedText() const {
        const std::wstring visibleText = displayText();
        const std::wstring visibleComment = displayComment();
        if (visibleComment.empty()) {
            return visibleText;
        }
        return visibleText + L" " + visibleComment;
    }
};

class CandidateWindow
    : public Ime::ImeWindow,
      public Ime::ComObject<Ime::ComInterface<ITfCandidateListUIElement>> {
public:
    CandidateWindow(Ime::TextService* service, Ime::EditSession* session);

    STDMETHODIMP GetDescription(BSTR* pbstrDescription);
    STDMETHODIMP GetGUID(GUID* pguid);
    STDMETHODIMP Show(BOOL bShow);
    STDMETHODIMP IsShown(BOOL* pbShow);

    STDMETHODIMP GetUpdatedFlags(DWORD* pdwFlags);
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** ppdim);
    STDMETHODIMP GetCount(UINT* puCount);
    STDMETHODIMP GetSelection(UINT* puIndex);
    STDMETHODIMP GetString(UINT uIndex, BSTR* pbstr);
    STDMETHODIMP GetPageIndex(UINT* puIndex, UINT uSize, UINT* puPageCnt);
    STDMETHODIMP SetPageIndex(UINT* puIndex, UINT uPageCnt);
    STDMETHODIMP GetCurrentPage(UINT* puPage);

    void add(CandidateUiItem item, wchar_t selKey);
    void clear();
    void setFont(HFONT font);
    void setCandPerRow(int n);
    void setCandSpacing(int spacing);
    void setCurrentSel(int sel);
    void setUseCursor(bool use);
    void setPreeditText(std::wstring text);
    void setPreeditCursor(int cursor);
    void setPreeditSelection(int start, int end);
    void setCommentFont(HFONT font);
    void setBackgroundColor(COLORREF color);
    void setHighlightColor(COLORREF color);
    void setTextColor(COLORREF color);
    void setHighlightTextColor(COLORREF color);
    void setCommentColor(COLORREF color);
    void setCommentHighlightColor(COLORREF color);
    void setDisplayPreferences(TypeDuck::DisplayPreferences preferences);
    void syncOwner(Ime::EditSession* session);
    // Screen rect of the caret the popup is anchored to. Both the DPI and the work-area
    // width are resolved from the monitor this rect lands on, i.e. the monitor the popup
    // is actually placed on, which is not necessarily the owner window's monitor.
    void setAnchorRect(const RECT& rect);
    void recalculateSize() override;
    void refresh();

protected:
    ~CandidateWindow(void) override;

    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void onPaint();
    void renderSurface(HDC hdc, const RECT& rc, bool transparentOutsidePanels);
    void presentLayeredSurface();
    void paintInputBuffer(HDC hdc, const RECT& panelRc);
    void paintPageNavigation(HDC hdc, const RECT& panelRc);
    void paintCandidateRow(HDC hdc, int index, const RECT& rowRc);
    void itemRect(int index, RECT& rect) const;
    void pageNavigationButtonRect(bool next, RECT& rect) const;
    int itemWidth(int index) const;
    int itemHeight(int index) const;
    int itemTextWidth(int index) const;
    int itemCommentWidth(int index) const;
    int hitTestCandidate(POINT pt) const;
    int hitTestPageNavigation(POINT pt) const;
    bool isPageNavigationEnabled(bool next) const;
    void onLButtonDown(WPARAM wp, LPARAM lp);
    void onLButtonUp(WPARAM wp, LPARAM lp);
    void onMouseMove(WPARAM wp, LPARAM lp);
    void onMouseLeave();
    void onMouseWheel(WPARAM wp, LPARAM lp);
    void paintPreeditCursor(HDC hdc, const RECT& preeditRc, int cursorX);
    void applyWindowShape();
    bool usesLayeredPresentation() const;
    void resizeForLayout(int width, int height);
    int scalePx(int value) const;
    // Monitor the popup is placed on, derived from the caret anchor; null until an anchor
    // has been pushed, in which case callers fall back to the owner window's monitor.
    HMONITOR anchorMonitor() const;
    bool updateDpiFromOwner(HWND owner);
    void refreshOwnedFonts();

private:
    BOOL shown_;
    int selKeyWidth_;
    int textWidth_;
    int commentWidth_;
    int itemHeight_;
    int candPerRow_;
    int candSpacing_;
    int colSpacing_;
    int rowSpacing_;
    int padX_;
    int padY_;
    int labelGap_;
    int cellGap_;
    int borderWidth_;
    int borderRadius_;
    int minWidth_;
    int preeditHeight_;
    int preeditGap_;
    int contentTop_;
    int rowPaddingY_;
    int rowInnerGap_;
    int pageNavWidth_;
    int candidatePanelWidth_;
    int candidatePanelHeight_;
    COLORREF backgroundColor_;
    COLORREF highlightColor_;
    COLORREF textColor_;
    COLORREF highlightTextColor_;
    COLORREF commentColor_;
    COLORREF commentHighlightColor_;
    std::wstring preedit_;
    int preeditCursor_;
    int preeditSelectionStart_;
    int preeditSelectionEnd_;
    HFONT commentFont_;
    std::vector<wchar_t> selKeys_;
    std::vector<CandidateUiItem> items_;
    std::vector<int> itemTextWidths_;
    std::vector<int> itemCommentWidths_;
    std::vector<int> itemWidths_;
    std::vector<int> itemHeights_;
    TypeDuck::DisplayPreferences displayPreferences_;
    int currentSel_;
    int pressedSel_;
    int pressedPageNavDirection_;
    int hoveredPageNavDirection_;
    int dpiX_;
    int dpiY_;
    HFONT ownedFont_;
    HFONT ownedCommentFont_;
    bool draggingWindow_;
    bool trackingMouse_;
    bool useCursor_;
    bool hasAnchorRect_;
    RECT anchorRect_;
};

} // namespace Moqi
