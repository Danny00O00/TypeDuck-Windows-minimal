#include "TypeDuckPreferences.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include <json/json.h>

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace Moqi::TypeDuck {

namespace {

constexpr const char* kPreferencesFileName = "TypeDuckPreferences.json";
constexpr const char* kJsonSource = "TypeDuckPreferences.json";
constexpr const char* kApplyFailureMessage =
    "設定未能套用，已保留原有設定 / Settings could not be applied; existing settings were kept";
constexpr const char* kInvalidFileMessage =
    "設定檔無效，已使用預設值 / Settings file is invalid; defaults were loaded";
constexpr const char* kUnreadableFileMessage =
    "設定檔未能讀取，已使用預設值 / Settings file could not be read; defaults were loaded";
// Refusing to save is the SUCCESSFUL outcome of the rule "never write over a file
// we could not read": the user keeps every setting in the unread file and loses
// only this one change, instead of keeping this one change and losing the file.
constexpr const char* kUnreadableSaveMessage =
    "設定檔未能讀取，為免覆蓋原有設定，今次變更未有儲存 / Settings file could not be "
    "read; this change was not saved so that the existing settings are not overwritten";

const std::vector<std::string>& languageOrder() {
  static const std::vector<std::string> languages{"eng", "hin", "ind", "nep", "urd"};
  return languages;
}

bool isKnownLanguage(const std::string& language) {
  const auto& languages = languageOrder();
  return std::find(languages.begin(), languages.end(), language) != languages.end();
}

bool isKnownRomanizationMode(const std::string& value) {
  return value == "always" || value == "reverse_only" || value == "never";
}

struct PreferencesDocument {
  PreferencesSource source = PreferencesSource::IoError;
  // The document on disk when source == File; an empty object otherwise. Callers
  // merge onto it, so it is always a valid object and never needs a null check.
  Json::Value root{Json::objectValue};
};

// THE single place that decides what is on disk. loadPreferences and
// savePreferences both go through it, so the read that decides "may I write?"
// and the read that builds the merge base can never disagree -- if they could,
// save would happily rewrite from an empty base a file that load had just
// declared unreadable, which is exactly the bug this function exists to kill.
//
// Never throws: jsoncpp raises Json::Exception (e.g. on a document nested past
// its reader stack limit) and this runs on the launcher's libuv loop thread.
// An exception AFTER the bytes are in hand is a parse failure -> Corrupt; one
// before is a read failure -> IoError, the verdict that forbids writing.
PreferencesDocument readPreferencesDocument(const std::filesystem::path& path) {
  bool bytesWereRead = false;
  try {
    // Ask what is actually there BEFORE opening. "Missing" and "there but denied"
    // both fail to open and mean opposite things; and a DIRECTORY sitting where
    // the file belongs opens happily on POSIX (the read then fails) while Windows
    // refuses outright, so stat-ing first is what makes the verdict identical on
    // both platforms instead of a coin toss decided by the C library.
    // Key off the returned file_type, NOT off the error_code. libc++ reports a
    // plain missing file by setting ec to ENOENT *and* returning not_found, so
    // treating a non-empty ec as "cannot tell" would misfile every first run as
    // IoError -- which then refuses to create the file at all. file_type is the
    // channel the standard defines for exactly this question: not_found means the
    // file genuinely is not there, none means the status could not be determined.
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
      // First run, or a path under a directory that does not exist yet.
      return {PreferencesSource::Absent, Json::Value(Json::objectValue)};
    }
    if (status.type() == std::filesystem::file_type::none) {
      // We cannot even tell what is there (a denied parent directory, say).
      // "Unknown" must never be optimistically treated as "nothing".
      return {PreferencesSource::IoError, Json::Value(Json::objectValue)};
    }
    if (!std::filesystem::is_regular_file(status)) {
      // A directory (or a device, or a socket) sitting where the file belongs.
      return {PreferencesSource::IoError, Json::Value(Json::objectValue)};
    }

    std::error_code sizeEc;
    const auto expectedSize = std::filesystem::file_size(path, sizeEc);

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
      return {PreferencesSource::IoError, Json::Value(Json::objectValue)};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string contents = buffer.str();
    // Count the bytes; do not ask the source stream how it went. Inserting a
    // streambuf sets failbit/badbit on the DESTINATION, never on the ifstream,
    // and filebuf::underflow() reports a failed read() as plain eof -- so a
    // short read looks exactly like a clean end of file. A file on a dropped
    // network share (APPDATA is routinely redirected) therefore hands back
    // truncated bytes that fail to parse, and "unparseable" is a writable
    // verdict. Trusting the stream state here would rewrite a file we never
    // actually read.
    if (sizeEc || contents.size() != static_cast<std::size_t>(expectedSize)) {
      return {PreferencesSource::IoError, Json::Value(Json::objectValue)};
    }
    bytesWereRead = true;

    // Parse from the bytes we already hold, using the same CharReaderBuilder +
    // parseFromStream pair this file has always used -- the point of reading the
    // file ourselves is only to learn WHETHER the bytes arrived, which a parse
    // straight off the ifstream cannot tell us (it reports "not JSON" for a read
    // error and a garbage file alike, and those two need opposite verdicts).
    std::istringstream contentStream(contents);
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    if (!Json::parseFromStream(builder, contentStream, &root, &errors) ||
        !root.isObject()) {
      // Read fine, unparseable (or a JSON array/scalar, which cannot be merged
      // onto). There is nothing in there to lose, so a rewrite may self-heal it.
      return {PreferencesSource::Corrupt, Json::Value(Json::objectValue)};
    }
    return {PreferencesSource::File, root};
  } catch (const std::exception&) {
    return {bytesWereRead ? PreferencesSource::Corrupt : PreferencesSource::IoError,
            Json::Value(Json::objectValue)};
  } catch (...) {
    return {bytesWereRead ? PreferencesSource::Corrupt : PreferencesSource::IoError,
            Json::Value(Json::objectValue)};
  }
}

// `root` is the document already on disk (see existingPreferencesJson). We
// overwrite only the keys this build models and leave every other key untouched,
// so a preference written by a newer build, a hand edit, or a settings app this
// binary predates survives the rewrite. Starting from a fresh Json::Value would
// silently ERASE those keys — and since the Shift toggle persists asciiMode on
// every press, that erasure would land on the very first keystroke.
Json::Value preferencesToJson(const Preferences& preferences, Json::Value root) {
  if (!root.isObject()) {
    root = Json::Value(Json::objectValue);
  }
  Json::Value languages(Json::arrayValue);
  for (const auto& language : preferences.displayLanguages) {
    languages.append(language);
  }
  root["displayLanguages"] = languages;
  root["mainLanguage"] = preferences.mainLanguage;
  root["pageSize"] = preferences.pageSize;
  root["isHeiTypeface"] = preferences.isHeiTypeface;
  root["showRomanization"] = preferences.showRomanization;
  root["enableCompletion"] = preferences.enableCompletion;
  root["enableCorrection"] = preferences.enableCorrection;
  root["enableSentence"] = preferences.enableSentence;
  root["enableLearning"] = preferences.enableLearning;
  root["showReverseCode"] = preferences.showReverseCode;
  root["isCangjie5"] = preferences.isCangjie5;
  root["asciiMode"] = preferences.asciiMode;
  return root;
}

Preferences preferencesFromJson(const Json::Value& root) {
  Preferences preferences = defaultPreferences();
  if (root["displayLanguages"].isArray()) {
    preferences.displayLanguages.clear();
    for (const auto& item : root["displayLanguages"]) {
      if (item.isString()) {
        preferences.displayLanguages.push_back(item.asString());
      }
    }
  }
  if (root["mainLanguage"].isString()) {
    preferences.mainLanguage = root["mainLanguage"].asString();
  }
  if (root["pageSize"].isInt()) {
    preferences.pageSize = root["pageSize"].asInt();
  }
  if (root["isHeiTypeface"].isBool()) {
    preferences.isHeiTypeface = root["isHeiTypeface"].asBool();
  }
  if (root["showRomanization"].isString()) {
    preferences.showRomanization = root["showRomanization"].asString();
  }
  if (root["enableCompletion"].isBool()) {
    preferences.enableCompletion = root["enableCompletion"].asBool();
  }
  if (root["enableCorrection"].isBool()) {
    preferences.enableCorrection = root["enableCorrection"].asBool();
  }
  if (root["enableSentence"].isBool()) {
    preferences.enableSentence = root["enableSentence"].asBool();
  }
  if (root["enableLearning"].isBool()) {
    preferences.enableLearning = root["enableLearning"].asBool();
  }
  if (root["showReverseCode"].isBool()) {
    preferences.showReverseCode = root["showReverseCode"].asBool();
  }
  if (root["isCangjie5"].isBool()) {
    preferences.isCangjie5 = root["isCangjie5"].asBool();
  }
  if (root["asciiMode"].isBool()) {
    preferences.asciiMode = root["asciiMode"].asBool();
  }
  return preferences;
}

// Windows keeps the user profile path in UTF-16. The narrow CRT environment is a
// lossy ANSI-code-page copy of it ('?' replaces anything the active code page
// cannot encode), so a profile such as C:\Users\陳大文 on a non-CJK code page
// would yield an invalid path and every load/save would fail. Read the wide
// environment first (scripts and tests still override it), then fall back to the
// roaming known folder. Other platforms keep std::getenv("APPDATA") so the
// portable unit test build (Tests/TypeDuckSettings) keeps compiling and can
// redirect the path.
std::filesystem::path roamingAppDataPath() {
#ifdef _WIN32
  if (const wchar_t* wideValue = ::_wgetenv(L"APPDATA");
      wideValue != nullptr && wideValue[0] != L'\0') {
    return std::filesystem::path(wideValue);
  }
  wchar_t* roamingPath = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath)) ||
      roamingPath == nullptr) {
    return {};
  }
  std::filesystem::path result{roamingPath};
  ::CoTaskMemFree(roamingPath);
  return result;
