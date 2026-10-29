module llm.runtime;

import :internal;
import std;

namespace llm {

Target nativeTarget() {
#if defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
  return Target::WindowsArm64;
#elif defined(_WIN32)
  return Target::WindowsX64;
#elif defined(__APPLE__) && defined(__aarch64__)
  return Target::MacosArm64;
#elif defined(__linux__) && defined(__aarch64__)
  return Target::LinuxArm64;
#elif defined(__linux__) && defined(__x86_64__)
  return Target::LinuxX64;
#else
#error "llm supports Windows/Linux x64/ARM64 and macOS ARM64"
#endif
}

bool executableTarget(Target Value) {
  auto Host = nativeTarget();
  if (isArm64Target(Value) != isArm64Target(Host))
    return false;
  if (isWindowsTarget(Host))
    return isWindowsTarget(Value);
  return Value == Host;
}

namespace runtime {
namespace {
std::string projectIdentity(const Path &Directory) {
  return collisionKey(std::filesystem::canonical(Directory));
}
}

Path projectRoot(const Context &Call, const Path &Requested) {
  auto Root = std::filesystem::canonical(
      Requested.empty()         ? Call.InvocationDirectory
      : Requested.is_absolute() ? Requested
                                : Call.InvocationDirectory / Requested);
  for (;;) {
    if (std::filesystem::is_regular_file(Root / ".llm"))
      return Root;
    if (!Requested.empty() || Root == Root.parent_path())
      throw Failure("No .llm found in the selected project");
    Root = Root.parent_path();
  }
}

std::vector<Path> sourceInventory(const Path &Root) {
  std::vector<Path> Result;
  for (std::filesystem::recursive_directory_iterator Entry(Root), End;
       Entry != End; ++Entry) {
    auto File = Entry->path();
    auto Relative = File.lexically_relative(Root);
    auto First = *Relative.begin();
    if (First == "obj" || First == "bin" || First == ".git" ||
        redirectsPath(File)) {
      Entry.disable_recursion_pending();
      continue;
    }
    if (Entry->is_directory() &&
        std::filesystem::is_regular_file(File / ".llm")) {
      Entry.disable_recursion_pending();
      continue;
    }
    if (Entry->is_regular_file())
      Result.push_back(std::move(Relative));
  }
  std::ranges::sort(Result);
  return Result;
}

ProjectGraph loadGraph(Context &Call, const Manifest &Project,
                       Target BuildTarget,
                       const std::map<std::string, Path> &Packages,
                       bool AllowUnresolved) {
  std::map<std::string, ProjectInput> Inputs;
  std::set<std::string> Visiting;
  auto RootIdentity = projectIdentity(Project.Root);
  auto Visit = [&](auto &&Self, const Path &Directory,
                   const Path &Instance) -> std::string {
    cancelled(Call);
    auto Canonical = std::filesystem::canonical(Directory);
    auto Identity = projectIdentity(Canonical);
    if (Visiting.contains(Identity))
      throw Failure("Dependency cycle: " + Identity);
    if (Inputs.contains(Identity))
      return Identity;
    if (Inputs.size() >= 4096)
      throw Failure("Dependency graph exceeds 4096 projects");
    Visiting.insert(Identity);
    ProjectInput Input;
    Input.Identity = Identity;
    Input.Project =
        Identity == RootIdentity ? Project : readManifest(Canonical);
    Input.Project.Root = Canonical;
    Input.RegistryInstance = Instance;
    Input.Imported = readImportedProduct(Canonical, BuildTarget);
    Input.Inventory = sourceInventory(Canonical);
    auto Settings = take(effectiveSettings(Input.Project, BuildTarget));
    auto References = [&](const Strings &Values, bool Public) {
      for (const auto &Text : Values) {
        auto Key = Public ? "dependencies.public" : "dependencies.private";
        SourceLocation Location{Canonical / ".llm", 1, 1};
        if (auto Found = Input.Project.Origins.find(Key);
            Found != Input.Project.Origins.end())
          Location = Found->second;
        auto Reference =
            take(dependencyReference(Input.Project, Text, Public, Location));
        Path ProviderRoot, ProviderInstance = Instance;
        if (Reference.Local) {
          ProviderRoot = std::filesystem::canonical(*Reference.Local);
          if (!Instance.empty()) {
            auto Relative = ProviderRoot.lexically_relative(Instance);
            if (Relative != "." && !isRelativeSafe(Relative))
              throw Failure(Diagnostic{
                  "Registry package local dependency escapes its archive",
                  Location});
          }
        } else {
          auto Source = take(parseRegistrySource(Reference.Registry));
          auto Found =
              Packages.find(packageIdentity(Source, Reference.Package));
          if (Found == Packages.end() && AllowUnresolved) {
            Input.Complete = false;
            continue;
          }
          if (Found == Packages.end())
            throw Failure(
                Diagnostic{"Unresolved registry package: " + Text, Location});
          ProviderRoot = Found->second;
          ProviderInstance = ProviderRoot;
        }
        Input.Dependencies.push_back(
            {Self(Self, ProviderRoot, ProviderInstance), Public});
      }
    };
    References(Settings.Dependencies.Private, false);
    References(Settings.Dependencies.Public, true);
    Inputs.emplace(Identity, std::move(Input));
    Visiting.erase(Identity);
    return Identity;
  };
  Visit(Visit, Project.Root, {});
  std::vector<ProjectInput> Projects;
  for (auto &[Identity, Input] : Inputs)
    Projects.push_back(std::move(Input));
  return take(buildGraph(RootIdentity, std::move(Projects), BuildTarget));
}

}
}

