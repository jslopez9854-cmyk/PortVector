#include "HomeActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BookStats.h"
#include "ReadingStats.h"
#include "CrossPetSettings.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/crosspet/CrossPetTheme.h"
#include "components/icons/cover.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/tools.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

// ── CrossPet layout constants ────────────────────────────────────────────────
namespace {
constexpr int CP_HEADER_H       = 30;
constexpr int CP_CARD_MARGIN    = CrossPetMetrics::cardMargin;
constexpr int CP_CARD_Y         = 38;
constexpr int CP_CARD_H         = 345;
constexpr int CP_CARD_R         = CrossPetMetrics::cardRadius;
constexpr int CP_COVER_H        = 260;
constexpr int CP_PAD            = CrossPetMetrics::cardPadding;
constexpr int CP_RECENT_LABEL_Y = 280;
constexpr int CP_RECENT_COVER_Y = 302;
constexpr int CP_MAX_RECENT     = 3;
constexpr int CP_BOTTOM_BAR_H   = 80;
constexpr int CP_BOTTOM_ITEMS   = 4;
constexpr int CP_BOTTOM_ICON_SZ = 32;
constexpr int CP_BOTTOM_R       = 10;
constexpr int CP_FOCUS_COVER_PCT = 45;  // % of availH used for focus mode cover thumbnail
constexpr int CP_SHADOW         = CrossPetMetrics::shadowOffset;
}  // namespace

// ── Continue reading card ─────────────────────────────────────────────────────

void HomeActivity::renderContinueReadingCardInkbound() {
  const int screenW = renderer.getScreenWidth();
  const int cardX = CP_CARD_MARGIN;
  const int cardW = screenW - 2 * CP_CARD_MARGIN;

  // Fake drop shadow: DarkGray rect offset behind white card
  renderer.fillRoundedRect(cardX, CP_CARD_Y, cardW, CP_CARD_H, CP_CARD_R, Color::White);

  if (recentBooks.empty()) {
    // Book placeholder — centered icon + welcoming text
    constexpr int iconSz = 32;
    const int centerY = CP_CARD_Y + CP_CARD_H / 2;
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
    const int blockH = iconSz + 8 + lineH + 4 + smallH;
    const int startY = centerY - blockH / 2;

    // Book cover icon centered
    renderer.drawIcon(CoverIcon, cardX + (cardW - iconSz) / 2, startY, iconSz, iconSz);

    // "No recent books" bold
    const char* title = tr(STR_NO_RECENT_BOOKS);
    const int titleW = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, cardX + (cardW - titleW) / 2,
                      startY + iconSz + 8, title, true, EpdFontFamily::BOLD);

    // "Start reading below" subtitle
    const char* sub = tr(STR_START_READING);
    const int subW = renderer.getTextWidth(SMALL_FONT_ID, sub);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardW - subW) / 2,
                      startY + iconSz + 8 + lineH + 4, sub, true);
    return;
  }

  const RecentBook& book = recentBooks[0];
  const int coverW = CP_COVER_H * 0.7;
const int coverX = cardX + (cardW - coverW) / 2;
  const int coverY = CP_CARD_Y + CP_PAD;
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int medLineH = renderer.getLineHeight(UI_12_FONT_ID);

  // Cover image
  bool drewCover = false;
  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, CP_COVER_H);
    FsFile f;
    if (Storage.openFileForRead("HOME", thumbPath, f)) {
      Bitmap bmp(f);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bmp, coverX, coverY, coverW, CP_COVER_H);
        drewCover = true;
      }
      f.close();
    }
  }

  // Fallback placeholder if cover is not ready yet
  if (!drewCover) {
    renderer.drawRoundedRect(coverX, coverY, coverW, CP_COVER_H, 1, 8, true);

    constexpr int iconSz = 32;
    const int iconX = coverX + (coverW - iconSz) / 2;
    const int iconY = coverY + (CP_COVER_H - iconSz) / 2 - 10;
    renderer.drawIcon(CoverIcon, iconX, iconY, iconSz, iconSz);

    const char* loadingTxt = tr(STR_CONTINUE_READING);
    const int txtW = renderer.getTextWidth(SMALL_FONT_ID, loadingTxt);
    renderer.drawText(
      SMALL_FONT_ID,
      coverX + (coverW - txtW) / 2,
      iconY + iconSz + 10,
      loadingTxt,
      true
    );
  }
  // Right side: title, author, progress, per-book stats
