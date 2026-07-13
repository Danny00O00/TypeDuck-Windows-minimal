#ifndef _TYPEDUCK_PREFERENCES_H_
#define _TYPEDUCK_PREFERENCES_H_

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Moqi::TypeDuck {

struct Preferences {
  std::vector<std::string> displayLanguages;
  std::string mainLanguage;
  int pageSize = 6;
  bool isHeiTypeface = false;
  std::string showRomanization;
  bool enableCompletion = true;
  bool enableCorrection = false;
  bool enableSentence = true;
  bool enableLearning = true;
  bool showReverseCode = true;
  bool isCangjie5 = true;
  bool asciiMode = false;
};

struct PreferenceDescriptor {
  std::string id;
  std::string label;
  std::string control;
  bool affectsRime = false;
};

struct CapabilityMetadata {
  std::string id;
  bool supported = true;
  std::string message;
};

// Where the Preferences in a ValidationResult actually came from.
//
// A load that cannot read the file hands back DEFAULTS, and defaults are
// byte-for-byte indistinguishable from legitimately stored values. Any caller
// that compares the loaded state against what it is about to write -- the
// ascii-mode persistence path skips its save when the stored value already
// matches, to avoid rewriting the file on every engine echo -- must be able to
// tell "the file says false" from "we never managed to read the file, here is a
// false we made up". Without that distinction a locked or corrupt file looks
// like a clean load of the value the caller wanted, so the save, the retries and
// the warning are all skipped and the user's setting silently reverts on the
// next restart.
enum class PreferencesSource {
  // Not produced by a file load at all (validatePreferences and friends).
  NotLoaded,
  // No preferences file exists yet: defaults ARE the truth on disk (first run).
  Absent,
  // Values were genuinely read and parsed out of the preferences file.
  File,
  // A preferences file is there, but it could not be opened, could not be
  // parsed, or is not a JSON object: defaults were substituted for values that
  // nobody has read.
  Unreadable,
};

struct ValidationResult {
  bool ok = true;
  Preferences preferences;
  std::string message;
  // Declared last, with a default member initializer, so the existing three-field
  // aggregate initializers ({ok, preferences, message}) keep compiling untouched.
  // Only loadPreferences ever sets this to anything but NotLoaded.
  PreferencesSource source = PreferencesSource::NotLoaded;
};

struct SaveResult {
  bool ok = true;
  std::string message;
};

struct RimeSideEffects {
  std::string defaultCustomFile;
  std::string defaultCustomPath;
  int pageSize = 6;
  bool enableCompletion = true;
  bool enableCorrection = false;
  bool enableSentence = true;
  bool enableLearning = true;
  bool isCangjie5 = true;
  std::string commonCustomFile;
  std::string commonPatchKey;
  std::vector<std::string> commonPatches;
  // Live engine option forwarded in the backend push; never mapped to yaml patches.
  bool asciiMode = false;
};

struct ApplyResult {
  bool ok = true;
  std::string message;
};

using RimeApplier = std::function<ApplyResult(const RimeSideEffects&)>;

Preferences defaultPreferences();
std::vector<PreferenceDescriptor> preferenceDescriptors();
std::vector<CapabilityMetadata> defaultCapabilities();
std::vector<std::string> interfaceOnlyPreferenceIdsForTest();

bool preferenceAffectsRime(const std::string& id);
bool preferenceIsLiveEngineOption(const std::string& id);
ValidationResult validatePreferences(const Preferences& preferences);
RimeSideEffects rimeSideEffects(const Preferences& preferences);

// True only when result.preferences were really read back from a preferences
// file. False for defaults that were invented because the file was absent or
// unreadable -- never treat those as proof of what is on disk.
bool preferencesWereReadFromFile(const ValidationResult& result);

std::filesystem::path defaultPreferencesPath();
// Never throws. Every failure (missing file, denied file, unparseable bytes, a
// document nested deep enough to make jsoncpp raise Json::Exception) degrades to
// defaults plus a truthful PreferencesSource. The launcher calls this from the
// libuv loop thread that forwards backend replies to every client, ahead of the
// forwarding itself, so an escaping exception would break typing for everyone.
ValidationResult loadPreferences(const std::filesystem::path& path);
// Never throws either: it parses the file on disk as a merge base.
SaveResult savePreferences(const std::filesystem::path& path,
                           const Preferences& preferences);
ApplyResult applyPreferences(const std::filesystem::path& path,
                             const Preferences& preferences,
                             RimeApplier applyRime);

} // namespace Moqi::TypeDuck

#endif // _TYPEDUCK_PREFERENCES_H_
