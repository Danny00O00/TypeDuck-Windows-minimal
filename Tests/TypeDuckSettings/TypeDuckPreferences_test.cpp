#include "gtest/gtest.h"

#include "MoqLauncher/TypeDuckPreferences.h"

#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
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

// The nesting depth at which jsoncpp's reader gives up, ASKED OF THE LIBRARY
// rather than assumed. CharReaderBuilder's documented default is 1000, but it is
// a build-time constant: the jsoncpp these tests linked against on the dev machine
// reports 256. Hard-coding either number would leave the test asserting nothing on
// the build that used the other one, so we read it from the same defaults the
// production code's CharReaderBuilder is constructed with.
int jsoncppStackLimit() {
  Json::Value settings;
  Json::CharReaderBuilder::setDefaults(&settings);
  return settings["stackLimit"].isIntegral() ? settings["stackLimit"].asInt() : 1000;
}

// A JSON value nested `depth` objects deep: {"a":{"a": ... 1 ... }}. Perfectly
// VALID JSON at any depth -- that is the entire point of these tests.
std::string deeplyNestedJsonValue(int depth) {
  std::string value;
  for (int i = 0; i < depth; ++i) {
    value += "{\"a\":";
  }
  value += "1";
  for (int i = 0; i < depth; ++i) {
    value += "}";
  }
  return value;
}

// Does this jsoncpp actually THROW on these bytes (as opposed to returning
// failure)? The fix's whole premise, checked against the library in front of us
// instead of taken on faith: a build with no stack limit would parse the deep
// document happily, and the tests below would then be asserting a hazard that does
// not exist on it. They skip instead of failing spuriously.
bool jsoncppThrowsOnParse(const std::string& document) {
  std::istringstream stream(document);
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  try {
    Json::parseFromStream(builder, stream, &root, &errors);
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

// Parsing with the limit lifted, to prove the document the production code refused
// to judge is in fact VALID and complete -- i.e. that "Undetermined" really does
// mean "we could not tell", not "it was garbage after all".
bool parseWithRaisedStackLimit(const std::string& document, Json::Value* root) {
  std::istringstream stream(document);
  Json::CharReaderBuilder builder;
  builder.settings_["stackLimit"] = jsoncppStackLimit() * 4 + 1000;
  std::string errors;
  try {
    return Json::parseFromStream(builder, stream, root, &errors);
  } catch (const std::exception&) {
    return false;
  }
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

TEST(TypeDuckPreferences, UnreadableFileIsReportedUnreadableNotAsACleanDefaultLoad) {
  // A load that could not read the file hands back defaults, and defaults look
  // exactly like stored values. The ascii-mode persistence path compares the
  // loaded value against the value it is about to write and skips the save when
  // they match, so a load that cannot read the file MUST say so: otherwise disk
  // holds true, the file goes unreadable, the user toggles to false, the
  // invented default false "matches", the save/retry/warning are all skipped,
  // and the next restart restores true -- the opposite of what the user set.
  const auto root = makeTempDir("typeduck-unreadable-load-test");

  // A file whose read permission has been revoked. POSIX honours that; MSVC's
  // std::filesystem only models the read-only attribute and a root test runner
  // ignores the mode entirely, so this half only asserts where it truly applies.
  const auto denied = root / "denied.json";
  writeFile(denied, R"({"pageSize": 8, "asciiMode": true})");
  std::error_code ec;
  std::filesystem::permissions(denied, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  bool deniedIsStillOpenable = true;
  {
    std::ifstream probe(denied, std::ios::binary);
    deniedIsStillOpenable = probe.is_open();
  }
  if (!deniedIsStillOpenable) {
    const auto loaded = Moqi::TypeDuck::loadPreferences(denied);
    // The bytes are there and we cannot see them: IoError, never Corrupt.
    EXPECT_EQ(loaded.source, Moqi::TypeDuck::PreferencesSource::IoError);
    EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(loaded));
    EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded));
    EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(loaded));
    EXPECT_FALSE(loaded.ok);
    EXPECT_FALSE(loaded.message.empty());
    // The stored asciiMode was true; the defaults we fell back to say false, and
    // nothing may mistake that false for the value on disk.
    EXPECT_FALSE(loaded.preferences.asciiMode);
    EXPECT_EQ(loaded.preferences.pageSize,
              Moqi::TypeDuck::defaultPreferences().pageSize);
  }
  std::filesystem::permissions(denied, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);

  // A path that exists but cannot be read as a preferences document. Portable on
  // purpose: Windows refuses to open a directory as a file, POSIX opens it and
  // then fails the read. Either way the verdict must be "unreadable", never
  // "absent" and never a clean load.
  const auto directoryPath = root / "directory.json";
  std::filesystem::create_directories(directoryPath);
  const auto loadedDirectory = Moqi::TypeDuck::loadPreferences(directoryPath);
  EXPECT_EQ(loadedDirectory.source, Moqi::TypeDuck::PreferencesSource::IoError);
  EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(loadedDirectory));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(loadedDirectory));
  EXPECT_FALSE(loadedDirectory.ok);
  EXPECT_FALSE(loadedDirectory.message.empty());

  // And a file that opens but is not JSON at all. This one is CORRUPT, not
  // IoError: the bytes were read, there is simply nothing parseable in them. The
  // difference is not cosmetic -- it is what decides whether the file may be
  // rewritten. Nothing can be lost by self-healing this one.
  const auto garbage = root / "garbage.json";
  writeFile(garbage, std::string("\x01\x02 not json {{{", 15));
  const auto loadedGarbage = Moqi::TypeDuck::loadPreferences(garbage);
  EXPECT_EQ(loadedGarbage.source, Moqi::TypeDuck::PreferencesSource::Corrupt);
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(loadedGarbage));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loadedGarbage));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(loadedGarbage));
  EXPECT_FALSE(loadedGarbage.ok);

  // A readable, valid file is the control: this one really was read from disk.
  const auto good = root / "good.json";
  auto prefs = Moqi::TypeDuck::defaultPreferences();
  prefs.asciiMode = true;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(good, prefs).ok);
  const auto loadedGood = Moqi::TypeDuck::loadPreferences(good);
  EXPECT_EQ(loadedGood.source, Moqi::TypeDuck::PreferencesSource::File);
  EXPECT_TRUE(Moqi::TypeDuck::preferencesWereReadFromFile(loadedGood));
  EXPECT_TRUE(Moqi::TypeDuck::preferencesAreAuthoritative(loadedGood));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(loadedGood));
  EXPECT_TRUE(loadedGood.ok);
  EXPECT_TRUE(loadedGood.preferences.asciiMode);
}

