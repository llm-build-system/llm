module llm.runtime:internal;

import llm.runtime;
import std;

namespace llm::runtime {

void enableCancellation();
bool interrupted();

class Failure : public std::exception {
public:
  Diagnostic Error;

  explicit Failure(Diagnostic Value) : Error(std::move(Value)) {}
  explicit Failure(std::string Message) : Error{std::move(Message)} {}
  const char *what() const noexcept override { return Error.Message.c_str(); }
};

template <class T> T take(Result<T> Value) {
  if (!Value)
    throw Failure(std::move(Value.error()));
  return std::move(*Value);
}

inline void take(Result<void> Value) {
  if (!Value)
    throw Failure(std::move(Value.error()));
}

inline void cancelled(const Context &Call) {
  if (Call.Cancellation.stop_requested() || interrupted())
    throw Failure("Operation cancelled");
}

inline void emit(Context &Call, EventKind Kind, std::string Text) {
  if (Call.Emit)
    Call.Emit(Event{Kind, std::move(Text)});
}

struct ProcessResult {
  int ExitCode = 0;
  int Signal = 0;
  bool TimedOut = false;
  bool success() const { return ExitCode == 0 && Signal == 0 && !TimedOut; }
};

struct ProcessOptions {
  Path Directory;
  std::optional<Path> Output;
  std::optional<Path> StandardOutput;
  std::optional<Path> StandardInput;
  std::optional<Path> ToolBin;
  bool OutputToError = false;
  bool Interactive = false;
  std::chrono::milliseconds Timeout{};
  std::map<std::string, std::string> Environment;
  std::string *CaptureOutput = nullptr;
  std::string *CaptureError = nullptr;
};

class FileLock {
  struct Impl;
  std::unique_ptr<Impl> State;

public:
  explicit FileLock(Context &Call, const Path &File, bool Wait = false);
  ~FileLock();
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;
};

struct InstallLayout {
  Path ProgramRoot;
  Path DataRoot;
  Path CommandPath;
  bool System = false;
  Path ArchiveCache;