#else
  const char* value = std::getenv("APPDATA");
  return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

} // namespace

Preferences defaultPreferences() {
  return Preferences{
      {"eng"},
      "eng",
      6,
      false,
      "always",
      true,
      false,
      true,
      true,
      true,
      true,
      false,
  };
}

std::vector<PreferenceDescriptor> preferenceDescriptors() {
  return {
      {"displayLanguages", "顯示語言 Display Languages", "radio-checkbox-list", false},
      {"pageSize", "每頁候選詞數量 No. of Candidates Per Page", "range", true},
      {"isHeiTypeface", "中文字體 Chinese Typeface", "segmented", false},
      {"showRomanization", "候選詞粵拼 Candidates Jyutping", "radio-list", false},
      {"enableCompletion", "自動完成 Auto-completion", "toggle", true},
      {"enableCorrection", "自動校正 Auto-correction", "toggle", true},
      {"enableSentence", "自動組詞 Auto-composition", "toggle", true},
      {"enableLearning", "輸入記憶 Input Memory", "toggle", true},
      {"reverseLookupHeader", "反查設定 Reverse Lookup Settings", "section", false},
      {"showReverseCode", "顯示完整輸入碼 Show Full Input Code", "toggle", false},
      {"isCangjie5", "倉頡版本 Cangjie Version", "segmented", true},
  };
}

