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
// "Could not read it" and "read it, it was garbage" are NOT the same verdict, and
// the difference decides whether the file may be overwritten:
//   - IoError: the bytes are still there, we just cannot see them. Rewriting the
//     file would destroy settings we never read -- every unknown key, every value
//     this build does not model. NEVER write.
//   - Corrupt: the bytes were read and there is nothing parseable in them, so a
//     rewrite cannot lose anything. Self-healing the file is the right move.
enum class PreferencesSource {
  // Not produced by a file load at all (validatePreferences and friends).
  NotLoaded,
  // No preferences file exists yet: defaults ARE the truth on disk (first run).
  // A KNOWN state, not an error: it must not warn, and it must not be confused
  // with a file we merely failed to read.
  Absent,
  // Values were genuinely read and parsed out of the preferences file.
  // Authoritative.
  File,
  // The file exists (or we cannot even tell whether it does) but could not be
  // opened or read: a denied ACL, an antivirus lock, a directory sitting where
  // the file belongs, a failing disk. WE DO NOT KNOW WHAT IS IN IT. Defaults were
  // substituted for values nobody has read, and the file MUST NOT be written over.
  IoError,
  // The bytes were read, but they are not valid JSON / not a JSON object.
  // Nothing parseable is in there, so nothing can be lost by rewriting it.
  Corrupt,
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

// True when result.preferences are a trustworthy picture of what is on disk:
// either they were parsed out of the file (File), or there is no file and the
// defaults therefore ARE the stored state (Absent, i.e. first run). A caller that
// wants to skip a redundant write -- "the value I am about to store is already
// the value on disk" -- must gate that skip on THIS, so that an unreadable or
// corrupt file (where the loaded values are invented) never masquerades as a
// match and silences the save.
bool preferencesAreAuthoritative(const ValidationResult& result);

// True ONLY for IoError: the file could not be read, so its contents are unknown
// and writing would destroy them. Every writer must check this before saving and
// bail out instead. Corrupt does NOT block writes -- nothing parseable is in the
// file, so the rewrite that self-heals it loses nothing.
bool preferencesBlockWrites(const ValidationResult& result);

std::filesystem::path defaultPreferencesPath();
// Never throws. Every failure (missing file, denied file, unparseable bytes, a
// document nested deep enough to make jsoncpp raise Json::Exception) degrades to
// defaults plus a truthful PreferencesSource. The launcher calls this from the
// libuv loop thread that forwards backend replies to every client, ahead of the
// forwarding itself, so an escaping exception would break typing for everyone.
ValidationResult loadPreferences(const std::filesystem::path& path);
// Never throws either: it parses the file on disk as a merge base, so that keys
// this build does not model survive the rewrite.
//
// REFUSES TO WRITE when that merge base comes back IoError: a file we could not
// read must never be written over, or one Shift press would rewrite the user's
// settings from defaults and destroy everything the unread file was holding. The
// failure is reported in the result and NOTHING is touched on disk -- not the
// target, not the temp file. An Absent base (first run) and a Corrupt base (there
// is nothing parseable in it to lose) both merge onto an empty object and write.
SaveResult savePreferences(const std::filesystem::path& path,
                           const Preferences& preferences);
ApplyResult applyPreferences(const std::filesystem::path& path,
                             const Preferences& preferences,
                             RimeApplier applyRime);

} // namespace Moqi::TypeDuck

#endif // _TYPEDUCK_PREFERENCES_H_