  Path cache() const {
    return ArchiveCache.empty() ? DataRoot / "cache" : ArchiveCache;
  }
};

struct SdkPackage {
  std::string Id;
  std::string Url;
  std::string Sha256;
  std::string ArchiveRoot;
  std::string Format;
  Path Destination;
  std::vector<Path> RequiredFiles;
  std::vector<std::pair<std::string, std::string>> Mappings;
  std::uint64_t MaxDownload = 512ULL * 1024 * 1024;
  std::uint64_t MaxExpanded = 8ULL * 1024 * 1024 * 1024;
  bool ToolchainOnly = false;
};

struct ToolchainBundle {
  std::string Id;
  std::vector<SdkPackage> Packages;
};

struct HttpRequest {
  std::map<std::string, std::string> Headers;
  std::uint64_t Limit = 512ULL * 1024 * 1024;
};

struct HttpResponse {
  unsigned Status = 0;
  std::map<std::string, std::string> Headers;
  std::string Url;
};

struct LockData {
  std::map<std::string, PackageVersion> Packages;
};

struct BuildResult {
  Path Executable;
  std::map<Path, Strings> CompileCommands;
  std::map<Path, ModuleFacts> Modules;
};
enum class BuildDetails { None, Arguments, Modules };

Path userHome();
Path executablePath();
InstallLayout systemLayout();
InstallLayout userLayout();
InstallLayout installationLayout(const Context &Call);
bool elevated();
int elevateOperation(Context &Call, std::string_view Operation,
                     std::optional<Target> BuildTarget = {});
void protectPrivatePath(const Path &File, bool Directory);
void checkPrivateFile(const Path &File);
std::string readSecret(std::string_view Prompt);
ProcessResult runProcess(Context &Call, const Path &Executable,
                         const Strings &Arguments,
                         const ProcessOptions &Options);
std::string quoteArgument(std::string_view Text);
void secureDataDirectory(const Path &Directory, bool System);
struct PathEntry {
  Path File;
  std::string Text;
  bool Created = false;
  bool Separator = false;
};
void validateCommandRegistration(const InstallLayout &Layout);
std::vector<PathEntry> registerCommand(const InstallLayout &Layout,
                                       std::span<const PathEntry> Previous);
void unregisterCommand(const InstallLayout &Layout,
                       std::span<const PathEntry> Entries);
void removeInstalledFile(const Path &File);
void finishUninstall(const InstallLayout &Layout, const Path &VersionRoot);
void activateCommand(const Path &Source, const Path &Destination);
void rejectSymlinkAncestors(const Path &File);
bool redirectsPath(const Path &File);
std::string collisionKey(const Path &File);

Path nativePath(const Path &File);
std::string readText(const Path &File);
void writeText(const Path &File, std::string_view Text);
void replaceFile(const Path &Source, const Path &Destination);
void removeOutputFile(const Path &File);
void atomicWrite(const Path &File, std::string_view Text);
std::string uniqueId();
std::string sha256File(const Path &File);
std::string sha256Text(std::string_view Text);
std::string randomHex(std::size_t Bytes);
HttpResponse httpsGet(Context &Call, std::string Url, const Path &Output,
                      const HttpRequest &Request = {});
HttpResponse httpGet(Context &Call, std::string Url, const Path &Output,
                     const HttpRequest &Request = {});
void downloadToolchainArchive(Context &Call, const std::string &Url,
                              const Path &Output, std::uint64_t Limit);
std::string redirectUrl(std::string_view Base, std::string_view Location);

Manifest parseManifest(std::string_view Text, const Path &File);
Manifest readManifest(const Path &Root);
std::vector<Path> sourceInventory(const Path &Root);
Path projectRoot(const Context &Call, const Path &Requested);
ProjectGraph loadGraph(Context &Call, const Manifest &Project,
                       Target BuildTarget,
                       const std::map<std::string, Path> &Packages = {},
                       bool AllowUnresolved = false);
LockData parseLock(std::string_view Text);
std::string lockText(const LockData &Lock);
std::string jsonString(std::string_view Text);
std::optional<ImportedProduct> readImportedProduct(const Path &Root,
                                                   Target BuildTarget);

ToolchainBundle pinnedBundle(Target BuildTarget);
ToolchainBundle hostBundle(Target Host);
ToolchainBundle targetBundle(Target BuildTarget);
Path targetToolchain(const Path &Tools, Target BuildTarget);
Path compilerConfig(const Path &Tools, Target BuildTarget, bool Cxx);
std::string toolchainIdentity(const Path &Tools, Target BuildTarget);
void prepareTargetToolchain(Context &Call, const Path &Tools,
                            const Path &TargetRoot, Target BuildTarget);
void validateTargetToolchain(Context &Call, const Path &Tools,
                             const Path &TargetRoot, Target BuildTarget);
bool completeTargetInstallation(Context &Call, const Path &TargetRoot,
                                Target BuildTarget, bool Audit = true);
std::string bundleFingerprint(const ToolchainBundle &Bundle);
Path installedToolchain(const Context &Call);
Path debuggerPath(const Path &Toolchain);
std::map<std::string, std::string> debuggerEnvironment(const Path &Toolchain);
void prepareToolchain(Context &Call, const Path &Toolchain);
void validateToolchain(Context &Call, const Path &Toolchain);
void extractPackage(Context &Call, const Path &Archive, const Path &Destination,
                    const SdkPackage &Package);
void createTarball(Context &Call, const Path &Root, const Path &Destination,
                   const std::map<Path, Path> &Symlinks = {});
BuildResult buildProject(Context &Call, const Command &Request,
                         const ProjectGraph &Graph,
                         BuildDetails Details = BuildDetails::None);
BuildResult buildProjects(Context &Call, const Command &Request,
                          const std::vector<ProjectGraph> &Graphs,
                          const Path &Owner,
                          BuildDetails Details = BuildDetails::None);
struct BuildFailure final : Failure {
  int ExitCode;
  BuildFailure(int Code, std::string Text)
      : Failure(std::move(Text)), ExitCode(Code) {}
};
struct ModuleUnit {
  std::string Project;
  Path Source, Object, Bmi, Scan, Flags, Standard, Compat;
};
struct ModuleLink {
  Path Output, Flags;
  std::set<std::string> Projects;
};
struct ModulePlan {
  Path Root, Dyndep;
  std::map<std::string, std::set<std::string>> Visible;
  std::vector<ModuleUnit> Units;
  std::vector<ModuleLink> Links;
};
std::string moduleKey(std::string_view Text);
Strings responseArguments(std::string_view Text);
std::string modulePlanText(const ModulePlan &Plan);
ModulePlan readModulePlan(const Path &File);
std::vector<ModuleFacts> resolveModules(const ModulePlan &Plan);
int collateModules(Context &Call, const Path &Descriptor);
ModuleFacts readModuleScan(const Path &File, const Path &Output,
                           const Path &Root);
std::string responseArgument(std::string_view Text);
Strings compileArguments(const CompileUnit &Unit, const ProjectNode &Node,
                         const BuildContext &Build, const Path &Toolchain);
Path compilerPath(const Path &Toolchain, bool Cxx);
Strings defaultLinkArguments(const Path &TargetRoot, Target Target, bool Cxx);
std::map<std::string, std::string> toolEnvironment(const Path &TargetRoot,
                                                   Target Target);
bool linkUsesCxx(const ProjectGraph &Graph, const std::string &Identity);
Strings linkArguments(const BuildAction &Action, const ProjectGraph &Graph,
                      const BuildContext &Build, const Path &Toolchain,
                      const std::map<std::string, Path> &StandardObjects);
using OwnedOutputs = std::set<Path>;
bool ownedPath(const Path &File);
OwnedOutputs readOwned(const Path &Root);
void checkOutput(const Path &Root, const Path &File);
void saveOwned(const Path &Root, const OwnedOutputs &Owned);
void removeOutputs(const Path &Root, const OwnedOutputs &Owned);
Path findProgram(const Path &Root, std::string_view Name);
struct InstallRequest {
  InstallLayout Layout;
  Path Executable;
  std::string Version;
  ToolchainBundle Bundle;
};
struct InstallResult {
  Path VersionRoot;
  bool Reused = false;
};
struct UninstallResult {
  Path VersionRoot;
  bool Removed = false;
  bool CommandRemoved = false;
};
Path installationFile(const Path &Prefix);
bool readInstallIdentity(std::istream &Input, std::string &Version,
                         std::string &Catalog, std::string &ExecutableHash);
bool completeInstallation(Context &Call, const Path &Prefix,
                          const std::string &Version,
                          const ToolchainBundle &Bundle);
InstallResult installRelease(Context &Call, const InstallRequest &Request);
UninstallResult uninstallRelease(Context &Call, const InstallRequest &Request);
InstallResult installTarget(Context &Call, const InstallRequest &Request,
                            Target BuildTarget);
UninstallResult uninstallTarget(Context &Call, const InstallRequest &Request,
                                Target BuildTarget);
int installationCommand(Context &Call, const Command &Request);
struct TestSuite {
  Manifest Project;
  std::string Selector;
};
std::vector<TestSuite> discoverTests(const Manifest &Project);
int testProject(Context &Call, const Command &Request, const Manifest &Project);
std::map<std::string, Path>
resolveDependencies(Context &Call, const Command &Request, const Path &Owner,
                    const std::vector<Manifest> &Projects);
std::map<std::string, Path> cachedDependencies(Context &Call,
                                               const Path &Owner);
int projectTools(Context &Call, const Command &Request,
                 const Manifest &Project);
int registryCommand(Context &Call, const Command &Request,
                    const Manifest &Project);
int packageProject(Context &Call, const Command &Request,
                   const Manifest &Project);

}