int textY = coverY + CP_COVER_H + 10;

auto title = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), cardW - 20);
int titleW = renderer.getTextWidth(UI_12_FONT_ID, title.c_str());

renderer.drawText(UI_12_FONT_ID,
  cardX + (cardW - titleW) / 2,
  textY,
  title.c_str(),
  true
);

textY += medLineH + 10;

char pct[8];
snprintf(pct, sizeof(pct), "%d%%", book.progressPercent);

const int pctW = renderer.getTextWidth(SMALL_FONT_ID, pct);

// layout for bar + percent on same line
const int rightPad = 12;
const int gap = 8;
const int barH = 6;

const int totalRowW = (cardW * 0.6) + gap + pctW;
const int rowX = cardX + (cardW - totalRowW) / 2;

const int barW = cardW * 0.6;
const int barX = rowX;
const int barY = textY + (smallLineH - barH) / 2;

renderer.drawRoundedRect(barX, barY, barW, barH, 1, 3, true);

int fillW = barW * book.progressPercent / 100;
if (fillW > 4) {
  renderer.fillRoundedRect(barX + 1, barY + 1, fillW - 2, barH - 2, 2, Color::Black);
}

renderer.drawText(SMALL_FONT_ID,
  barX + barW + gap,
  textY,
  pct,
  true
);
}

// ── Recent cover thumbnails ───────────────────────────────────────────────────

void HomeActivity::renderRecentCoversInkbound() {
  return;
}
void HomeActivity::renderRecentSelectionInkbound() {
  return;
}
// ── Reading stats panel in gap between recent covers and bottom bar ───────────

void HomeActivity::renderReadingStatsBarInkbound() {
  return;
}
// ── Bottom navigation bar ─────────────────────────────────────────────────────

// Render bottom bar icons + labels (static, cached in cover buffer)
void HomeActivity::renderBottomGridInkbound() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

const int gridTop = CP_CARD_Y + CP_CARD_H + 32;
  const int gridBottom = screenH - BaseMetrics::values.buttonHintsHeight - 8;

  const int gridHeight = gridBottom - gridTop;
  const int cellW = screenW / 2;
const int rowH = gridHeight / 2;

  renderGridCell(0,      gridTop,        cellW, rowH, 0, ToolsIcon,     tr(STR_APPS));
  renderGridCell(cellW,  gridTop,        cellW, rowH, 1, LibraryIcon,   tr(STR_BROWSE_FILES));
  renderGridCell(0,      gridTop + rowH, cellW, rowH, 2, TransferIcon,  tr(STR_FILE_TRANSFER));
  renderGridCell(cellW,  gridTop + rowH, cellW, rowH, 3, Settings2Icon, tr(STR_SETTINGS_TITLE));
}
// Render bottom bar selection highlight only (dynamic, per-frame)
void HomeActivity::renderBottomBarSelectionInkbound() {
  // nothing needed — selection handled inside renderGridCell
}
void HomeActivity::renderSelectionHighlightInkbound() {
  if (selectorIndex != 0) return;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const bool focusMode = PET_SETTINGS.homeFocusMode;

  if (focusMode) {
    constexpr int btnH = 44;
    constexpr int btnR = 10;
    constexpr int btnMargin = 20;
    const int barY = screenH - BaseMetrics::values.buttonHintsHeight - CP_BOTTOM_BAR_H;
    const int cardW = screenW - 2 * CP_CARD_MARGIN;
    const int btnW = cardW - 2 * btnMargin;
    const int btnX = CP_CARD_MARGIN + btnMargin;
    const int btnY = barY - 12 - btnH;

    renderer.fillRoundedRect(btnX, btnY, btnW, btnH, btnR, Color::Black);

    char ctaBuf[64];
    snprintf(ctaBuf, sizeof(ctaBuf), "%s >", tr(STR_CONTINUE_READING));
    const int medLineH = renderer.getLineHeight(UI_12_FONT_ID);
    const int ctaW = renderer.getTextWidth(UI_12_FONT_ID, ctaBuf, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, btnX + (btnW - ctaW) / 2,
                      btnY + (btnH - medLineH) / 2, ctaBuf, false, EpdFontFamily::BOLD);
  } else {
const int insetBottom = 12;

renderer.drawRoundedRect(
  CP_CARD_MARGIN,
  CP_CARD_Y,
  screenW - 2 * CP_CARD_MARGIN,
  CP_CARD_H - insetBottom,
  1,
  CP_CARD_R,
  true
    );
  }
}

