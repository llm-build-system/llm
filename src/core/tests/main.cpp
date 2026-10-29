import std;
import llm.core;

namespace {
using namespace llm;

void require(bool Condition, std::string_view Message) {
  if (!Condition)
    throw std::runtime_error(std::string(Message));
}

template <class T> T take(Result<T> Value) {
  if (!Value)
    throw std::runtime_error(Value.error().Message);
  return std::move(*Value);
}

ProjectInput project(std::string Name,
                     ProductType Type = ProductType::StaticLibrary) {
  ProjectInput Input;
  Input.Identity = Name;
  Input.Project.Name = Name;
  Input.Project.Root = Path("projects") / Name;
  Input.Project.Type = Type;
  Input.Inventory = {"src/main.cpp"};
  return Input;
}

void configurationContract() {
  for (auto Target : Targets)
    require(take(parseTarget(targetName(Target))) == Target,
            "Canonical targets round-trip without merging ABIs");
  for (auto [Name, Target] :
       {std::pair{"amd64-pc-win32-msvc", Target::WindowsX64},
        std::pair{"arm64-pc-windows-msvc", Target::WindowsArm64},
        std::pair{"amd64-pc-windows-gnu", Target::WindowsGnuX64},
        std::pair{"arm64-w64-win32-gnu", Target::WindowsGnuArm64},
        std::pair{"x86_64-w64-mingw32", Target::WindowsGnuX64},
        std::pair{"arm64-w64-mingw32", Target::WindowsGnuArm64},
        std::pair{"amd64-unknown-linux-gnu", Target::LinuxX64},
        std::pair{"arm64-unknown-linux-gnu", Target::LinuxArm64},
        std::pair{"amd64-unknown-linux-musl", Target::LinuxMuslX64},
        std::pair{"arm64-unknown-linux-musl", Target::LinuxMuslArm64},
        std::pair{"arm64-apple-macosx", Target::MacosArm64},
        std::pair{"aarch64-apple-macos", Target::MacosArm64}})
    require(take(parseTarget(Name)) == Target,
            "LLVM aliases resolve to the same target identity");
  for (auto Name : {"x64-pc-windows-msvc", "arm64ec-pc-windows-msvc",
                    "aarch64-apple-macosx14.0", "aarch64-w64-linux-gnu",
                    "x86_64-unknown-linux-gnux32"})
    require(!parseTarget(Name),
            "Unsupported target semantics are not discarded");
  require(artifactName("answer", ProductType::StaticLibrary,
                       Target::WindowsGnuX64) == "libanswer.a" &&
              linkArtifactName("answer", ProductType::SharedLibrary,
                               Target::WindowsGnuArm64) == "libanswer.dll.a",
          "GNU Windows uses GNU archive and import-library names");
  auto Input = project("app", ProductType::Executable);
  auto &Base = Input.Project.Base;
  Base.CStandard = Standard::C17;
  Base.Arguments.Compile = {"-Wall", "-Wall"};
  Base.Arguments.Cxx = {"-Wextra"};
  auto &Os = Input.Project.Platforms[PlatformLayer::Windows];
  Os.CxxStandard = Standard::Cxx20;
  Os.Arguments.Compile = {"-Wconversion"};
  Os.Arguments.Cxx = {"-Wshadow"};
  auto &Arch = Input.Project.Platforms[PlatformLayer::WindowsArm64];
  Arch.CxxStandard = Standard::Cxx23;
  Arch.Arguments.Cxx = {"-Wpedantic"};
  Input.Project.Platforms[PlatformLayer::WindowsGnu].Defines.Public = {
      "GNU_ABI"};
  auto GnuSettings =
      take(effectiveSettings(Input.Project, Target::WindowsGnuArm64));
  require(GnuSettings.Defines.Public == Strings{"GNU_ABI"} &&
              GnuSettings.CxxStandard == Standard::Cxx23,
          "ABI settings retain the matching operating-system and architecture "
          "layers");
  auto Settings = take(effectiveSettings(Input.Project, Target::WindowsArm64));
  require(Settings.CStandard == Standard::C17 &&
              Settings.CxxStandard == Standard::Cxx23,
          "Standards override only their own language");
  require(Settings.Arguments.Cxx == Strings{"-Wall", "-Wall", "-Wextra",
                                            "-Wconversion", "-Wshadow",
                                            "-Wpedantic"},
          "Specificity ordering and repeated argv elements are preserved");
  require(!parseProductType("exe") && !parseStandard("c++23") &&
              !validateFlags({"--target=x"}),
          "No old aliases, normalization or compiler-control bypasses");
  require(!isRelativeSafe("../outside.cpp") &&
              !isRelativeSafe("C:/outside.cpp"),
          "Source patterns cannot escape their project");
  require(
      matchesGlob("src/**/*.cpp", "src/main.cpp") &&
          matchesGlob("src/**/*.cpp", "src/a/b.cpp") &&
          !matchesGlob("src/*.cpp", "src/a/b.cpp"),
      "Recursive source glob distinguishes directories from ordinary stars");
  require(matchesGlob("**", "tests/") && !matchesGlob("*", "tests/"),
          "The default test filter includes directory labels");
  auto Automatic =
      take(selectSources(Settings, {"main.c", "code/unit.cppm",
                                    "dist/helper.cpp", "tests/helper.cpp"}));
  require(Automatic.size() == 4,
          "Sources are automatic across the project; src, dist and tests have "
          "no implicit selection role");
  auto PlatformProject = Input.Project;
  PlatformProject.Platforms[PlatformLayer::Windows].Sources = {
      "platform/windows.cpp"};
  PlatformProject.Platforms[PlatformLayer::Linux].Sources = {
      "platform/posix.cpp"};
  PlatformProject.Platforms[PlatformLayer::Macos].Sources = {
      "platform/posix.cpp"};
  auto Linux = take(effectiveSettings(PlatformProject, Target::LinuxX64));
  require(take(selectSources(Linux, {"main.cpp", "platform/windows.cpp",
                                     "platform/posix.cpp"}))
                  .size() == 2,
          "Automatic discovery respects explicitly platform-assigned sources");
  Settings.Sources = {"missing.cpp"};
  require(!selectSources(Settings, Input.Inventory),
          "Missing explicit sources fail");
  Base.Generators = {{{}, {"obj/generated.cpp"}, {"generator"}, {}}};
  Base.Sources = {"obj/generated.cpp"};
  auto Generated = take(effectiveSettings(Input.Project, Target::LinuxX64));
  auto Files = take(selectSources(Generated, Input.Inventory));
  require(std::ranges::find(Files, Path("obj/generated.cpp")) != Files.end(),
          "Declared generated sources are known before their first creation");
  Input.Project.Platforms[PlatformLayer::Linux].Generators = Base.Generators;
  require(!effectiveSettings(Input.Project, Target::LinuxX64),
          "Active generator output collisions fail");
}

void graphAndModuleContract() {
  auto App = project("app", ProductType::Executable);
  auto Library = project("library");
  auto Hidden = project("hidden");
  App.Dependencies = {{"library", false}};
  Library.Dependencies = {{"hidden", false}};
  Library.Project.Base.Defines.Public = {"LIBRARY_API=1"};
  Hidden.Project.Base.Defines.Public = {"HIDDEN_API=1"};
  auto Graph =
      take(buildGraph("app", {App, Library, Hidden}, Target::LinuxX64));
  require(
      Graph.Projects.at("app").Compile.Defines == Strings{"LIBRARY_API=1"},
      "Private implementation compile requirements do not leak to consumers");
  require(Graph.Projects.at("app").LinkOrder == Strings{"library", "hidden"},
          "Private static dependencies remain in the final link closure");
  std::vector<ModuleFacts> Facts = {
      {"app", "src/main.cpp", {}, {"api", "std"}},
      {"library", "src/main.cpp", ModuleProvider{"api", true}, {}},
      {"hidden", "src/main.cpp", ModuleProvider{"secret", true}, {}}};
  auto Ordered = take(orderModules(Graph, Facts));
  require(Ordered.Dependencies[0] == std::vector<std::size_t>{1},
          "Consumer imports depend on their visible provider");
  Facts[0].Imports = {"secret"};
  require(!orderModules(Graph, Facts),
          "Private transitive module providers are hidden");
  Facts[0].Imports = {"api"};
  Facts[1].Imports = {"api"};
  require(!orderModules(Graph, Facts), "Module cycles fail before execution");
  Library.Dependencies.front().Public = true;
  auto PublicGraph =
      take(buildGraph("app", {App, Library, Hidden}, Target::LinuxX64));
  require(
      PublicGraph.Projects.at("app").Compile.VisibleProjects.contains("hidden"),
      "Public module visibility propagates");
  Hidden.Dependencies = {{"app", false}};
  require(!buildGraph("app", {App, Library, Hidden}, Target::LinuxX64),
          "Ordinary dependency cycles are rejected");
}

void resolutionContract() {
  auto Prerelease = take(parseVersion("1.0.0-rc.2"));
  auto Later = take(parseVersion("1.0.0-rc.10"));
  require(compareVersions(Prerelease, Later) < 0,
          "SemVer numeric prerelease ordering");
  require(!take(parseRequirement("1.*.0")).matches(Prerelease),
          "Wildcard requirements do not select prereleases");
  require(take(parseRequirement("1.0.0+one"))
                  .matches(take(parseVersion("1.0.0+one"))) &&
              !take(parseRequirement("1.0.0+one"))
                   .matches(take(parseVersion("1.0.0+two"))),
          "Exact build metadata retains package identity");
  require(!parseVersion("01.0.0") && !parseRequirement("1.*"),
          "Strict version syntax");
  for (const auto &[Text, Below, First, Inside, Outside] :
       {std::tuple{"^1.2.3", "1.2.2", "1.2.3", "1.99.99", "2.0.0"},
        std::tuple{"^0.2.3", "0.2.2", "0.2.3", "0.2.99", "0.3.0"},
        std::tuple{"^0.0.3", "0.0.2", "0.0.3", "0.0.3", "0.0.4"},
        std::tuple{"^0.0.0", "", "0.0.0", "0.0.0", "0.0.1"},
        std::tuple{"~1.2.3", "1.2.2", "1.2.3", "1.2.99", "1.3.0"},
        std::tuple{"~0.0.3", "0.0.2", "0.0.3", "0.0.99", "0.1.0"},
        std::tuple{"[1.2.3, *)", "1.2.2", "1.2.3", "999.0.0", ""},
        std::tuple{"(1.2.3, *)", "1.2.3", "1.2.4", "999.0.0", ""},
        std::tuple{"[1.2.3,2.0.0)", "1.2.2", "1.2.3", "1.9.9", "2.0.0"},
        std::tuple{"[1.2.3, 2.0.0]", "1.2.2", "1.2.3", "2.0.0", "2.0.1"},
        std::tuple{"(1.2.3, 2.0.0]", "1.2.3", "1.2.4", "2.0.0", "2.0.1"},
        std::tuple{"(1.2.3, 2.0.0)", "1.2.3", "1.2.4", "1.9.9", "2.0.0"},
        std::tuple{"(*, 2.0.0)", "", "0.0.0", "1.9.9", "2.0.0"},
        std::tuple{"(*, 2.0.0]", "", "0.0.0", "2.0.0", "2.0.1"},
        std::tuple{"[1.2.3, 1.2.3]", "1.2.2", "1.2.3", "1.2.3", "1.2.4"}}) {
    auto Requirement = take(parseRequirement(Text));
    require(
        Requirement.matches(take(parseVersion(First))) &&
            Requirement.matches(take(parseVersion(Inside))) &&
            (!*Below || !Requirement.matches(take(parseVersion(Below)))) &&
            (!*Outside || !Requirement.matches(take(parseVersion(Outside)))) &&
            !Requirement.matches(take(parseVersion("1.2.4-rc.1"))),
        std::string("Version range boundaries: ") + Text);
  }
  require(take(parseRequirement("^1.2.3+one"))
                  .matches(take(parseVersion("1.2.3+two"))) &&
              take(parseRequirement("^999999999999999999999.0.0"))
                  .matches(take(parseVersion("999999999999999999999.1.0"))),
          "Ranges use SemVer precedence without metadata identity or integer "
          "overflow");
  require(take(parseRequirement("[ 1.2.3 ,\t2.0.0 )"))
                  .matches(take(parseVersion("1.5.0"))) &&
              take(parseRequirement("(*, *)"))
                  .matches(take(parseVersion("999.0.0"))),
          "Interval whitespace and unbounded limits");
  for (const auto *Invalid :
       {"^", "~1.2", ">=1.2.3", ">1.2.3", "^1.2.3-rc.1", "[1.2.3]", "[,2.0.0)",
        "[1.2.3,)", "[*,2.0.0)", "[1.2.3,*]", "[2.0.0,1.0.0)", "[1.2.3,1.2.3)",
        "[1.2.3,2.0.0,3.0.0)", "[1.2.*,2.0.0)", "[1.2.3,2.0.0-rc.1)",
        "(1.2.3,2.0.0", "1.2.3|2.0.0", "^1.2.3 || ^2.0.0"})
    require(!parseRequirement(Invalid), "Invalid or unsupported range syntax");
  auto Candidate = [](std::string VersionText, std::string DependencyText) {
    PackageCandidate Value;
    Value.Package.Selected = take(parseVersion(VersionText));
    Value.Dependencies.push_back(
        {"b", take(parseRequirement(DependencyText)), "a -> b"});
    return Value;
  };
  PackageCatalog Catalog;
  Catalog.Packages["a"] = {Candidate("2.0.0", "^2.0.0"),
                           Candidate("1.0.0", "~1.5.0")};
  PackageCandidate B;
  B.Package.Selected = take(parseVersion("1.5.0"));
  Catalog.Packages["b"] = {B};
  std::vector<PackageRequirement> Requests = {
      {"a", take(parseRequirement("[1.0.0, *)")), "app -> a"},
      {"b", take(parseRequirement("^1.0.0")), "app -> b"}};
  auto Solved = take(resolvePackages(Requests, Catalog));
  require(Solved.Packages.at("a").Package.Selected.Text == "1.0.0",
          "Global resolution backtracks instead of choosing conflicting latest "
          "versions");
  Catalog.LockedVersions["a"] = "2.0.0";
  require(!resolvePackages(Requests, Catalog),
          "Build does not silently replace locked versions");
  Catalog.Unlocked.insert("a");
  require(bool(resolvePackages(Requests, Catalog)),
          "Explicitly unlocked packages may reselect");
  Catalog.Packages.at("a")[1].MetadataAvailable = false;
  auto Step = take(resolvePackageStep(Requests, Catalog));
  require(std::holds_alternative<PackageMetadataRequest>(Step) &&
              std::get<PackageMetadataRequest>(Step).Selected->Selected.Text ==
                  "1.0.0",
          "Lazy metadata is requested for the eligible backtracking candidate");
  auto Source =
      take(parseRegistrySource("https://GitHub.com/Owner/Registry.git/"));
  require(Source.Url == "https://github.com/owner/registry",
          "Source identity is canonical");
  require(!parseHttpsUrl("https://secret@example.com/x") &&
              !parseRegistrySource("https://host/path?query"),
          "Credential and identity boundaries reject ambiguous URLs");
  require(
      take(parseRegistrySource("http://EXAMPLE.com:80/index/")).Url ==
              "http://example.com/index" &&
          take(parseRegistrySource("file:///tmp/registry")).Url ==
              "file:///tmp/registry",
      "HTTP and local registry sources retain distinct canonical identities");
  require(!parseHttpsUrl("http://example.com/index") &&
              !parseRegistrySource("ssh://example.com/index") &&
              !parseRegistrySource("file://remote/index") &&
              !parseRegistrySource("file:///tmp/%00index"),
          "Registry transports preserve HTTPS-only callers and reject unsafe "
          "file URLs");
}

void buildPlanContract() {
  auto App = project("app", ProductType::Executable);
  App.Project.Base.Generators = {
      {{"obj/first.hpp"}, {"obj/second.hpp"}, {"generator"}, {}},
      {{"schema.idl"}, {"obj/first.hpp"}, {"generator"}, {}}};
  App.Project.Base.Libraries = {"obj/vendor.a"};
  auto Graph = take(buildGraph("app", {App}, Target::LinuxX64));
  auto Plan =
      take(planBuild(Graph, {Target::LinuxX64, Profile::Release, false}));
  require(std::ranges::find(Plan.Actions[0].Dependencies, std::size_t(1)) !=
              Plan.Actions[0].Dependencies.end(),
          "Forward generator references form producer edges");
  const auto &Link = Plan.Actions[Plan.Roots.front()];
  require(std::ranges::find(Link.Inputs, Path("projects/app/obj/vendor.a")) !=
              Link.Inputs.end(),
          "Explicit external archives are real link inputs for incrementality");
  auto Check = take(planBuild(Graph, {Target::LinuxX64, Profile::Debug, true}));
  require(std::ranges::none_of(Check.Actions,
                               [](const auto &Action) {
                                 return Action.Kind == ActionKind::Link;
                               }),
          "Check does not perform a final link");
  App.Project.Base.Generators[1].Inputs = {"obj/second.hpp"};
  Graph = take(buildGraph("app", {App}, Target::LinuxX64));
  require(!planBuild(Graph, {Target::LinuxX64, Profile::Debug, false}),
          "Generator cycles fail before executing external programs");
}
}

int main() try {
  configurationContract();
  graphAndModuleContract();
  resolutionContract();
  buildPlanContract();
  std::cout << "Core contract checks passed.\n";
  return 0;
} catch (const std::exception &Error) {
  std::cerr << Error.what() << '\n';
  return 1;
}
