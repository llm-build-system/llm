export module llm.runtime;

export import llm.core;
import std;

export namespace llm {

enum class Operation {
  New,
  Build,
  Run,
  Debug,
  Check,
  Rebuild,
  Clean,
  Add,
  Remove,
  Update,
  Login,
  Logout,
  Test,
  Format,
  Analyze,
  List,
  Install,
  Uninstall,
  CheckInstallation,
  Package
};

enum class ColorMode { Automatic, Always, Never };
enum class EventKind {
  Progress,
  Warning,
  Error,
  Note,
  ToolOutput,
  StandardOutput
};

struct Event {
  EventKind Kind = EventKind::Progress;
  std::string Text;
};

struct Presentation {
  bool Quiet = false;
  bool Verbose = false;
  ColorMode Color = ColorMode::Automatic;
};

Target nativeTarget();
bool executableTarget(Target Value);

struct Command {
  Operation Action = Operation::Build;
  Path Project;
  Target BuildTarget = nativeTarget();
  bool TargetExplicit = false;
  std::optional<Path> InstallPath;
  bool Global = false;
  Profile Mode = Profile::Debug;
  int Jobs = 0;
  bool JobsExplicit = false;
  bool NoBuild = false;
  bool All = false;
  bool ListOnly = false;
  bool Json = false;
  bool Public = false;
  bool FormatCheck = false;
  bool IncludeTests = false;
  ProductType NewType = ProductType::Executable;
  std::string Selection;
  std::string Filter = "**";
  std::string Checks;
  std::string WarningsAsErrors;
  std::optional<double> Timeout;
  Strings Arguments;
  std::vector<Path> FormatPaths;
};

struct Context {
  std::string Version;
  Path Executable;
  Path InvocationDirectory;
  std::optional<Path> Toolchain;
  Path UserDirectory;
  Presentation Output;
  std::function<void(const Event &)> Emit;
  std::function<std::string(std::string_view)> ReadSecret;
  std::stop_token Cancellation;
};

Result<int> execute(Context &Call, const Command &Request);
Result<int> runInternal(Context &Call, const Strings &Arguments);

}