// ── CrossPet loop (card layout navigation) ────────────────────────────────────

void HomeActivity::loopCrossPetInkbound() {
  const bool focusMode = PET_SETTINGS.homeFocusMode;
const int barStart = 1;  // only Continue Reading above
const int itemCount = barStart + CP_BOTTOM_ITEMS;

  buttonNavigator.onNext([this, itemCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, itemCount);
    requestUpdate();
  });

  // Back long-press = sync
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= 800 && !syncTriggered) {
    syncTriggered = true;
    doSync();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) syncTriggered = false;

  if (syncResultMsg && millis() > syncResultExpiry) {
    syncResultMsg = nullptr;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
if (selectorIndex == 0) {
  if (!recentBooks.empty()) onSelectBook(recentBooks[0].path);
} else {
switch (selectorIndex - barStart) {
  case 0: onToolsOpen(); break;
  case 1: onFileBrowserOpen(); break;
  case 2: onFileTransferOpen(); break;
  case 3: onSettingsOpen(); break;
}
    }
  }
}

// ── Button hints (static, drawn once into buffer) ────────────────────────────

void HomeActivity::renderButtonHintsInkbound() {
  GUI.drawButtonHints(
    renderer,
    tr(STR_DIR_UP),
    tr(STR_DIR_DOWN),
    tr(STR_BACK),
    tr(STR_SELECT)
  );
}
// ── Focus mode: single large book card ────────────────────────────────────────