std::vector<CapabilityMetadata> defaultCapabilities() {
  return {
      {"pageSize", true, ""},
      {"enableCompletion", true, ""},
      {"enableCorrection", true, ""},
      {"enableSentence", true, ""},
      {"enableLearning", true, ""},
      {"isCangjie5", true, ""},
      {"showReverseCode", true, ""},
      {"displayLanguages", true, ""},
      {"mainLanguage", true, ""},
      {"isHeiTypeface", true, ""},
      {"showRomanization", true, ""},
      {"asciiMode", true, ""},
  };
}

std::vector<std::string> interfaceOnlyPreferenceIdsForTest() {
  return {"displayLanguages", "mainLanguage", "isHeiTypeface",
          "showRomanization", "showReverseCode"};
}

bool preferenceAffectsRime(const std::string& id) {
  return id == "pageSize" || id == "enableCompletion" ||
         id == "enableCorrection" || id == "enableSentence" ||
         id == "enableLearning" || id == "isCangjie5";
}

// asciiMode is a live engine option: persisted in JSON and pushed to the
// backend via settings update, but it never patches Rime yaml and never
// triggers redeploy.
bool preferenceIsLiveEngineOption(const std::string& id) {
  return id == "asciiMode";
}

ValidationResult validatePreferences(const Preferences& preferences) {
  Preferences normalized = preferences;
  bool ok = true;
  std::string message;

  std::vector<std::string> languages;
  std::set<std::string> seen;
  for (const auto& language : preferences.displayLanguages) {
    if (!isKnownLanguage(language)) {
      ok = false;
      continue;
    }
    if (seen.insert(language).second) {
      languages.push_back(language);
    }
  }
  if (languages.empty()) {
    ok = false;
    languages = {"eng"};
  }
  normalized.displayLanguages = languages;

  if (!isKnownLanguage(normalized.mainLanguage)) {
    ok = false;
    normalized.mainLanguage = "eng";
  }
  if (std::find(normalized.displayLanguages.begin(),
                normalized.displayLanguages.end(),
                normalized.mainLanguage) == normalized.displayLanguages.end()) {
    normalized.displayLanguages.push_back(normalized.mainLanguage);
  }

  if (normalized.pageSize < 4 || normalized.pageSize > 10) {
    ok = false;
    normalized.pageSize = 6;
  }
  if (!isKnownRomanizationMode(normalized.showRomanization)) {
    ok = false;
    normalized.showRomanization = "always";
  }

  if (!ok) {
    message = "設定值無效，已使用預設值 / Invalid settings were replaced with defaults";
  }
  return {ok, normalized, message};
}