TEST(TypeDuckPreferences, PathologicallyNestedJsonDegradesInsteadOfThrowing) {
  // jsoncpp THROWS (Json::RuntimeError, "Exceeded stackLimit in readValue()") once
  // a document nests past its reader stack limit, which a file reaches with a few
  // kilobytes of brackets. loadPreferences runs on the launcher's libuv loop thread
  // from handleBackendReply(), ahead of forwarding the reply to every client, so an
  // exception thrown out of it would take the reply path (and the launcher) with
  // it. It must degrade instead of unwinding.
  //
  // THIS TEST USED TO ASSERT THE BUG. It required the deep document to SELF-HEAL
  // (EXPECT_TRUE(saved.ok)) -- i.e. to be rewritten from defaults -- on the theory
  // that an exception proves the file is garbage. It does not. A throw is the
  // parser declining to judge, and the very same throw comes back for a perfectly
  // VALID document (see DeeplyNestedUnknownKeyIsUndeterminedAndSurvivesTheSave).
  // The parser cannot tell us which of the two we are holding, so neither can we,
  // and a file we cannot judge must not be overwritten. The correct, stronger
  // property is the opposite of what this test used to demand: the save REFUSES and
  // the bytes on disk are left exactly as they were.
  const auto root = makeTempDir("typeduck-nested-json-test");
  const auto path = root / "preferences.json";
  constexpr int kNestingDepth = 2000;
  const std::string original =
      std::string(kNestingDepth, '[') + std::string(kNestingDepth, ']');
  writeFile(path, original);

  if (!jsoncppThrowsOnParse(original)) {
    GTEST_SKIP() << "this jsoncpp does not throw at depth " << kNestingDepth
                 << " (stackLimit=" << jsoncppStackLimit()
                 << "); there is no undetermined-parse hazard to assert";
  }

  Moqi::TypeDuck::ValidationResult loaded;
  ASSERT_NO_THROW(loaded = Moqi::TypeDuck::loadPreferences(path));
  // The bytes came off the disk fine; it is the PARSE that blew up. That is neither
  // IoError (we DID read the bytes) nor Corrupt (the parser never actually rejected
  // them -- it gave up). It is Undetermined, and Undetermined does not authorise a
  // write.
  EXPECT_EQ(loaded.source, Moqi::TypeDuck::PreferencesSource::Undetermined);
  EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(loaded));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(loaded));
  EXPECT_FALSE(loaded.ok);
  EXPECT_FALSE(loaded.message.empty());
  // Usable defaults still come back, so no caller is left holding a broken struct.
  EXPECT_EQ(loaded.preferences.pageSize,
            Moqi::TypeDuck::defaultPreferences().pageSize);

  // The save path parses the same document as its merge base, so it must not throw
  // either -- and it must REFUSE, because merging onto the empty base it fell back
  // to would rename a defaults document over bytes nobody has understood.
  auto toggled = loaded.preferences;
  toggled.asciiMode = true;  // exactly what one Shift press tries to persist
  Moqi::TypeDuck::SaveResult saved;
  ASSERT_NO_THROW(saved = Moqi::TypeDuck::savePreferences(path, toggled));
  EXPECT_FALSE(saved.ok);
  EXPECT_FALSE(saved.message.empty());
  EXPECT_NE(saved.message.find("設定"), std::string::npos);
  EXPECT_NE(saved.message.find("settings"), std::string::npos);

  // The whole point: THE ORIGINAL BYTES ARE STILL THERE, byte for byte...
  EXPECT_EQ(readFile(path), original);
  // ...and refusing meant touching nothing at all, so no temp file was left behind.
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));

  // The verdict is stable: reloading says Undetermined again, not "now it is fine".
  EXPECT_EQ(Moqi::TypeDuck::loadPreferences(path).source,
            Moqi::TypeDuck::PreferencesSource::Undetermined);
}

