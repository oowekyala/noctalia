#pragma once

#include "config/config_types.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/wallpaper_polarity.h"
#include "render/core/wallpaper_types.h"
#include "render/scene/node.h"
#include "render/scene/wallpaper_node.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Box;

enum class WallpaperTransitionDirection {
  Forward,
  Reverse,
};

struct WallpaperInstance {
  std::uint32_t outputName = 0;
  struct wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::string connectorName;
  std::string description;

  std::unique_ptr<LayerSurface> surface;
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  Box* fillNode = nullptr;
  WallpaperNode* wallpaperNode = nullptr;

  // Wallpaper state
  std::string currentPath;
  std::string pendingPath;
  std::string queuedPath;
  WallpaperSourceKind currentSourceKind = WallpaperSourceKind::Image;
  WallpaperSourceKind nextSourceKind = WallpaperSourceKind::Image;
  Color currentColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  Color nextColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  SharedTextureCache::Lease currentImage;
  SharedTextureCache::Lease nextImage;
  // Image still decoding; it starts the change to loadingPath once its texture is uploaded.
  SharedTextureCache::Lease loadingImage;
  std::string loadingPath;
  // While the inverted version of a loaded image is rendered (loadingImage), the original.
  SharedTextureCache::Lease loadingCounterpart;
  bool loadingInverted = false;

  // wallpaper.match_theme_mode (wallpaper_polarity.h): each image's lightness analysis,
  // and whether it is shown inverted.
  std::optional<wallpaper_polarity::Analysis> currentAnalysis;
  std::optional<wallpaper_polarity::Analysis> nextAnalysis;
  bool currentInverted = false;
  bool nextInverted = false;
  // The shown image's other version (inverted or original), held only until its texture is
  // ready: to crossfade to it after a theme mode change, or, for an inverted version not
  // rendered yet, so that the palette of the other mode has it. Only the shown version
  // stays resident: the inverted one is rendered again from the original in milliseconds,
  // and the original is decoded again from its file.
  SharedTextureCache::Lease currentCounterpart;

  // Transition state
  float transitionTime = 0.0F;
  bool transitioning = false;
  WallpaperTransitionDirection transitionDirection = WallpaperTransitionDirection::Forward;
  AnimationManager::Id transitionAnimId = 0;
  WallpaperTransition activeTransition = WallpaperTransition::Fade;
  TransitionParams transitionParams;
};