void HomeActivity::renderFocusCardInkbound() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int cardX = CP_CARD_MARGIN;
  const int cardW = screenW - 2 * CP_CARD_MARGIN;
  const int barY = screenH - BaseMetrics::values.buttonHintsHeight - CP_BOTTOM_BAR_H;
  const int availH = barY - CP_CARD_Y - 12;

  if (recentBooks.empty()) {
    // Empty state: plain card with centered placeholder (keep border for empty state only)
    renderer.drawRoundedRect(cardX, CP_CARD_Y, cardW, availH, 1, CP_CARD_R, true);
    constexpr int iconSz = 32;
    const int centerY = CP_CARD_Y + availH / 2;
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
    const int blockH = iconSz + 8 + lineH + 4 + smallH;
    const int startY = centerY - blockH / 2;
    renderer.drawIcon(CoverIcon, cardX + (cardW - iconSz) / 2, startY, iconSz, iconSz);
    const char* title = tr(STR_NO_RECENT_BOOKS);
    const int titleW = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, cardX + (cardW - titleW) / 2,
                      startY + iconSz + 8, title, true, EpdFontFamily::BOLD);
    const char* sub = tr(STR_START_READING);
    const int subW = renderer.getTextWidth(SMALL_FONT_ID, sub);
    renderer.drawText(SMALL_FONT_ID, cardX + (cardW - subW) / 2,
                      startY + iconSz + 8 + lineH + 4, sub, true);
    return;
  }

  const RecentBook& book = recentBooks[0];
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int medLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int innerW = cardW - 2 * CP_PAD;

  // CTA button dimensions (need to reserve space at bottom)
  constexpr int btnH = 44;
  constexpr int btnR = 10;
  constexpr int btnMargin = 20;
  const int btnW = cardW - 2 * btnMargin;
  const int btnX = cardX + btnMargin;
  const int btnY = barY - 12 - btnH;

  // Calculate content height to center vertically in available space above CTA
  const int coverH = availH * CP_FOCUS_COVER_PCT / 100;
  const int coverW = (int)(coverH * 0.7f);

  // Estimate total content height: cover + title + author + progress + info
  const int titleH = medLineH * 2;  // assume up to 2 lines
  const int authorH = book.author.empty() ? 0 : smallLineH;
  const int progressH = 6 + 6 + smallLineH;  // bar + gap + info text
  const int contentH = coverH + 12 + titleH + 2 + authorH + 12 + progressH;
  const int contentAreaH = btnY - CP_CARD_Y - 8;
  int y = CP_CARD_Y + std::max(0, (contentAreaH - contentH) / 2);

  // Cover centered horizontally
  const int coverX = (screenW - coverW) / 2;
  const int coverY = y;
  renderer.drawRoundedRect(coverX - 2, coverY - 2, coverW + 4, coverH + 4, 1, CP_CARD_R, true);

  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverH);
    FsFile f;
    if (Storage.openFileForRead("HOME_FOCUS", thumbPath, f)) {
      Bitmap bmp(f);
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        const int actualH = std::min((int)bmp.getHeight(), coverH);
        const int actualW = std::min((int)bmp.getWidth(), coverW);
        renderer.drawBitmap(bmp, coverX + (coverW - actualW) / 2, coverY, actualW, actualH);
      }
      f.close();
    }
  }
  y = coverY + coverH + 12;

  // Title (regular weight, up to 2 lines, centered)
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), innerW, 2);
  for (const auto& line : titleLines) {
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
    renderer.drawText(UI_12_FONT_ID, (screenW - tw) / 2, y, line.c_str(), true);
    y += medLineH;
  }
  y += 2;

  if (!book.author.empty()) {
    auto author = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), innerW);
    const int aw = renderer.getTextWidth(SMALL_FONT_ID, author.c_str());
    renderer.drawText(SMALL_FONT_ID, (screenW - aw) / 2, y, author.c_str(), true);
    y += smallLineH;
  }
  y += 12;

  // Progress bar with rounded caps
  constexpr int barMargin = 30;
  const int pBarW = cardW - 2 * barMargin;
  const int pBarX = cardX + barMargin;
  constexpr int pBarH = 6;
  constexpr int pBarR = 3;
  renderer.drawRoundedRect(pBarX, y, pBarW, pBarH, 1, pBarR, true);
  const int fillW = pBarW * book.progressPercent / 100;
  if (fillW > pBarR * 2) renderer.fillRoundedRect(pBarX + 1, y + 1, fillW - 2, pBarH - 2, pBarR - 1, Color::Black);
  y += pBarH + 6;

  // Info line: "42% · 2h 30m read · ~1h left"
  const auto* bs = BOOK_STATS.getBook(book.path.c_str());
  uint32_t bookMin = bs ? bs->totalSeconds / 60 : 0;
  char progressInfo[96];
  if (bookMin > 0) {
    char readBuf[24];
    if (bookMin >= 60)
      snprintf(readBuf, sizeof(readBuf), "%dh %dm", (int)(bookMin / 60), (int)(bookMin % 60));
    else
      snprintf(readBuf, sizeof(readBuf), "%dm", (int)bookMin);
    if (book.progressPercent > 0) {
      uint32_t estMin = bookMin * (100 - book.progressPercent) / book.progressPercent;
      char estBuf[24];
      if (estMin >= 60)
        snprintf(estBuf, sizeof(estBuf), "~%dh %dm", (int)(estMin / 60), (int)(estMin % 60));
      else
        snprintf(estBuf, sizeof(estBuf), "~%dm", (int)estMin);
      snprintf(progressInfo, sizeof(progressInfo), "%d%% \xC2\xB7 %s read \xC2\xB7 %s left",
               book.progressPercent, readBuf, estBuf);
    } else {
      snprintf(progressInfo, sizeof(progressInfo), "%d%% \xC2\xB7 %s read", book.progressPercent, readBuf);
    }
  } else {
    snprintf(progressInfo, sizeof(progressInfo), "%d%%", book.progressPercent);
  }
  const int piW = renderer.getTextWidth(SMALL_FONT_ID, progressInfo);
  renderer.drawText(SMALL_FONT_ID, (screenW - piW) / 2, y, progressInfo, true);

  // CTA button — outlined by default (filled on selection via renderSelectionHighlight)
  renderer.drawRoundedRect(btnX, btnY, btnW, btnH, 1, btnR, true);
  char ctaBuf[64];
  snprintf(ctaBuf, sizeof(ctaBuf), "%s >", tr(STR_CONTINUE_READING));
  const int ctaW = renderer.getTextWidth(UI_12_FONT_ID, ctaBuf);
  const int ctaX = btnX + (btnW - ctaW) / 2;
  const int ctaY = btnY + (btnH - medLineH) / 2;
  renderer.drawText(UI_12_FONT_ID, ctaX, ctaY, ctaBuf, true);
}