TEST(TypeDuckPreferences, DeeplyNestedUnknownKeyIsUndeterminedAndSurvivesTheSave) {
  // THE USER-VISIBLE BUG, and the reason a thrown parse may never be filed as
  // Corrupt. This file is not hostile and it is not damaged: it is a completely
  // VALID JSON object holding every modelled preference the user has set, plus one
  // unknown key -- written by a newer build, a hand edit, or a settings app this
  // binary predates -- whose value happens to nest past jsoncpp's reader stack
  // limit. jsoncpp throws on it exactly as it throws on a wall of brackets.
  //
  // Before the fix: the throw was filed as Corrupt, Corrupt is writable by design,
  // and so the next Shift press merged asciiMode onto an EMPTY object and renamed
  // that over the file -- resetting pageSize to 6, resetting everything else to
  // defaults, and DELETING the deep key outright. The file was never garbage. We
  // just could not read it, and said "garbage" instead of "I do not know".
  const auto root = makeTempDir("typeduck-deep-unknown-key-test");
  const auto path = root / "preferences.json";

  // Find the real limit rather than guessing it: ask the library (it is a build-time
  // constant -- nominally 1000, but 256 on the jsoncpp these tests linked against),
  // then nest a little past it.
  const int stackLimit = jsoncppStackLimit();
  ASSERT_GT(stackLimit, 0);
  if (stackLimit > 20000) {
    GTEST_SKIP() << "stackLimit " << stackLimit
                 << " is too deep to provoke without risking the real C stack";
  }
  const int depth = stackLimit + 8;

  // A valid settings document: the normal modelled keys, all set to NON-default
  // values so that a rewrite-from-defaults would be unmistakable, plus the deep
  // unknown key.
  const std::string original =
      "{\n"
      "  \"displayLanguages\": [\"eng\", \"hin\"],\n"
      "  \"mainLanguage\": \"hin\",\n"
      "  \"pageSize\": 9,\n"
      "  \"isHeiTypeface\": true,\n"
      "  \"showRomanization\": \"never\",\n"
      "  \"enableCompletion\": false,\n"
      "  \"enableCorrection\": true,\n"
      "  \"enableSentence\": false,\n"
      "  \"enableLearning\": false,\n"
      "  \"showReverseCode\": false,\n"
      "  \"isCangjie5\": false,\n"
      "  \"asciiMode\": false,\n"
      "  \"deeplyNestedFutureSetting\": " +
      deeplyNestedJsonValue(depth) +
      "\n"
      "}";
  writeFile(path, original);

  // Establish the premise against the library in front of us, not from memory:
  // (a) the production parser THROWS on this document...
  if (!jsoncppThrowsOnParse(original)) {
    GTEST_SKIP() << "this jsoncpp parses depth " << depth
                 << " without throwing (stackLimit=" << stackLimit
                 << "); the hazard does not exist on this build";
  }
  // (b) ...and yet the document is perfectly VALID and complete -- lift the limit
  // and every key is right there. "The parser threw" is emphatically NOT "the file
  // is garbage", and that is the entire distinction between Undetermined and
  // Corrupt.
  Json::Value proof;
  ASSERT_TRUE(parseWithRaisedStackLimit(original, &proof));
  ASSERT_TRUE(proof.isObject());
  EXPECT_EQ(proof["pageSize"].asInt(), 9);
  EXPECT_TRUE(proof.isMember("deeplyNestedFutureSetting"));

  // The load must say "I do not know", not "it is garbage".
  Moqi::TypeDuck::ValidationResult loaded;
  ASSERT_NO_THROW(loaded = Moqi::TypeDuck::loadPreferences(path));
  EXPECT_EQ(loaded.source, Moqi::TypeDuck::PreferencesSource::Undetermined);
  EXPECT_NE(loaded.source, Moqi::TypeDuck::PreferencesSource::Corrupt);
  EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(loaded));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(loaded));
  EXPECT_FALSE(loaded.ok);
  EXPECT_FALSE(loaded.message.empty());

  // One Shift press. This is the keystroke that used to destroy the file.
  auto toggled = loaded.preferences;
  toggled.asciiMode = true;
  Moqi::TypeDuck::SaveResult saved;
  ASSERT_NO_THROW(saved = Moqi::TypeDuck::savePreferences(path, toggled));
  EXPECT_FALSE(saved.ok);
  EXPECT_FALSE(saved.message.empty());
  // Bilingual, and truthful about WHICH failure this is: the file is intact and
  // unreadable-to-us, not locked or on a broken disk. It must therefore not be the
  // "could not be read" message, which would send the user hunting for an antivirus
  // lock that does not exist.
  EXPECT_NE(saved.message.find("設定檔"), std::string::npos);
  EXPECT_NE(saved.message.find("settings file"), std::string::npos);
  EXPECT_NE(saved.message.find("未能解析"), std::string::npos);
  EXPECT_EQ(saved.message.find("未能讀取"), std::string::npos);

  // THE ASSERTION THE WHOLE FIX EXISTS FOR: the file on disk is byte-for-byte what
  // the user had. Not merged, not defaulted, not truncated -- untouched.
  EXPECT_EQ(readFile(path), original);
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));
  EXPECT_TRUE(std::filesystem::exists(path));

  // And spelled out in terms of what the user would actually notice, in case a
  // future refactor makes the byte comparison pass for the wrong reason: their
  // settings and their unknown key are all still in the file.
  Json::Value after;
  ASSERT_TRUE(parseWithRaisedStackLimit(readFile(path), &after));
  ASSERT_TRUE(after.isObject());
  EXPECT_EQ(after["pageSize"].asInt(), 9);                 // not reset to 6
  EXPECT_EQ(after["showRomanization"].asString(), "never");  // not reset to "always"
  EXPECT_FALSE(after["isCangjie5"].asBool());
  EXPECT_TRUE(after.isMember("deeplyNestedFutureSetting"));  // not deleted
  // The toggle itself was NOT persisted, and that is the deal we accepted: losing
  // the toggled input mode across a restart is survivable, destroying the settings
  // file is not.
  EXPECT_FALSE(after["asciiMode"].asBool());
}

