export module llm.core;

import std;

export namespace llm {

using Strings = std::vector<std::string>;
using Path = std::filesystem::path;

struct SourceLocation {
  Path File;
  std::size_t Line = 1;
  std::size_t Column = 1;
};

struct Diagnostic {
  std::string Message;
  SourceLocation Location;
  Strings Notes;
};

template <class T> using Result = std::expected<T, Diagnostic>;

enum class ProductType {
  Executable,
  StaticLibrary,
  SharedLibrary,
  HeaderOnlyLibrary,
  Test
};

enum class Target {
  WindowsX64,
  WindowsArm64,
  LinuxX64,
  LinuxArm64,
  MacosArm64,
  WindowsGnuX64,
  WindowsGnuArm64,
  LinuxMuslX64,
  LinuxMuslArm64
};

inline constexpr std::array Targets = {
    Target::WindowsX64,      Target::WindowsArm64,   Target::WindowsGnuX64,
    Target::WindowsGnuArm64, Target::LinuxX64,       Target::LinuxArm64,
    Target::LinuxMuslX64,    Target::LinuxMuslArm64, Target::MacosArm64};

bool isWindowsTarget(Target Value);
bool isMsvcTarget(Target Value);
bool isMuslTarget(Target Value);
bool isArm64Target(Target Value);

enum class PlatformLayer {
  Windows,
  Linux,
  Macos,
  WindowsX64,
  WindowsArm64,
  LinuxX64,
  LinuxArm64,
  WindowsMsvc,
  WindowsGnu,
  LinuxGnu,
  LinuxMusl
};

enum class Language { C, Cxx, Assembly };
enum class Profile { Debug, Release };
enum class Standard {
  C89,
  C90,
  C99,
  C11,
  C17,
  C23,
  Cxx98,
  Cxx03,
  Cxx11,
  Cxx14,
  Cxx17,
  Cxx20,
  Cxx23
};

std::string_view productTypeName(ProductType Type);
Result<ProductType> parseProductType(std::string_view Name);
bool isLibrary(ProductType Type);
std::string_view targetName(Target Value);
Result<Target> parseTarget(std::string_view Name);
std::string_view standardName(Standard Value);
Result<Standard> parseStandard(std::string_view Name);
Language standardLanguage(Standard Value);
std::optional<Language> sourceLanguage(const Path &File);
bool isModuleSource(const Path &File);
bool isHeader(const Path &File);
std::string pathText(const Path &Value);
Path pathFromText(std::string_view Value);
bool isRelativeSafe(const Path &Value);
bool isProductName(std::string_view Name);
bool matchesGlob(std::string_view Pattern, std::string_view Text);
Result<void> validateFlags(const Strings &Flags);

struct Visibility {
  Strings Private;
  Strings Public;
};

struct Flags {
  Strings Compile;
  Strings C;
  Strings Cxx;
  Strings Assembly;
  Strings Link;
};

struct Generator {
  Strings Inputs;
  Strings Outputs;
  Strings Command;
  SourceLocation Location;
};

struct Layer {
  Strings Sources;
  std::map<std::string, Strings> Inputs;
  Strings Excludes;
  Strings Libraries;
  Strings Resources;
  std::optional<Standard> CStandard;
  std::optional<Standard> CxxStandard;
  Visibility Includes;
  Visibility Defines;
  Visibility Dependencies;
  Flags Arguments;
  std::vector<Generator> Generators;
  std::optional<Strings> TestArguments;
  std::optional<std::string> WorkingDirectory;
  std::optional<double> Timeout;
  std::map<std::string, std::string> Environment;
};

struct Manifest {
  Path Root;
  std::string Name;
  ProductType Type = ProductType::Executable;
  Layer Base;
  std::map<PlatformLayer, Layer> Platforms;
  std::map<std::string, std::string> Registries;
  std::map<std::string, SourceLocation> Origins;
};

struct EffectiveSettings {
  Strings Sources;
  std::map<std::string, Strings> Inputs;
  Strings InactiveSources;
  Strings Excludes;
  Strings Libraries;
  Strings Resources;
  Standard CStandard = Standard::C23;
  Standard CxxStandard = Standard::Cxx23;
  Visibility Includes;
  Visibility Defines;
  Visibility Dependencies;
  Flags Arguments;
  std::vector<Generator> Generators;
  Strings TestArguments;
  std::optional<std::string> WorkingDirectory;
  std::optional<double> Timeout;
  std::map<std::string, std::string> Environment;
};

Result<void> validateManifest(const Manifest &Project);
Result<EffectiveSettings> effectiveSettings(const Manifest &Project,
                                            Target BuildTarget);
Result<std::vector<Path>> selectSources(const EffectiveSettings &Settings,
                                        const std::vector<Path> &Inventory);

struct Version {
  std::string Text;
  std::array<std::string, 3> Components;
  Strings Prerelease;
};

struct VersionRequirement {
  std::optional<Version> Exact;
  std::array<std::optional<std::string>, 3> Components;
  std::optional<Version> Minimum;
  std::optional<Version> Maximum;
  bool IncludeMinimum = true;
  bool IncludeMaximum = false;

