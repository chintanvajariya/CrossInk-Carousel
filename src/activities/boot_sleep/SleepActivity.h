#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  // Quick Resume sleep screen: leaves the reader page in the framebuffer,
  // draws a small filled circle in the lower-left as a "device asleep" hint,
  // and triggers a HALF_REFRESH so the panel keeps the page image. The
  // framebuffer is dumped to SD by main.cpp's enterDeepSleep after this
  // returns; on next boot it's restored before the splash to skip the boot
  // logo entirely.
  void renderQuickResumeSleepScreen() const;
  bool canSnapshotOverlayBackground = false;
  bool overlayPageBufferStored = false;
  bool overlayPageBufferTrusted = false;
  const bool fromTimeout = false;
};