TEST(TypeDuckPreferences, AbsentFileStaysTheQuietFirstRunDefaultPath) {
  // The first-run path must keep working exactly as before: no file yet is not
  // an error, must not warn the user, must not be confused with an unreadable
  // file, and must not create anything on disk just by being read.
  const auto root = makeTempDir("typeduck-absent-load-test");
  const auto path = root / "preferences.json";
  ASSERT_FALSE(std::filesystem::exists(path));

  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_TRUE(loaded.ok);
  EXPECT_TRUE(loaded.message.empty());
  EXPECT_EQ(loaded.source, Moqi::TypeDuck::PreferencesSource::Absent);
  EXPECT_NE(loaded.source, Moqi::TypeDuck::PreferencesSource::IoError);
  EXPECT_NE(loaded.source, Moqi::TypeDuck::PreferencesSource::Corrupt);
  // Absent means "defaults ARE the state on disk", but they still were not read
  // out of a file, and callers that need proof of the stored value must not get
  // it here.
  EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(loaded));
  // It IS authoritative, though -- there is no file, so the defaults really are
  // the stored state. And an absent file blocks nothing: first run must write.
  EXPECT_TRUE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(loaded));

  const auto defaults = Moqi::TypeDuck::defaultPreferences();
  EXPECT_EQ(loaded.preferences.displayLanguages, defaults.displayLanguages);
  EXPECT_EQ(loaded.preferences.pageSize, defaults.pageSize);
  EXPECT_EQ(loaded.preferences.showRomanization, defaults.showRomanization);
  EXPECT_FALSE(loaded.preferences.asciiMode);
  EXPECT_FALSE(std::filesystem::exists(path));

  // A file under a directory that does not exist yet is just as absent.
  const auto missing = Moqi::TypeDuck::loadPreferences(
      root / "no-such-directory" / "preferences.json");
  EXPECT_TRUE(missing.ok);
  EXPECT_TRUE(missing.message.empty());
  EXPECT_EQ(missing.source, Moqi::TypeDuck::PreferencesSource::Absent);
  EXPECT_FALSE(Moqi::TypeDuck::preferencesWereReadFromFile(missing));
}

