// sc5 - Soulcalibur V (Xbox 360), recompiled with ReXGlue.
//
// The app layer: where the game's files, its title update and the saves live, and the GPU plugin.
// Later: the hooks the launcher link and netplay need (as mk9-rollback/mk9/src/mk9_app.*).

#pragma once

#include <rex/rex_app.h>

#include "sc5/gameimage.h"

class Sc5App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Sc5App>(new Sc5App(ctx, "sc5", *sc5::GameImage()));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
};