// ── CrossPet main render ──────────────────────────────────────────────────────

void HomeActivity::renderCrossPetInkbound() {
  const int screenW = renderer.getScreenWidth();
  const bool focusMode = PET_SETTINGS.homeFocusMode;

  if (!coverRendered) {
    // First render: build full base buffer (covers + static UI elements)
    renderer.clearScreen();
if (focusMode) {
  renderFocusCardInkbound();
} else {
  renderContinueReadingCardInkbound();
  renderRecentCoversInkbound();
  renderReadingStatsBarInkbound();
}
renderButtonHintsInkbound();
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  } else {
    // Fast path: restore cached buffer (covers + static elements)
    restoreCoverBuffer();
  }

  // Dynamic elements (redrawn each frame — lightweight)
  GUI.drawHeader(renderer, Rect{0, 0, screenW, CP_HEADER_H}, nullptr);
  renderHeaderClock();
  renderPetStatusWidget(CP_HEADER_H);

  // Selection highlights only (not full bottom bar redraw)
renderSelectionHighlightInkbound();
if (!focusMode) renderRecentSelectionInkbound();
if (!focusMode) {
  renderBottomGridInkbound();
}
  renderer.displayBuffer();

  // Post-render: trigger cover thumbnail loading
  if (!firstRenderDone) {
    firstRenderDone = true;
    // Focus mode uses taller cover — compute from screen layout
    int loadCoverH = CP_COVER_H;
    if (focusMode) {
      const int sh = renderer.getScreenHeight();
      const int availH = sh - BaseMetrics::values.buttonHintsHeight - CP_BOTTOM_BAR_H - CP_CARD_Y - 12;
      loadCoverH = availH * CP_FOCUS_COVER_PCT / 100;
    }
    bool needsLoad = false;
    for (const auto& b : recentBooks) {
      if (!b.coverBmpPath.empty() &&
          !Storage.exists(UITheme::getCoverThumbPath(b.coverBmpPath, loadCoverH).c_str())) {
        needsLoad = true; break;
      }
    }
    if (needsLoad) requestUpdate();
    else recentsLoaded = true;
  } else if (!recentsLoaded && !recentsLoading) {
    const int sh = renderer.getScreenHeight();
    const int availH = sh - BaseMetrics::values.buttonHintsHeight - CP_BOTTOM_BAR_H - CP_CARD_Y - 12;
    const int loadH = focusMode ? availH * CP_FOCUS_COVER_PCT / 100 : CP_COVER_H;

    recentsLoading = true;
    loadRecentCovers(loadH);

  }
}