TEST(TypeDuckPreferences, SourcePredicatesSplitReadFailureFromCorruption) {
  // The contract every writer codes against. Authoritative == "these values are
  // really what is on disk" (File, and Absent because no file means the defaults
  // ARE the state). BlockWrites == the verdicts under which THE CONTENTS ARE
  // UNKNOWN, and there are TWO of them:
  //   IoError      -- we never saw the bytes.
  //   Undetermined -- we saw them and the parser THREW instead of judging them, so
  //                   the document may be entirely valid and full of settings.
  // Corrupt is the one failure we are CERTAIN about -- the parser looked at the
  // bytes and rejected them -- so it stays writable and the save self-heals it.
  // Blocking Corrupt would wedge the Shift toggle forever on a single bad byte;
  // allowing Undetermined destroys a valid file. The split is the whole design.
  const auto sourceOf = [](Moqi::TypeDuck::PreferencesSource source) {
    Moqi::TypeDuck::ValidationResult result;
    result.source = source;
    return result;
  };
  using Source = Moqi::TypeDuck::PreferencesSource;

  EXPECT_TRUE(Moqi::TypeDuck::preferencesAreAuthoritative(sourceOf(Source::File)));
  EXPECT_TRUE(Moqi::TypeDuck::preferencesAreAuthoritative(sourceOf(Source::Absent)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(sourceOf(Source::IoError)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(sourceOf(Source::Corrupt)));
  EXPECT_FALSE(
      Moqi::TypeDuck::preferencesAreAuthoritative(sourceOf(Source::Undetermined)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(sourceOf(Source::NotLoaded)));

  EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(sourceOf(Source::IoError)));
  EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(sourceOf(Source::Undetermined)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(sourceOf(Source::Corrupt)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(sourceOf(Source::File)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(sourceOf(Source::Absent)));
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(sourceOf(Source::NotLoaded)));

  // Undetermined is NOT read-from-file: the values handed back with it are invented
  // defaults, and nothing may mistake them for the stored state.
  EXPECT_FALSE(
      Moqi::TypeDuck::preferencesWereReadFromFile(sourceOf(Source::Undetermined)));

  // The default-constructed result never claims to know anything about the disk.
  EXPECT_EQ(Moqi::TypeDuck::ValidationResult{}.source, Source::NotLoaded);
}

TEST(TypeDuckPreferences, UnreadableFileIsNeverOverwrittenBySave) {
  // THE REGRESSION THIS FIXES. A settings file we could not READ must never be
  // WRITTEN. savePreferences builds its merge base by re-reading the file, so if
  // an unreadable base were allowed through, the merge would happen onto an EMPTY
  // object and the rename would drop that over the user's file -- rewriting it
  // from defaults and destroying every other setting and every unknown key in it.
  // The Shift toggle calls savePreferences on every press, so this lands on one
  // keystroke. The save must fail, and the bytes on disk must be untouched.
  const auto root = makeTempDir("typeduck-unreadable-save-test");

  // Half 1 -- deterministic on BOTH platforms: a directory where the file belongs.
  // POSIX opens a directory as a file and fails the read; Windows refuses to open
  // it at all. Either way it is IoError, and the save must decline. The sentinel
  // inside proves nothing was clobbered, since replacing the path would take the
  // whole directory (and the sentinel) with it.
  const auto directoryPath = root / "directory.json";
  std::filesystem::create_directories(directoryPath);
  writeFile(directoryPath / "sentinel.txt", "do-not-destroy-me");

  const auto loadedDirectory = Moqi::TypeDuck::loadPreferences(directoryPath);
  EXPECT_EQ(loadedDirectory.source, Moqi::TypeDuck::PreferencesSource::IoError);
  EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(loadedDirectory));

  auto toggled = Moqi::TypeDuck::defaultPreferences();
  toggled.asciiMode = true;  // exactly what one Shift press tries to persist
  const auto savedOverDirectory =
      Moqi::TypeDuck::savePreferences(directoryPath, toggled);
  EXPECT_FALSE(savedOverDirectory.ok);
  EXPECT_FALSE(savedOverDirectory.message.empty());
  EXPECT_NE(savedOverDirectory.message.find("設定"), std::string::npos);
  EXPECT_NE(savedOverDirectory.message.find("Settings"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_directory(directoryPath));
  EXPECT_EQ(readFile(directoryPath / "sentinel.txt"), "do-not-destroy-me");
  // Refusing means touching NOTHING: no temp file may be left lying around.
  EXPECT_FALSE(std::filesystem::exists(root / "directory.json.tmp"));

  // Half 2 -- the real-world shape of the bug, and the half that actually proves
  // the user's bytes survive: an unreadable FILE inside a still-WRITABLE
  // directory (an ACL on the file alone, an antivirus lock). Here the rename in
  // savePreferences would genuinely succeed, so nothing but the IoError check
  // stands between the user and a settings file rewritten from defaults.
  //
  // Gated on an actual probe, not on an assumption: POSIX honours chmod 000, but
  // MSVC's std::filesystem does not model it and root ignores it entirely. Where
  // the file stays readable there is no read failure to test, so we skip.
  const auto denied = root / "denied.json";
  const std::string original =
      "{\n  \"pageSize\": 9,\n  \"asciiMode\": false,\n"
      "  \"futureScalarSetting\": \"keep-me\"\n}";
  writeFile(denied, original);
  std::error_code ec;
  std::filesystem::permissions(denied, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  bool deniedIsStillOpenable = true;
  {
    std::ifstream probe(denied, std::ios::binary);
    deniedIsStillOpenable = probe.is_open();
  }

  if (!deniedIsStillOpenable) {
    // The directory is untouched, so a write here WOULD land. Only the contract
    // stops it.
    EXPECT_TRUE(std::filesystem::exists(root));

    const auto loadedDenied = Moqi::TypeDuck::loadPreferences(denied);
    EXPECT_EQ(loadedDenied.source, Moqi::TypeDuck::PreferencesSource::IoError);
    EXPECT_TRUE(Moqi::TypeDuck::preferencesBlockWrites(loadedDenied));
    EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loadedDenied));

    const auto savedOverDenied = Moqi::TypeDuck::savePreferences(denied, toggled);
    EXPECT_FALSE(savedOverDenied.ok);
    EXPECT_FALSE(savedOverDenied.message.empty());
    EXPECT_NE(savedOverDenied.message.find("設定"), std::string::npos);
    EXPECT_NE(savedOverDenied.message.find("Settings"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "denied.json.tmp"));

    // Give the read permission back and look: the ORIGINAL BYTES are still there,
    // pageSize 9 and the unmodelled key included. Before the fix this file came
    // back as a defaults document with pageSize 6 and futureScalarSetting gone.
    std::filesystem::permissions(denied, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    EXPECT_EQ(readFile(denied), original);

    const auto reloaded = Moqi::TypeDuck::loadPreferences(denied);
    EXPECT_EQ(reloaded.source, Moqi::TypeDuck::PreferencesSource::File);
    EXPECT_EQ(reloaded.preferences.pageSize, 9);
    EXPECT_FALSE(reloaded.preferences.asciiMode);
  }
  // Always restore, so the next run's remove_all() can clean up.
  std::filesystem::permissions(denied, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
}

TEST(TypeDuckPreferences, AbsentFileMatchingTheDefaultIsNotCreated) {
  // First run, engine echoes its default mode (Chinese). The stored state and the
  // new value already agree -- Absent means the defaults ARE what is on disk -- so
  // persistTypeDuckAsciiMode() must take its no-op early-out and create NOTHING.
  // Writing here is pure downside: on a machine where the directory is not
  // writable it burns every retry and warns the user about a change they never
  // made. This asserts the exact condition that guards that early-out.
  const auto root = makeTempDir("typeduck-absent-noop-test");
  const auto path = root / "preferences.json";
  ASSERT_FALSE(std::filesystem::exists(path));

  // What the backend echoes on a fresh install: asciiMode == the default, false.
  const bool asciiModeFromEngine = Moqi::TypeDuck::defaultPreferences().asciiMode;

  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  ASSERT_EQ(loaded.source, Moqi::TypeDuck::PreferencesSource::Absent);

  // persistTypeDuckAsciiMode()'s early-out, verbatim. It must fire.
  EXPECT_TRUE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded) &&
              loaded.preferences.asciiMode == asciiModeFromEngine);

  // Reading must not have created the file, and because the early-out fires no
  // save runs -- so nothing exists on disk afterwards, not even a temp file.
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));
  EXPECT_TRUE(std::filesystem::is_empty(root));

  // The other half of the same branch: the first Shift press to the NON-default
  // mode does differ from the stored state, so the early-out must NOT fire and
  // the file must be created.
  const bool toggledMode = !asciiModeFromEngine;
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded) &&
               loaded.preferences.asciiMode == toggledMode);
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(loaded));

  auto prefs = loaded.preferences;
  prefs.asciiMode = toggledMode;
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, prefs).ok);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));

  const auto reloaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_EQ(reloaded.source, Moqi::TypeDuck::PreferencesSource::File);
  EXPECT_EQ(reloaded.preferences.asciiMode, toggledMode);
}

