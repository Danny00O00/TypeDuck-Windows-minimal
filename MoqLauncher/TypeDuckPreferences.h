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
// "Could not read it", "read it, it was garbage" and "read it, could not decide"
// are THREE different verdicts, and the difference decides whether the file may be
// overwritten:
//   - IoError: the bytes are still there, we just cannot see them. Rewriting the
//     file would destroy settings we never read -- every unknown key, every value
//     this build does not model. NEVER write.
//   - Corrupt: the bytes were read and the parser RETURNED FAILURE on them (or the
//     root is not an object). Definitively garbage: there is nothing parseable in
//     there to lose, so a rewrite cannot destroy anything. Self-healing is right.
//   - Undetermined: the bytes were read and the parser THREW. That is NOT a
//     verdict of garbage -- jsoncpp throws on a perfectly VALID document that
//     merely nests past its reader stack limit -- so the file may be holding every
//     one of the user's settings. We simply could not decide. NEVER write.
// Only a verdict we are CERTAIN of may authorise a write. "The parser gave up" is
// not certainty, and filing it as Corrupt (which is writable by design) is how a
// valid settings file with one deeply nested unknown key used to get rewritten
// from defaults on the next Shift press.
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
  // The bytes were read and the parser RETURNED FAILURE on them: they are not
  // valid JSON, or they parse to something that is not a JSON object. Nothing
  // parseable is in there, so nothing can be lost by rewriting it.
  Corrupt,
  // The bytes were read and the parser THREW: jsoncpp raises Json::RuntimeError
  // once a document nests past its CharReaderBuilder stack limit, and a std::
  // bad_alloc can surface here too. The document may be perfectly VALID and we
  // merely failed to decide -- so this is "unknown contents", exactly like
  // IoError, and NOT a licence to overwrite. Declared last so the enumerator
  // values of the cases above are unchanged.
  Undetermined,
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

// True for IoError and Undetermined: the two verdicts under which the contents of
// the file are UNKNOWN, so writing would destroy settings nobody has read. Every
// writer must check this before saving and bail out instead. Corrupt does NOT
// block writes -- the parser positively rejected those bytes, so nothing
// parseable is in the file and the rewrite that self-heals it loses nothing.
bool preferencesBlockWrites(const ValidationResult& result);

std::filesystem::path defaultPreferencesPath();
// Never throws. Every failure (missing file, denied file, unparseable bytes, a
// document nested deep enough to make jsoncpp raise Json::Exception) degrades to
// defaults plus a truthful PreferencesSource. The launcher calls this from the
// libuv loop thread that forwards backend replies to every client, ahead of the
// forwarding itself, so an escaping exception would break typing for everyone.
//
// The PreferencesSource it reports is the whole point: a parse that FAILED is
// Corrupt (writable), a parse that THREW is Undetermined (not writable). The two
// are not interchangeable -- see the enum.
ValidationResult loadPreferences(const std::filesystem::path& path);
// Never throws either: it parses the file on disk as a merge base, so that keys
// this build does not model survive the rewrite.
//
// REFUSES TO WRITE whenever preferencesBlockWrites is true of that merge base --
// IoError (we could not read the bytes) and Undetermined (we read them and could
// not decide what they are). A file whose contents are unknown must never be
// written over, or one Shift press would rewrite the user's settings from defaults
// and destroy everything the file was holding. The check happens BEFORE any
// directory is created and BEFORE any temp file is opened: the failure is reported
// in the result and NOTHING is touched on disk -- not the target, not the temp
// file. The two refusals carry DIFFERENT messages, because "we cannot open your
// file" and "your file is intact but we cannot make sense of it" send the user to
// different places. An Absent base (first run) and a Corrupt base (the parser
// rejected the bytes, so there is nothing in it to lose) both merge onto an empty
// object and write.
SaveResult savePreferences(const std::filesystem::path& path,
                           const Preferences& preferences);
ApplyResult applyPreferences(const std::filesystem::path& path,
                             const Preferences& preferences,
                             RimeApplier applyRime);

} // namespace Moqi::TypeDuck

#endif // _TYPEDUCK_PREFERENCES_H_