namespace llm {
namespace {
void newProduct(Context &Call, const Command &Request) {
  using namespace runtime;
  namespace fs = std::filesystem;
  auto Root = Request.Project.empty() ? Call.InvocationDirectory
              : Request.Project.is_absolute()
                  ? Request.Project
                  : Call.InvocationDirectory / Request.Project;
  Root = fs::absolute(Root).lexically_normal();
  if (Root.filename().empty())
    Root = Root.parent_path();
  rejectSymlinkAncestors(Root);
  auto Name = pathText(Root.filename());
  if (!isProductName(Name) || Name.starts_with('-'))
    throw Failure("Project directory must have a portable product name");
  fs::create_directories(Root);
  rejectSymlinkAncestors(Root / "src");
  if (fs::exists(Root / "src") && !fs::is_directory(Root / "src"))
    throw Failure("Project src path must be a directory");
  auto Write = [&](const Path &File, std::string_view Text) {
    rejectSymlinkAncestors(File);
    std::ofstream Output(File, std::ios::binary | std::ios::out |
                                   std::ios::noreplace);
    if (!Output)
      throw Failure("Refusing to overwrite existing file: " + pathText(File));
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
    Output.close();
    if (!Output)
      throw Failure("Cannot finish new file: " + pathText(File));
  };
  Write(Root / ".llm",
        "type = " + jsonString(productTypeName(Request.NewType)) + '\n');
  if (!fs::exists(fs::symlink_status(Root / ".gitignore")))
    Write(Root / ".gitignore", "/obj/\n/bin/\n");
  fs::create_directories(Root / "src");
  emit(Call, EventKind::Progress, "Created empty project " + Name);
}
}

Result<int> execute(Context &Call, const Command &Request) {
  using namespace runtime;
  namespace fs = std::filesystem;
  try {
    enableCancellation();
    cancelled(Call);
    if (Call.Executable.empty())
      Call.Executable = executablePath();
    if (Call.InvocationDirectory.empty())
      Call.InvocationDirectory = fs::current_path();
    if (Request.Action == Operation::Install ||
        Request.Action == Operation::Uninstall ||
        Request.Action == Operation::CheckInstallation)
      return installationCommand(Call, Request);
    if (!executableTarget(Request.BuildTarget) &&
        (Request.Action == Operation::Run ||
         Request.Action == Operation::Debug ||
         Request.Action == Operation::Test ||
         Request.Action == Operation::Analyze)) {
      auto Name = Request.Action == Operation::Debug  ? "debug"
                  : Request.Action == Operation::Run  ? "run"
                  : Request.Action == Operation::Test ? "test"
                                                      : "analyze";
      throw Failure(std::string(Name) + " requires the native target " +
                    std::string(targetName(nativeTarget())) + ". Selected " +
                    std::string(targetName(Request.BuildTarget)) +
                    ". Use llm build --target " +
                    std::string(targetName(Request.BuildTarget)) +
                    " to build for this target.");
    }
    if (Request.Action == Operation::New) {
      newProduct(Call, Request);
      return 0;
    }
    auto Root = projectRoot(Call, Request.Project);
    if (Request.Action == Operation::Clean) {
      rejectSymlinkAncestors(Root / "obj");
      fs::create_directories(Root / "obj");
      FileLock Lock(Call, Root / "obj/.guard", true);
      removeOutputs(Root, readOwned(Root));
      saveOwned(Root, {});
      emit(Call, EventKind::Progress, "Clean finished.");
      return 0;
    }
    auto Project = readManifest(Root);
    Path Debugger;
    if (Request.Action == Operation::Debug) {
      if (Request.Mode != Profile::Debug || Request.NoBuild)
        throw Failure(
            "debug requires a Debug build and does not accept --no-build");
      if (Project.Type != ProductType::Executable)
        throw Failure("debug requires type = 'executable'");
      auto Tools = Call.Toolchain ? *Call.Toolchain : installedToolchain(Call);
      Debugger = debuggerPath(Tools);
    }
    if (Request.Action == Operation::Build ||
        Request.Action == Operation::Rebuild ||
        Request.Action == Operation::Check ||
        Request.Action == Operation::Package ||
        Request.Action == Operation::Debug ||
        (Request.Action == Operation::Run && !Request.NoBuild)) {
      auto Tools = Call.Toolchain ? *Call.Toolchain : installedToolchain(Call);
      targetToolchain(Tools, Request.BuildTarget);
    }
    switch (Request.Action) {
    case Operation::Add:
    case Operation::Remove:
    case Operation::Update:
    case Operation::Login:
    case Operation::Logout:
      return registryCommand(Call, Request, Project);
    case Operation::Test:
      return testProject(Call, Request, Project);
    case Operation::Format:
    case Operation::Analyze:
    case Operation::List:
      return projectTools(Call, Request, Project);
    case Operation::Package:
      return packageProject(Call, Request, Project);
    default:
      break;
    }
    if (Request.Action == Operation::Run &&
        Project.Type != ProductType::Executable)
      throw Failure("run requires type = 'executable'");
    Path Executable =
        Root / "bin" / pathFromText(targetName(Request.BuildTarget)) /
        (Request.Mode == Profile::Release ? "release" : "debug") /
        artifactName(Project.Name, Project.Type, Request.BuildTarget);
    if (!(Request.Action == Operation::Run && Request.NoBuild)) {
      auto Owner = Root;
      std::vector<Manifest> Roots{Project};
      auto Packages = resolveDependencies(Call, Request, Owner, Roots);
      auto Graph = loadGraph(Call, Project, Request.BuildTarget, Packages);
      if (Request.Action == Operation::Rebuild) {
        FileLock Lock(Call, Root / "obj/.guard", true);
        auto Owned = readOwned(Root);
        OwnedOutputs Selected;
        auto TargetDirectory =
            std::string(targetName(Request.BuildTarget)) + '/' +
            (Request.Mode == Profile::Release ? "release" : "debug") + '/';
        std::erase_if(Owned, [&](const Path &File) {
          auto Text = pathText(File);
          if (!Text.starts_with("bin/" + TargetDirectory) &&
              !Text.starts_with("obj/" + TargetDirectory))
            return false;
          Selected.insert(File);
          return true;
        });
        removeOutputs(Root, Selected);
        saveOwned(Root, Owned);
      }
      Executable = buildProject(Call, Request, Graph).Executable;
    }
    if (Request.Action == Operation::Debug) {
      auto Environment =
          debuggerEnvironment(Debugger.parent_path().parent_path());
      Strings Arguments;
#ifdef __linux__
      Arguments.emplace_back("--source-quietly");
      for (const auto &Entry : Environment) {
        const auto &Name = Entry.first;
        auto Value = std::getenv(Name.c_str());
        Arguments.emplace_back("--one-line-before-file");
        Arguments.emplace_back(
            Value ? "settings append target.env-vars " +
                        quoteArgument(Name + '=' + Value)
                  : "settings append target.unset-env-vars " + Name);
      }
#endif
      Arguments.emplace_back("--");
      Arguments.emplace_back(pathText(Executable));
      Arguments.insert(Arguments.end(), Request.Arguments.begin(),
                       Request.Arguments.end());
      return runProcess(Call, Debugger, Arguments,
                        {.Directory = Call.InvocationDirectory,
                         .Interactive = true,
                         .Environment = std::move(Environment)})
          .ExitCode;
    }
    if (Request.Action == Operation::Run) {
      rejectSymlinkAncestors(Executable);
      if (!fs::is_regular_file(Executable))
        throw Failure("Executable is missing. Run llm build");
      return runProcess(Call, Executable, Request.Arguments,
                        {.Directory = Call.InvocationDirectory})
          .ExitCode;
    }
    return 0;
  } catch (const BuildFailure &Error) {
    if (Error.ExitCode)
      return Error.ExitCode;
    return std::unexpected(Error.Error);
  } catch (const Failure &Error) {
    return std::unexpected(Error.Error);
  } catch (const std::exception &Error) {
    return std::unexpected(Diagnostic{Error.what()});
  }
}
}