TEST(TypeDuckPreferences, CorruptFileIsReportedCorruptAndStaysWritable) {
  // The counterpart to UnreadableFileIsNeverOverwrittenBySave, and the reason the
  // enum has to be SPLIT rather than just "unreadable": a file whose bytes we did
  // read and which contains nothing parseable has nothing to lose. Blocking the
  // write here would wedge the Shift toggle and the settings dialog forever on a
  // single bad byte. It must self-heal instead.
  const auto root = makeTempDir("typeduck-corrupt-writable-test");
  const auto path = root / "preferences.json";
  writeFile(path, "{ this is not json at all ]]");

  const auto loaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_EQ(loaded.source, Moqi::TypeDuck::PreferencesSource::Corrupt);
  EXPECT_FALSE(loaded.ok);
  EXPECT_FALSE(Moqi::TypeDuck::preferencesAreAuthoritative(loaded));
  // The load says "do not trust these values" but NOT "do not write": corruption
  // must never be mistaken for an I/O failure.
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(loaded));

  auto prefs = loaded.preferences;
  prefs.asciiMode = true;
  const auto saved = Moqi::TypeDuck::savePreferences(path, prefs);
  ASSERT_TRUE(saved.ok) << saved.message;
  EXPECT_FALSE(std::filesystem::exists(root / "preferences.json.tmp"));

  const auto reloaded = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_EQ(reloaded.source, Moqi::TypeDuck::PreferencesSource::File);
  EXPECT_TRUE(reloaded.ok);
  EXPECT_TRUE(reloaded.preferences.asciiMode);

  // An empty file is corrupt in the same way -- read fine, nothing parseable in
  // it -- and must likewise self-heal rather than jam the toggle.
  writeFile(path, "");
  const auto loadedEmpty = Moqi::TypeDuck::loadPreferences(path);
  EXPECT_EQ(loadedEmpty.source, Moqi::TypeDuck::PreferencesSource::Corrupt);
  EXPECT_FALSE(Moqi::TypeDuck::preferencesBlockWrites(loadedEmpty));
  ASSERT_TRUE(Moqi::TypeDuck::savePreferences(path, prefs).ok);
  EXPECT_EQ(Moqi::TypeDuck::loadPreferences(path).source,
            Moqi::TypeDuck::PreferencesSource::File);
}
