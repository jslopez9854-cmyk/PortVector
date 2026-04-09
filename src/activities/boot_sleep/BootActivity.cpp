#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/PortVectorLogo176.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int logoSize = 176;

  // Slightly above center
  const int logoY = (pageHeight - logoSize) / 2 - 10;

renderer.drawImage(PortVectorLogo176,
                   (pageWidth - logoSize) / 2,
                   logoY,
                   logoSize,
                   logoSize);
   // Title with better spacing
  renderer.drawCenteredText(UI_10_FONT_ID, logoY + logoSize + 20,
                            "PortVector", true, EpdFontFamily::BOLD);

  // Version (minimal)
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 25,
                            "v1.0");

  renderer.displayBuffer();
}