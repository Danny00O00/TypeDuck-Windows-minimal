#include "gtest/gtest.h"

#include "MoqLauncher/TypeDuckPreferences.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>

namespace {

std::filesystem::path makeTempDir(const char* name) {
  auto root = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  return root;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

} // namespace

TEST(TypeDuckPreferences, DefaultsMatchWebAlphaDecisionContract) {
  // D-17, D-20, D-21, D-42: Web DEFAULT_PREFERENCES in hooks.ts/consts.ts
  // are the source for native JSON defaults and Display Languages order.
  const auto prefs = Moqi::TypeDuck::defaultPreferences();
  EXPECT_EQ(prefs.displayLanguages, (std::vector<std::string>{"eng"}));
  EXPECT_EQ(prefs.mainLanguage, "eng");
  EXPECT_EQ(prefs.pageSize, 6);
  EXPECT_FALSE(prefs.isHeiTypeface);
  EXPECT_EQ(prefs.showRomanization, "always");
  EXPECT_TRUE(prefs.enableCompletion);
  EXPECT_FALSE(prefs.enableCorrection);
  EXPECT_TRUE(prefs.enableSentence);
  EXPECT_TRUE(prefs.enableLearning);
  EXPECT_TRUE(prefs.showReverseCode);
  EXPECT_TRUE(prefs.isCangjie5);
  EXPECT_FALSE(prefs.asciiMode);

  const auto descriptors = Moqi::TypeDuck::preferenceDescriptors();
  ASSERT_FALSE(descriptors.empty());
  EXPECT_EQ(descriptors.front().id, "displayLanguages");
  EXPECT_EQ(descriptors.front().label, "顯示語言 Display Languages");
}

TEST(TypeDuckPreferences, DefaultPathUsesRoamingAppData) {
  const auto roaming = makeTempDir("typeduck-roaming-appdata-test");
  const auto local = makeTempDir("typeduck-local-appdata-test");
  _putenv_s("APPDATA", roaming.string().c_str());
  _putenv_s("LOCALAPPDATA", local.string().c_str());

  EXPECT_EQ(Moqi::TypeDuck::defaultPreferencesPath(),
            roaming / "TypeDuckIME" / "TypeDuckPreferences.json");
}

TEST(TypeDuckPreferences, PageSizeRangeAndDefaultCustomPatch) {
  // D-45: pageSize is the only preference mapped to default.custom.yaml,
  // and it must customize the Rime menu/page_size path.
  auto prefs = Moqi::TypeDuck::defaultPreferences();
  auto invalid = prefs;
  invalid.pageSize = 3;
  auto result = Moqi::TypeDuck::validatePreferences(invalid);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.preferences.pageSize, 6);

  for (int size = 4; size <= 10; ++size) {
    prefs.pageSize = size;
    result = Moqi::TypeDuck::validatePreferences(prefs);
    ASSERT_TRUE(result.ok) << size;
    const auto sideEffects = Moqi::TypeDuck::rimeSideEffects(result.preferences);
    EXPECT_EQ(sideEffects.defaultCustomPath, "menu/page_size");
    EXPECT_EQ(sideEffects.pageSize, size);
  }
}

TEST(TypeDuckPreferences, InterfaceOnlyPreferencesStayJsonOnly) {
  // D-43: interface-only settings mirror TypeDuck Web types.ts and must not
  // trigger common.custom.yaml or default.custom.yaml customization.
  const auto prefs = Moqi::TypeDuck::defaultPreferences();
  const auto interfaceOnly =
      Moqi::TypeDuck::interfaceOnlyPreferenceIdsForTest();
  EXPECT_EQ(interfaceOnly,
            (std::vector<std::string>{"displayLanguages", "mainLanguage",
                                      "isHeiTypeface", "showRomanization",
                                      "showReverseCode"}));
  for (const auto& id : interfaceOnly) {
    EXPECT_FALSE(Moqi::TypeDuck::preferenceAffectsRime(id)) << id;
  }
}

TEST(TypeDuckPreferences, EnginePreferencesProduceCommonPatchList) {
  // D-46: worker.ts/api.cpp/common.yaml map engine preferences into
  // common.custom.yaml __patch entries with Cangjie roots always present.
  auto prefs = Moqi::TypeDuck::defaultPreferences();
  prefs.enableCompletion = false;
  prefs.enableCorrection = true;
  prefs.enableSentence = false;
  prefs.enableLearning = false;
  prefs.isCangjie5 = false;

  const auto sideEffects = Moqi::TypeDuck::rimeSideEffects(prefs);
  EXPECT_EQ(sideEffects.commonPatchKey, "__patch");
  EXPECT_EQ(sideEffects.commonPatches,
            (std::vector<std::string>{
                "common:/show_cangjie_roots",
                "common:/disable_completion",
                "common:/enable_correction",
                "common:/disable_sentence",
                "common:/disable_learning",
                "common:/use_cangjie3"}));
}

