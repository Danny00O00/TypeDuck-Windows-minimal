#include "gtest/gtest.h"

#include "MoqLauncher/TypeDuckPreferences.h"

#include <json/json.h>

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

void writeFile(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << contents;
}

// Reads the file back as raw JSON so a test can assert on keys the Preferences
// struct does not model (and therefore cannot observe through loadPreferences).
bool parseJsonFile(const std::filesystem::path& path, Json::Value* root) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return false;
  }
  Json::CharReaderBuilder builder;
  std::string errors;
  return Json::parseFromStream(builder, stream, root, &errors);
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

TEST(TypeDuckPreferences, HiddenControlsSurviveSaveReloadAlongsideAsciiMode) {
  // The display-language, candidates-jyutping and full-input-code controls are
  // hidden from the settings UI, so nothing in the UI can rewrite them. They
  // must still round-trip through save+reload, and toggling asciiMode (the one
  // preference the Shift key writes behind the user's back) must not disturb
  // them. Every value below is deliberately NON-default, so a field that gets
  // dropped or reset would fall back to a different value and fail the compare.
  const auto root = makeTempDir("typeduck-hidden-controls-test");
  const auto path = root / "preferences.json";
  {
    std::ofstream stream(path, std::ios::binary);
    stream << R"({
  "displayLanguages": ["eng", "hin", "urd"],
  "mainLanguage": "hin",
  "pageSize": 6,
  "isHeiTypeface": false,
  "showRomanization": "reverse_only",
  "enableCompletion": true,
  "enableCorrection": false,
  "enableSentence": true,
  "enableLearning": true,
  "showReverseCode": false,
  "isCangjie5": true,
  "asciiMode": true
})";
  }

  const std::vector<std::string> expectedLanguages{"eng", "hin", "urd"};

  const auto first = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_TRUE(first.ok) << first.message;
  EXPECT_EQ(first.preferences.displayLanguages, expectedLanguages);
  EXPECT_EQ(first.preferences.mainLanguage, "hin");
  EXPECT_EQ(first.preferences.showRomanization, "reverse_only");
  EXPECT_FALSE(first.preferences.showReverseCode);
  EXPECT_TRUE(first.preferences.asciiMode);

  // Save the loaded preferences straight back, exactly as the asciiMode
  // persistence path does, then reload from disk.
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, first.preferences).ok);

  const auto second = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_TRUE(second.ok) << second.message;
  EXPECT_EQ(second.preferences.displayLanguages, expectedLanguages);
  EXPECT_EQ(second.preferences.mainLanguage, "hin");
  EXPECT_EQ(second.preferences.showRomanization, "reverse_only");
  EXPECT_FALSE(second.preferences.showReverseCode);
  EXPECT_TRUE(second.preferences.asciiMode);
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

TEST(TypeDuckPreferences, UnknownJsonKeysSurviveSaveLoadRoundTrip) {
  // savePreferences rewrites the whole file, and the Shift toggle now rewrites
  // it on every press. A key this build does not model -- one written by a newer
  // settings app, a hand edit, or a build the user later downgraded from -- must
  // survive that rewrite. Without the merge base it is erased on the very first
  // Shift press, which is when the user is least expecting to lose settings.
  const auto root = makeTempDir("typeduck-unknown-keys-test");
  const auto path = root / "preferences.json";
  writeFile(path, R"({
  "displayLanguages": ["eng"],
  "mainLanguage": "eng",
  "pageSize": 8,
  "isHeiTypeface": true,
  "showRomanization": "never",
  "enableCompletion": true,
  "enableCorrection": true,
  "enableSentence": true,
  "enableLearning": true,
  "showReverseCode": false,
  "isCangjie5": true,
  "asciiMode": false,
  "futureScalarSetting": "keep-me",
  "futureObjectSetting": {"nested": 42}
})");

  // Exactly what the Shift toggle does: load, flip asciiMode, save.
  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_TRUE(loaded.ok) << loaded.message;
  auto toggled = loaded.preferences;
  toggled.asciiMode = true;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, toggled).ok);

  Json::Value rewritten;
  ASSERT_TRUE(parseJsonFile(path, &rewritten));

  // The unmodelled keys are still there, values intact.
  EXPECT_EQ(rewritten["futureScalarSetting"].asString(), "keep-me");
  EXPECT_EQ(rewritten["futureObjectSetting"]["nested"].asInt(), 42);

  // ...and the modelled keys were still written correctly on top of them.
  EXPECT_TRUE(rewritten["asciiMode"].asBool());
  EXPECT_EQ(rewritten["pageSize"].asInt(), 8);
  EXPECT_TRUE(rewritten["isHeiTypeface"].asBool());
  EXPECT_EQ(rewritten["showRomanization"].asString(), "never");
  EXPECT_FALSE(rewritten["showReverseCode"].asBool());

  const auto reloaded = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_TRUE(reloaded.ok) << reloaded.message;
  EXPECT_TRUE(reloaded.preferences.asciiMode);
  EXPECT_EQ(reloaded.preferences.pageSize, 8);
  EXPECT_EQ(reloaded.preferences.showRomanization, "never");
  EXPECT_FALSE(reloaded.preferences.showReverseCode);
}

TEST(TypeDuckPreferences, CorruptJsonStillSavesFromDefaults) {
  // The merge base has to degrade safely. An unparseable file cannot be merged
  // onto, but it must not wedge the save either -- if it did, one bad byte would
  // jam the Shift toggle and the settings dialog forever. The save must still
  // succeed and self-heal the file back to a valid document.
  const auto root = makeTempDir("typeduck-corrupt-json-test");
  const auto path = root / "preferences.json";
  writeFile(path, "{ this is not json at all ]]");

  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_FALSE(loaded.ok);  // the corrupt file is reported...
  EXPECT_FALSE(loaded.message.empty());

  auto prefs = loaded.preferences;  // ...but usable defaults come back.
  prefs.asciiMode = true;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, prefs).ok);
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));

  const auto reloaded = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_TRUE(reloaded.ok) << reloaded.message;
  EXPECT_TRUE(reloaded.preferences.asciiMode);
  EXPECT_EQ(reloaded.preferences.pageSize,
            Moqi::TypeDuck::defaultPreferences().pageSize);

  // Same guarantee for a file that parses but is not a JSON object: the merge
  // base must reject it rather than let jsoncpp throw on subscripting an array.
  writeFile(path, "[1, 2, 3]");
  auto fromArray = Moqi::TypeDuck::defaultPreferences();
  fromArray.asciiMode = true;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, fromArray).ok);

  const auto afterArray = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_TRUE(afterArray.ok) << afterArray.message;
  EXPECT_TRUE(afterArray.preferences.asciiMode);
}