  bool matches(const Version &Candidate) const;
};

Result<Version> parseVersion(std::string_view Text);
int compareVersions(const Version &Left, const Version &Right);
Result<VersionRequirement> parseRequirement(std::string_view Text);

struct Url {
  std::string Scheme = "https";
  std::string Host;
  unsigned Port = 443;
  std::string Origin;
  std::string Resource;
};

struct RegistrySource {
  std::string Url;
  std::string Owner;
  std::string Repository;
  bool Github = false;
};

struct PackageVersion {
  Version Selected;
  std::string Url;
  std::string Sha256;
};

Result<Url> parseHttpsUrl(std::string_view Text);
Result<Url> parseRegistryUrl(std::string_view Text);
Result<RegistrySource> parseRegistrySource(std::string_view Text);
Result<std::vector<PackageVersion>> parsePackageVersions(std::string_view Text);
std::string packageIdentity(const RegistrySource &Source,
                            std::string_view Name);

struct DependencyReference {
  std::string Spelling;
  bool Public = false;
  std::optional<Path> Local;
  std::string Registry;
  std::string Package;
  VersionRequirement Requirement;
  std::string RequirementText;
  SourceLocation Location;
};

Result<DependencyReference> dependencyReference(const Manifest &Project,
                                                std::string Spelling,
                                                bool Public,
                                                SourceLocation Location = {});

struct PackageRequirement {
  std::string Identity;
  VersionRequirement Requirement;
  std::string Chain;
};

struct PackageCandidate {
  PackageVersion Package;
  std::vector<PackageRequirement> Dependencies;
  bool MetadataAvailable = true;
};

struct PackageCatalog {
  std::map<std::string, std::vector<PackageCandidate>> Packages;
  std::map<std::string, std::string> LockedVersions;
  std::set<std::string> Unlocked;
};

struct PackageResolution {
  std::map<std::string, PackageCandidate> Packages;
};

struct PackageMetadataRequest {
  std::string Identity;
  std::optional<PackageVersion> Selected;
};
using PackageResolutionStep =
    std::variant<PackageResolution, PackageMetadataRequest>;
Result<PackageResolutionStep>
resolvePackageStep(const std::vector<PackageRequirement> &Requirements,
                   const PackageCatalog &Catalog);

Result<PackageResolution>
resolvePackages(const std::vector<PackageRequirement> &Requirements,
                const PackageCatalog &Catalog);

struct UsageRequirements {
  Strings Includes;
  Strings Defines;
  std::set<std::string> VisibleProjects;
};

struct DependencyEdge {
  std::string Provider;
  bool Public = false;
};

struct ImportedProduct {
  Path Artifact;
  Path LinkArtifact;
  std::map<std::string, Path> StandardObjects;
  Target BuildTarget = Target::LinuxX64;
  Profile Mode = Profile::Debug;
  std::string ToolchainIdentity;
  bool UsesCxx = false;
};

struct ProjectInput {
  std::string Identity;
  Manifest Project;
  std::vector<Path> Inventory;
  std::vector<DependencyEdge> Dependencies;
  std::optional<ImportedProduct> Imported;
  Path RegistryInstance;
  bool Complete = true;
};

struct ProjectNode {
  ProjectInput Input;
  EffectiveSettings Settings;
  std::vector<Path> Sources;
  UsageRequirements Compile;
  UsageRequirements Exported;
  Strings LinkOrder;
  Strings Runtimes;
};

struct ProjectGraph {
  std::string Root;
  std::map<std::string, ProjectNode> Projects;
  Strings BuildOrder;
  bool Complete = true;
};

Result<ProjectGraph> buildGraph(std::string Root,
                                std::vector<ProjectInput> Projects,
                                Target BuildTarget);

struct ModuleProvider {
  std::string Name;
  bool Interface = false;
};

struct ModuleFacts {
  std::string Project;
  Path Source;
  std::optional<ModuleProvider> Provider;
  Strings Imports;
};

struct ModuleOrder {
  std::vector<std::size_t> Order;
  std::vector<std::vector<std::size_t>> Dependencies;
};

Result<ModuleOrder> orderModules(const ProjectGraph &Graph,
                                 const std::vector<ModuleFacts> &Modules);

enum class ActionKind { Generate, Scan, Compile, Archive, Link };

struct CompileUnit {
  Path Source;
  Language SourceLanguage = Language::Cxx;
  Standard LanguageStandard = Standard::Cxx23;
  Strings Includes;
  Strings Defines;
  Strings Arguments;
};

struct BuildAction {
  ActionKind Kind = ActionKind::Compile;
  std::string Project;
  std::vector<Path> Inputs;
  std::vector<Path> Outputs;
  std::vector<std::size_t> Dependencies;
  std::variant<std::monostate, CompileUnit, Generator> Details;
};

struct BuildContext {
  Target BuildTarget = Target::LinuxX64;
  Profile Mode = Profile::Debug;
  bool CheckOnly = false;
  Path OutputRoot;
  std::set<std::string> Products;
};

struct BuildPlan {
  BuildContext Context;
  std::vector<BuildAction> Actions;
  std::map<std::string, Path> Artifacts;
  std::vector<std::size_t> Roots;
};

Path artifactName(std::string_view Name, ProductType Type, Target BuildTarget);
Path linkArtifactName(std::string_view Name, ProductType Type,
                      Target BuildTarget);
Result<BuildPlan> planBuild(const ProjectGraph &Graph,
                            const BuildContext &Context);

}