TEST(TypeDuckPreferences, AsciiModeJsonRoundTrip) {
  // asciiMode persists through save/load so the Shift-toggled input mode
  // survives launcher, backend, and machine restarts.
  const auto root = makeTempDir("typeduck-ascii-roundtrip-test");
  const auto path = root / "preferences.json";
  auto prefs = Moqi::TypeDuck::defaultPreferences();
  prefs.asciiMode = true;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, prefs).ok);

  EXPECT_NE(readFile(path).find("asciiMode"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));

  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_TRUE(loaded.ok);
  EXPECT_TRUE(loaded.preferences.asciiMode);
}

TEST(TypeDuckPreferences, LegacyJsonWithoutAsciiModeLoadsDefaultFalse) {
  // Settings files written before asciiMode existed must load safely with
  // Chinese mode as the default and every other field preserved.
  const auto root = makeTempDir("typeduck-ascii-legacy-test");
  const auto path = root / "preferences.json";
  {
    std::ofstream stream(path, std::ios::binary);
    stream << R"({
  "displayLanguages": ["eng"],
  "mainLanguage": "eng",
  "pageSize": 8,
  "isHeiTypeface": false,
  "showRomanization": "always",
  "enableCompletion": true,
  "enableCorrection": true,
  "enableSentence": true,
  "enableLearning": true,
  "showReverseCode": true,
  "isCangjie5": true
})";
  }

  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_TRUE(loaded.ok);
  EXPECT_FALSE(loaded.preferences.asciiMode);
  EXPECT_EQ(loaded.preferences.pageSize, 8);
  EXPECT_TRUE(loaded.preferences.enableCorrection);
}

TEST(TypeDuckPreferences, AsciiModeIsLiveEngineOptionWithNoRimePatches) {
  // asciiMode is a live engine option: it rides the backend settings push
  // but must never change yaml patch output or trigger redeploy.
  const auto defaults = Moqi::TypeDuck::defaultPreferences();
  auto toggled = defaults;
  toggled.asciiMode = true;

  const auto defaultEffects = Moqi::TypeDuck::rimeSideEffects(defaults);
  const auto toggledEffects = Moqi::TypeDuck::rimeSideEffects(toggled);
  EXPECT_EQ(toggledEffects.commonPatches, defaultEffects.commonPatches);
  EXPECT_EQ(toggledEffects.defaultCustomPath, defaultEffects.defaultCustomPath);
  EXPECT_EQ(toggledEffects.pageSize, defaultEffects.pageSize);
  EXPECT_FALSE(defaultEffects.asciiMode);
  EXPECT_TRUE(toggledEffects.asciiMode);

  EXPECT_FALSE(Moqi::TypeDuck::preferenceAffectsRime("asciiMode"));
  EXPECT_TRUE(Moqi::TypeDuck::preferenceIsLiveEngineOption("asciiMode"));
  const auto interfaceOnly =
      Moqi::TypeDuck::interfaceOnlyPreferenceIdsForTest();
  EXPECT_EQ(std::find(interfaceOnly.begin(), interfaceOnly.end(), "asciiMode"),
            interfaceOnly.end());
}

TEST(TypeDuckPreferences, FailedApplyDoesNotCorruptJsonSourceOfTruth) {
  // D-44 and D-47: apply is batched, deploy failures return bounded bilingual
  // status, and JSON remains the readable source of truth.
  const auto root = makeTempDir("typeduck-preferences-test");
  const auto path = root / "preferences.json";
  auto prefs = Moqi::TypeDuck::defaultPreferences();
  prefs.pageSize = 8;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, prefs).ok);

  const auto before = readFile(path);
  auto failed = Moqi::TypeDuck::applyPreferences(
      path, prefs,
      [](const Moqi::TypeDuck::RimeSideEffects&) {
        return Moqi::TypeDuck::ApplyResult{
            false,
            "設定未能套用，已保留原有設定 / Settings could not be applied; existing settings were kept"};
      });
  EXPECT_FALSE(failed.ok);
  EXPECT_NE(failed.message.find("設定"), std::string::npos);
  EXPECT_NE(failed.message.find("Settings"), std::string::npos);
  EXPECT_EQ(readFile(path), before);
}