RimeSideEffects rimeSideEffects(const Preferences& preferences) {
  const auto normalized = validatePreferences(preferences).preferences;
  RimeSideEffects effects;
  effects.defaultCustomFile = "default.custom.yaml";
  effects.defaultCustomPath = "menu/page_size";
  effects.pageSize = normalized.pageSize;
  effects.asciiMode = normalized.asciiMode;
  effects.enableCompletion = normalized.enableCompletion;
  effects.enableCorrection = normalized.enableCorrection;
  effects.enableSentence = normalized.enableSentence;
  effects.enableLearning = normalized.enableLearning;
  effects.isCangjie5 = normalized.isCangjie5;
  effects.commonCustomFile = "common.custom.yaml";
  effects.commonPatchKey = "__patch";
  effects.commonPatches.push_back("common:/show_cangjie_roots");
  if (!normalized.enableCompletion) {
    effects.commonPatches.push_back("common:/disable_completion");
  }
  if (normalized.enableCorrection) {
    effects.commonPatches.push_back("common:/enable_correction");
  }
  if (!normalized.enableSentence) {
    effects.commonPatches.push_back("common:/disable_sentence");
  }
  if (!normalized.enableLearning) {
    effects.commonPatches.push_back("common:/disable_learning");
  }
  if (!normalized.isCangjie5) {
    effects.commonPatches.push_back("common:/use_cangjie3");
  }
  return effects;
}

std::filesystem::path defaultPreferencesPath() {
  const auto roaming = roamingAppDataPath();
  if (roaming.empty()) {
    return std::filesystem::path(kPreferencesFileName);
  }
  return roaming / "TypeDuckIME" / kPreferencesFileName;
}

bool preferencesWereReadFromFile(const ValidationResult& result) {
  return result.source == PreferencesSource::File;
}

bool preferencesAreAuthoritative(const ValidationResult& result) {
  // Absent counts: no file means the defaults we handed back really are the state
  // on disk. IoError and Corrupt do not: those preferences were invented.
  return result.source == PreferencesSource::File ||
         result.source == PreferencesSource::Absent;
}

bool preferencesBlockWrites(const ValidationResult& result) {
  // IoError ONLY. Corrupt is deliberately writable (self-heal), and NotLoaded is
  // not a statement about the disk at all -- it never came from a load.
  return result.source == PreferencesSource::IoError;
}

