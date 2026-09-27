#pragma once
#include <Epub.h>
#include <Epub/ReaderRenderSpec.h>
#include <GfxRenderer.h>

#include <memory>
#include <optional>
#include <string>

#include "KOReaderSyncClient.h"

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item
  int totalPages;                  // Total pages in the current spine item
  uint32_t visibleTextOffset = 0;  // Authoritative zero-based visible codepoint offset
  bool hasVisibleTextOffset = false;
  uint16_t paragraphIndex = 0;     // 1-based synthetic paragraph index from XPath p[N]
  bool hasParagraphIndex = false;  // True when paragraphIndex was resolved from XPath
  uint16_t liIndex = 0;            // Running <li> count at the matched XPath element
  bool hasLiIndex = false;         // True when target element is <li> and liIndex was resolved
  char xpathAnchorId[64] = {};     // First <a id> captured inside the matched XPath element
};

/**
 * Progress position representation.
 */
struct SavedProgressPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Maps between CrossPoint and SavedProgress position formats, such as those used by KOReader.
 *
 * CrossPoint tracks position as (spineIndex, visibleTextOffset). Page number is
 * derived from the current section layout.
 * SavedProgress uses XPath-like strings + percentage.
 *
 * The section cache records page-start visible offsets during pagination. The
 * same body-text counting rules are used to generate and resolve KOReader
 * XPaths. Percentage remains metadata and a fallback only.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to SavedProgress format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return SavedProgress position
   */
  static SavedProgressPosition toSavedProgress(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert SavedProgress position to CrossPoint format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param savedPos SavedProgress position
   * @param renderer GfxRenderer for page count estimation
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @param currentRenderSpec When given, a target chapter's on-disk section cache is only
   *        trusted for its page count/content-offset lookups if it was built under this
   *        exact render spec (font/margins/viewport/etc). A stale cache from before a
   *        settings change is otherwise indistinguishable from an up-to-date one and would
   *        answer confidently with a page count that no longer matches the current layout.
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const SavedProgressPosition& savedPos,
                                         GfxRenderer& renderer, int currentSpineIndex = -1,
                                         int totalPagesInCurrentSpine = 0, int fallbackTotalPages = 0,
                                         const ReaderRenderSpec* currentRenderSpec = nullptr);

  /**
   * Convert a rich CrossPoint position (downloaded from a crosspoint-sync
   * server) directly to a CrossPoint position. Its standard KOReader XPath is
   * resolved to a content offset first; legacy spine/page/paragraph hints are
   * used only when that content anchor cannot be applied.
   *
   * @param xpathAlreadyTried when true, skip re-resolving rich.xpath and go straight to the
   *        legacy page hints. The caller sets this when it just resolved the identical XPath via
   *        toCrossPoint(), so retrying it here would decompress the chapter twice for nothing.
   * @return The position, or std::nullopt when the rich position cannot be
   *         applied (spine out of range, no section cache) and the caller
   *         should fall back to toCrossPoint().
   */
  static std::optional<CrossPointPosition> fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                            const KOReaderRichPosition& rich, GfxRenderer& renderer,
                                                            bool xpathAlreadyTried = false,
                                                            const ReaderRenderSpec* currentRenderSpec = nullptr);

 private:
  /**
   * Generate a fallback XPath by streaming the spine item's XHTML and resolving
   * a paragraph/text position from intra-spine progress.
   * Produces a full ancestry path such as
   * /body/DocFragment[3]/body/p[42]/text().17.
   */
  static std::string generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);
};