// Never throws; see the contract on the declaration. Failures do not just have
// to be survivable, they have to be HONEST: a result that carries defaults must
// say so, because a caller comparing the loaded value with the value it is about
// to write cannot otherwise tell a stored false from an invented one.
ValidationResult loadPreferences(const std::filesystem::path& path) {
  try {
    const auto document = readPreferencesDocument(path);
    switch (document.source) {
    case PreferencesSource::File: {
      auto result = validatePreferences(preferencesFromJson(document.root));
      // The bytes really came off the disk, even if some of the values in them
      // were out of range and got normalized away.
      result.source = PreferencesSource::File;
      if (!result.ok && result.message.empty()) {
        result.message = kInvalidFileMessage;
      }
      return result;
    }
    case PreferencesSource::Absent:
      // Not an error. First run: the defaults ARE what is on disk.
      return {true, defaultPreferences(), "", PreferencesSource::Absent};
    case PreferencesSource::Corrupt:
      return {false, defaultPreferences(), kInvalidFileMessage,
              PreferencesSource::Corrupt};
    case PreferencesSource::IoError:
    case PreferencesSource::NotLoaded:
      break;
    }
    return {false, defaultPreferences(), kUnreadableFileMessage,
            PreferencesSource::IoError};
  } catch (...) {
    // Backstop only -- readPreferencesDocument already absorbs the parse
    // exceptions (jsoncpp throws Json::Exception on a document nested past its
    // reader stack limit, which a hostile file reaches in a few kilobytes). What
    // is left is the likes of bad_alloc, and this runs on the libuv loop thread
    // ahead of reply forwarding, so it degrades instead of unwinding through it.
    // IoError, not Corrupt, is the conservative verdict: we no longer have a
    // trustworthy picture of the file, so nobody may overwrite it.
    return {false, defaultPreferences(), kUnreadableFileMessage,
            PreferencesSource::IoError};
  }
}

// Never throws: the merge base is parsed from disk, and Json::writeString can
// raise as well. The Shift toggle drives this from the libuv loop thread.
SaveResult savePreferences(const std::filesystem::path& path,
                           const Preferences& preferences) {
  try {
    const auto result = validatePreferences(preferences);
    if (!result.ok) {
      return {false, result.message};
    }

    // Read the merge base FIRST -- before creating a directory, before opening a
    // temp file, before touching anything at all.
    //
    // If the file is there but unreadable, this is where we STOP. Falling through
    // would merge the new value onto an EMPTY base and rename that over the file,
    // rewriting the user's settings from defaults and destroying every key the
    // unread file was holding. The Shift toggle calls this on every press, so
    // that would land on a single keystroke. Losing this one change is the
    // acceptable outcome; losing the file is not.
    const auto base = readPreferencesDocument(path);
    if (base.source == PreferencesSource::IoError) {
      return {false, kUnreadableSaveMessage};
    }
    // Absent (first run) and Corrupt (nothing parseable in there to lose) both
    // carry an empty object and write normally -- the Corrupt case self-heals.

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return {false, kApplyFailureMessage};
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    // Merge onto whatever is already on disk (read before the temp file is opened)
    // so keys this build does not model are preserved rather than erased.
    const Json::Value document =
        preferencesToJson(result.preferences, base.root);
    // Write to a sibling temp file and rename over the target so a crash or a
    // full disk mid-write can never leave a truncated preferences file behind.
    const auto tempPath =
        path.parent_path() / (path.filename().string() + ".tmp");
    {
      std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
      if (!stream.is_open()) {
        return {false, kApplyFailureMessage};
      }
      stream << Json::writeString(builder, document);
      stream.flush();
      if (!stream.good()) {
        stream.close();
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        return {false, kApplyFailureMessage};
      }
    }
    std::error_code renameEc;
    std::filesystem::rename(tempPath, path, renameEc);
    if (renameEc) {
      std::error_code removeEc;
      std::filesystem::remove(tempPath, removeEc);
      return {false, kApplyFailureMessage};
    }
    return {true, kJsonSource};
  } catch (const std::exception&) {
    return {false, kApplyFailureMessage};
  } catch (...) {
    return {false, kApplyFailureMessage};
  }
}

ApplyResult applyPreferences(const std::filesystem::path& path,
                             const Preferences& preferences,
                             RimeApplier applyRime) {
  const auto validation = validatePreferences(preferences);
  if (!validation.ok) {
    return {false, validation.message};
  }
  const auto saved = savePreferences(path, validation.preferences);
  if (!saved.ok) {
    return {false, saved.message};
  }
  if (applyRime) {
    const auto applied = applyRime(rimeSideEffects(validation.preferences));
    if (!applied.ok) {
      return {false, applied.message.empty() ? kApplyFailureMessage : applied.message};
    }
    return {true, applied.message.empty() ? "設定已套用 / Settings applied" : applied.message};
  }
  return {true, "設定已儲存 / Settings saved"};
}

} // namespace Moqi::TypeDuck
