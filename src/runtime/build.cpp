module llm.runtime;
import :internal;
import std;
namespace llm::runtime {
namespace fs = std::filesystem;
namespace {
std::string ninjaText(std::string_view Text, bool Path = false) {
  if (Text.find_first_of("\r\n\0", 0, 3) != std::string_view::npos)
    throw Failure("Invalid build graph argument");
  std::string Result;
  for (char C : Text) {
    if (C == '$' || (Path && (C == ' ' || C == ':')))
      Result += '$';
    Result += C;
  }
  return Result;
}
std::string ninjaPath(const Path &File) {
  return ninjaText(pathText(File), true);
}
Path suffix(Path File, std::string_view Text) {
  File += pathFromText(Text);
  return File;
}
struct BuildSession {
  Context &Call;
  const Command &Request;
  const ProjectGraph &Graph;
  Path Root, Toolchain, Objects, ManifestFile;
  BuildPlan Plan;
  std::set<std::string> Products;
  std::map<Path, OwnedOutputs> Ownership, Reserved;
  std::map<std::string, std::string> ArgumentGroups;
  ModulePlan Modules;
  struct StandardUnit {
    Path Source, Object, Bmi;
    Strings Arguments;
    std::optional<Path> Imported;
  };
  std::map<Path, StandardUnit> Standards;
  std::map<Path, Strings> Commands;
  BuildSession(Context &Call, const Command &Request, const ProjectGraph &Graph,
               Path Owner = {}, std::set<std::string> Products = {})
      : Call(Call), Request(Request), Graph(Graph),
        Root(Owner.empty() ? Graph.Projects.at(Graph.Root).Input.Project.Root
                           : Owner),
        Toolchain(Call.Toolchain ? fs::canonical(*Call.Toolchain)
                                 : installedToolchain(Call)),
        Objects(Path("obj") / pathFromText(targetName(Request.BuildTarget)) /
                (Request.Mode == Profile::Release ? "release" : "debug")),
        ManifestFile(Objects / (Request.Action == Operation::Check
                                    ? "check.ninja"
                                    : "build.ninja")),
        Plan(take(planBuild(Graph, {Request.BuildTarget, Request.Mode,
                                    Request.Action == Operation::Check, Owner,
                                    Products}))),
        Products(std::move(Products)) {
    Ownership.emplace(Root, readOwned(Root));
    targetToolchain(Toolchain, Request.BuildTarget);
    for (const auto &[Id, Node] : Graph.Projects) {
      Ownership.try_emplace(Node.Input.Project.Root,
                            readOwned(Node.Input.Project.Root));
      Modules.Visible.emplace(Id, Node.Compile.VisibleProjects);
      if (Node.Input.Imported &&
          Node.Input.Imported->ToolchainIdentity !=
              toolchainIdentity(Toolchain, Request.BuildTarget))
        throw Failure(
            "Development package requires its original pinned toolchain: " +
            Node.Input.Project.Name);
    }
    Modules.Root = Root;
    Modules.Dyndep =
        Objects /
        (Request.Action == Operation::Check ? "check.dd" : "build.dd");
  }
  void reserve(const Path &Owner, const Path &File) {
    cancelled(Call);
    if (Reserved[Owner].contains(File))
      return;
    checkOutput(Owner, File);
    auto &Owned = Ownership.at(Owner);
    if (fs::exists(nativePath(Owner / File)) && !Owned.contains(File))
      throw Failure("Refusing to overwrite foreign output: " +
                    pathText(Owner / File));
    Owned.insert(File);
    fs::create_directories(nativePath((Owner / File).parent_path()));
    Reserved[Owner].insert(File);
  }
  Path output(const Path &File) {
    if (File.is_absolute()) {
      for (const auto &[Owner, Owned] : Ownership) {
        auto Relative = File.lexically_relative(Owner);
        if (ownedPath(Relative)) {
          reserve(Owner, Relative);
          return File;
        }
      }
      throw Failure("Output has no owning project: " + pathText(File));
    }
    reserve(Root, File);
    return Root / File;
  }
  void saveOwnership() {
    for (const auto &[Owner, Owned] : Ownership)
      if (!Owned.empty() || fs::exists(Owner / "obj/.owned"))
        saveOwned(Owner, Owned);
  }

  std::string argumentText(std::ostringstream &Text, const Strings &Arguments,
                           bool Archive = false, bool Group = true) {
    auto Encode = [&](std::string_view Argument) {
#ifndef _WIN32
      if (Archive) {
        std::string Result = "\"";
        for (char Byte : Argument) {
          if (Byte == '\\' || Byte == '"')
            Result += '\\';
          Result += Byte;
        }
        return ninjaText(Result + '"');
      }
#endif
      return ninjaText(responseArgument(Argument));
    };
    std::size_t Prefix = 0;
    if (Group && !Archive)
      while (Prefix < Arguments.size() && Arguments[Prefix] != "-MMD" &&
             Arguments[Prefix] != "-MD" &&
             !Arguments[Prefix].starts_with("-fmodule-file=") &&
             !Arguments[Prefix].starts_with("-fmodule-output="))
        ++Prefix;
    std::string Result;
    if (Prefix) {
      std::string Value;
      for (std::size_t Index = 0; Index < Prefix; ++Index)
        Value += (Value.empty() ? "" : " ") + Encode(Arguments[Index]);
      auto [Entry, Added] = ArgumentGroups.try_emplace(
          Value, "flags" + std::to_string(ArgumentGroups.size()));
      if (Added)
        Text << Entry->second << " = " << Value << '\n';
      Result = "$" + Entry->second;
    }
    for (std::size_t Index = Prefix; Index < Arguments.size(); ++Index)
      Result += (Result.empty() ? "" : " ") + Encode(Arguments[Index]);
    return Result;
  }

  std::string rule(std::ostringstream &Text, const Path &Object,
                   const Path &Program, const Strings &Arguments,
                   std::string_view Description, bool Dependencies = false,
                   bool Archive = false) {
    output(Object);
    auto Work = Object.is_absolute()
                    ? Objects / "projects" /
                          moduleKey(pathText(Object.parent_path())) /
                          Object.filename()
                : *Object.begin() == "bin"
                    ? Objects / "projects/root" / Object.filename()
                    : Object;
    auto Response = suffix(Work, ".rsp");
    output(Response);
    if (Archive)
      output(suffix(Work, ".tmp"));
    auto Flags = argumentText(Text, Arguments, Archive, Dependencies);
    std::ostringstream Binding;
    if (!Archive)
      Binding << "  program = "
              << (Program == compilerPath(Toolchain, false) ? "$cc"
                  : Program == compilerPath(Toolchain, true)
                      ? "$cxx"
                      : ninjaText(quoteArgument(pathText(Program))))
              << '\n';
    Binding << "  response = " << ninjaPath(Response) << "\n  response_arg = "
            << ninjaText(
                   quoteArgument((Archive ? "" : "@") + pathText(Response)))
            << "\n  arguments = " << Flags
            << "\n  label = " << ninjaText(Description) << '\n';
    if (Dependencies) {
      auto Dep = suffix(Object, ".d");
      output(Dep);
      Binding << "  dependency_file = " << ninjaPath(Dep) << '\n';
    }
    return Binding.str();
  }

  void generatorRule(std::ostringstream &Text, std::size_t Index) {
    const auto &Action = Plan.Actions[Index];
    const auto &Owner = Graph.Projects.at(Action.Project).Input.Project.Root;
    const auto &Generator = std::get<llm::Generator>(Action.Details);
    auto Program = pathFromText(Generator.Command.front());
    if (!Program.is_absolute())
      Program = Owner / Program;
    bool Generated = std::ranges::any_of(Plan.Actions, [&](const auto &Other) {
      return Other.Kind == ActionKind::Generate &&
             std::ranges::find(Other.Outputs, Program.lexically_normal()) !=
                 Other.Outputs.end();
    });
    if (!Generated)
      Program = findProgram(Owner, Generator.Command.front());
    auto Response = Objects / "generate" / (std::to_string(Index) + ".rsp");
    output(Response);
    Text << "rule generate" << Index << "\n  command = "
         << ninjaText(quoteArgument(pathText(Call.Executable)))
         << " --internal-generate "
         << ninjaText(quoteArgument(pathText(Response)))
         << "\n  rspfile = " << ninjaPath(Response) << "\n  rspfile_content = "
         << ninjaText(responseArgument(pathText(Owner)));
    for (const auto &File : Action.Outputs) {
      reserve(Owner, File.lexically_relative(Owner));
      Text << " --output " << ninjaText(responseArgument(pathText(File)));
    }
    Text << " -- " << ninjaText(responseArgument(pathText(Program)));
    for (const auto &Argument : Generator.Command | std::views::drop(1))
      Text << ' ' << ninjaText(responseArgument(Argument));
    Text << "\n  restat = 1\n  description = Generating "
         << ninjaText(
                pathText(Action.Outputs.front().lexically_relative(Owner)))
         << "\nbuild";
    for (const auto &File : Action.Outputs)
      Text << ' ' << ninjaPath(File);
    Text << ": generate" << Index;
    for (const auto &File : Action.Inputs)
      Text << ' ' << ninjaPath(File);
    Text << " | " << ninjaPath(Program) << ' ' << ninjaPath(Call.Executable)
         << '\n';
  }

  std::pair<Path, Path> standard(const CompileUnit &Consumer) {
    CompileUnit Unit;
    Unit.SourceLanguage = Language::Cxx;
    Unit.LanguageStandard = Consumer.LanguageStandard;
    Unit.Arguments = Consumer.Arguments;
    ProjectNode Node;
    Node.Input.Project.Type = ProductType::StaticLibrary;
    auto Arguments = compileArguments(Unit, Node, Plan.Context, Toolchain);
    std::string Key;
    for (const auto &A : Arguments)
      Key += responseArgument(A) + "\n";
    auto Directory = Objects / "std" / moduleKey(Key);
    auto TargetRoot = targetToolchain(Toolchain, Request.BuildTarget);
    for (const auto *Name : {"std", "std.compat"}) {
      auto Bmi = Directory / (std::string(Name) + ".pcm");
      if (Standards.contains(Bmi))
        continue;
      StandardUnit S;
      S.Bmi = Bmi;
      S.Object = Directory / (std::string(Name) + ".o");
      S.Source = TargetRoot / "share/libc++/v1" / (std::string(Name) + ".cppm");
      if (!fs::is_regular_file(nativePath(S.Source)))
        throw Failure("Standard module source is missing: " +
                      pathText(S.Source));
      S.Arguments = Arguments;
      S.Arguments.insert(S.Arguments.end(),
                         {"-Wno-reserved-module-identifier",
                          "-Wno-include-angled-in-module-purview", "-x",
                          "c++-module"});
      if (std::string_view(Name) == "std.compat") {
        S.Imported = Directory / "std.pcm";
        S.Arguments.push_back("-fmodule-file=std=" +
                              pathText(Root / *S.Imported));
      }
      S.Arguments.insert(S.Arguments.end(),
                         {"-MD", "-MF", pathText(suffix(S.Object, ".d")), "-MQ",
                          pathText(S.Object), "-c", pathText(S.Source),
                          "-fmodule-output=" + pathText(S.Bmi), "-o",
                          pathText(S.Object)});
      Standards.emplace(Bmi, std::move(S));
    }
    return {Directory / "std.pcm", Directory / "std.compat.pcm"};
  }
  void generatedInputs(std::ostringstream &Text, const std::string &Project) {
    const auto &Visible = Graph.Projects.at(Project).Compile.VisibleProjects;
    for (const auto &A : Plan.Actions)
      if (A.Kind == ActionKind::Generate && Visible.contains(A.Project))
        for (const auto &File : A.Outputs)
          Text << ' ' << ninjaPath(File);
  }
  void inputs(std::ostringstream &Text, const Strings &Values) {
    for (const auto &V : Values)
      Text << ' ' << ninjaText(V, true);
  }
  std::string ninjaGraph(BuildDetails Details) {
    std::ostringstream Text;
    auto Tool = [&](std::string Name) {
      auto File = Toolchain / "bin" / pathFromText(Name);
#ifdef _WIN32
      File += ".exe";
#endif
      if (!fs::is_regular_file(nativePath(File)))
        throw Failure("Required host tool is missing: " + pathText(File));
      return File;
    };
    Path Scanner;
    if (std::ranges::any_of(Plan.Actions, [](const auto &A) {
          return A.Kind == ActionKind::Compile &&
                 std::get<CompileUnit>(A.Details).SourceLanguage ==
                     Language::Cxx &&
                 std::get<CompileUnit>(A.Details).LanguageStandard >=
                     Standard::Cxx20;
        }))
      Scanner = Tool("clang-scan-deps");
    Tool("ninja");
    Tool("llvm-ar");
    Text << "builddir = " << ninjaPath(Objects)
         << "\nllm = " << ninjaText(quoteArgument(pathText(Call.Executable)))
         << "\ncc = "
         << ninjaText(quoteArgument(pathText(compilerPath(Toolchain, false))))
         << "\ncxx = "
         << ninjaText(quoteArgument(pathText(compilerPath(Toolchain, true))))
         << "\ntools = " << ninjaText(quoteArgument(pathText(Toolchain)))
         << "\ntarget = " << targetName(Request.BuildTarget)
         << "\nrule compile\n  command = $program --rsp-quoting=windows "
            "$response_arg\n  rspfile = $response\n  rspfile_content = "
            "$arguments\n  depfile = $dependency_file\n  deps = gcc\n  "
            "description = $label"
         << "\nrule command\n  command = $program --rsp-quoting=windows "
            "$response_arg$post_link\n  rspfile = $response\n  rspfile_content "
            "= "
            "$arguments\n  description = $label"
         << "\nrule archive\n  command = $llm --internal-archive $out "
            "$response_arg $tools $target\n  rspfile = $response\n  "
            "rspfile_content = $arguments\n  description = $label"
         << "\nrule scan\n  command = "
         << ninjaText(quoteArgument(pathText(Scanner)))
         << " -format=p1689 -o $out -- $cxx --rsp-quoting=windows "
            "$response_arg\n  rspfile = $response\n  rspfile_content = "
            "$arguments\n  depfile = $dependency_file\n  deps = gcc\n  "
            "description = $label"
         << "\nrule collate\n  command = $llm --internal-collate $in\n  restat "
            "= 1\n  description = Resolving module dependencies\n";
    auto Descriptor = Objects / (Plan.Context.CheckOnly ? "check.modules.rsp"
                                                        : "build.modules.rsp");
    for (std::size_t Index = 0; Index < Plan.Actions.size(); ++Index) {
      const auto &A = Plan.Actions[Index];
      if (A.Kind != ActionKind::Compile)
        continue;
      const auto &Unit = std::get<CompileUnit>(A.Details);
      if (Unit.SourceLanguage != Language::Cxx ||
          (Unit.LanguageStandard != Standard::Cxx20 &&
           Unit.LanguageStandard != Standard::Cxx23))
        continue;
      auto [Std, Compat] = standard(Unit);
      ModuleUnit M{A.Project,
                   Unit.Source,
                   A.Outputs.front(),
                   suffix(A.Outputs.front(), ".pcm"),
                   suffix(A.Outputs.front(), ".scan.json"),
                   suffix(A.Outputs.front(), ".modules.rsp"),
                   Std,
                   Compat};
      for (const auto &File : {M.Bmi, M.Scan, M.Flags, suffix(M.Scan, ".d"),
                               suffix(M.Scan, ".rsp")})
        output(File);
      auto Args = compileArguments(Unit, Graph.Projects.at(A.Project),
                                   Plan.Context, Toolchain);
      Args.insert(Args.end(),
                  {"-MD", "-MF", pathText(suffix(M.Scan, ".d")), "-MT",
                   pathText(M.Scan), "-c", pathText(Unit.Source), "-o",
                   pathText(M.Object)});
      auto Flags = argumentText(Text, Args);
      Text << "build " << ninjaPath(M.Scan) << ": scan "
           << ninjaPath(Unit.Source) << " | " << ninjaPath(Scanner) << ' '
           << ninjaPath(compilerPath(Toolchain, true)) << ' '
           << ninjaPath(compilerConfig(Toolchain, Request.BuildTarget, true));
      for (const auto &File : A.Inputs | std::views::drop(1))
        Text << ' ' << ninjaPath(File);
      generatedInputs(Text, A.Project);
      Text << "\n  response = " << ninjaPath(suffix(M.Scan, ".rsp"))
           << "\n  response_arg = "
           << ninjaText(quoteArgument("@" + pathText(suffix(M.Scan, ".rsp"))))
           << "\n  arguments = " << Flags
           << "\n  dependency_file = " << ninjaPath(suffix(M.Scan, ".d"))
           << "\n  label = Scanning " << ninjaText(pathText(Unit.Source))
           << "\n";
      Modules.Units.push_back(std::move(M));
    }
    for (const auto &[Bmi, S] : Standards) {
      auto Binding =
          rule(Text, S.Object, compilerPath(Toolchain, true), S.Arguments,
               "Compiling " + pathText(S.Source.filename()), true);
      output(Bmi);
      Text << "build " << ninjaPath(S.Object) << " | " << ninjaPath(Bmi)
           << ": compile " << ninjaPath(S.Source) << " | "
           << ninjaPath(compilerPath(Toolchain, true)) << ' '
           << ninjaPath(compilerConfig(Toolchain, Request.BuildTarget, true));
      if (S.Imported)
        Text << ' ' << ninjaPath(*S.Imported);
      Text << '\n' << Binding;
    }
    for (std::size_t Index = 0; Index < Plan.Actions.size(); ++Index) {
      const auto &A = Plan.Actions[Index];
      if (A.Kind == ActionKind::Generate) {
        generatorRule(Text, Index);
        continue;
      }
      const auto &Node = Graph.Projects.at(A.Project);
      const auto &Object = A.Outputs.front();
      Strings Args;
      bool Deps = false;
      Path ModuleFlags;
      auto M = std::ranges::find_if(
          Modules.Units, [&](const auto &U) { return U.Object == Object; });
      Path Program;
      if (A.Kind == ActionKind::Compile) {
        const auto &Unit = std::get<CompileUnit>(A.Details);
        Program = compilerPath(Toolchain, Unit.SourceLanguage == Language::Cxx);
        Args = compileArguments(Unit, Node, Plan.Context, Toolchain);
        Deps = !(Unit.SourceLanguage == Language::Assembly &&
                 Unit.Source.extension() == ".s");
        if (M != Modules.Units.end()) {
          ModuleFlags = M->Flags;
          Args.push_back("@" + pathText(ModuleFlags));
        }
        if (Deps)
          Args.insert(Args.end(),
                      {"-MMD", "-MF", pathText(suffix(Object, ".d")), "-MQ",
                       pathText(Object)});
        Args.insert(Args.end(),
                    {"-c", pathText(Unit.Source), "-o", pathText(Object)});
        if (Details != BuildDetails::None)
          Commands.emplace(Object, Args);
      } else if (A.Kind == ActionKind::Archive) {
        Program = Tool("llvm-ar");
        for (const auto &File : A.Inputs)
          Args.push_back(pathText(File));
      } else {
        Program = compilerPath(Toolchain, linkUsesCxx(Graph, A.Project));
        Args = linkArguments(A, Graph, Plan.Context, Toolchain, {});
        if (!Modules.Units.empty()) {
          ModuleFlags = suffix(Object, ".modules.rsp");
          if (Object.is_absolute() || *Object.begin() == "bin")
            ModuleFlags =
                Objects / "projects/root" /
                pathFromText(moduleKey(pathText(Object)) + ".modules.rsp");
          ModuleLink L{Object, ModuleFlags, {A.Project}};
          for (const auto &Id : Node.LinkOrder)
            if (Graph.Projects.at(Id).Input.Project.Type !=
                ProductType::SharedLibrary)
              L.Projects.insert(Id);
          Modules.Links.push_back(std::move(L));
          output(ModuleFlags);
          Args.push_back("@" + pathText(ModuleFlags));
        }
      }
      Path Entitlements;
#ifdef __APPLE__
      if (A.Kind == ActionKind::Link && Request.Mode == Profile::Debug &&
          Request.BuildTarget == Target::MacosArm64 &&
          (Node.Input.Project.Type == ProductType::Executable ||
           Node.Input.Project.Type == ProductType::Test)) {
        Entitlements = output(Objects / "debug.entitlements");
        atomicWrite(Entitlements,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<plist version=\"1.0\"><dict>"
                    "<key>com.apple.security.get-task-allow</key><true/>"
                    "</dict></plist>\n");
      }
#endif
      auto Binding = rule(
          Text, Object, Program, Args,
          A.Kind == ActionKind::Compile
              ? "Compiling " + pathText(std::get<CompileUnit>(A.Details).Source)
          : A.Kind == ActionKind::Archive
              ? "Archiving " + Node.Input.Project.Name
              : "Linking " + Node.Input.Project.Name,
          Deps, A.Kind == ActionKind::Archive);
      Text << "build " << ninjaPath(Object);
      if (A.Outputs.size() > 1) {
        Text << " |";
        for (const auto &File : A.Outputs | std::views::drop(1)) {
          output(File);
          Text << ' ' << ninjaPath(File);
        }
      }
      Text << ": "
           << (A.Kind == ActionKind::Archive ? "archive"
               : Deps                        ? "compile"
                                             : "command");
      for (const auto &File : A.Inputs)
        Text << ' ' << ninjaPath(File);
      Text << " | " << ninjaPath(Program) << ' '
           << ninjaPath(compilerConfig(
                  Toolchain, Request.BuildTarget,
                  A.Kind == ActionKind::Compile
                      ? std::get<CompileUnit>(A.Details).SourceLanguage ==
                            Language::Cxx
                      : linkUsesCxx(Graph, A.Project)));
      if (!Entitlements.empty())
        Text << ' ' << ninjaPath(Entitlements) << " /usr/bin/codesign";
      if (A.Kind == ActionKind::Compile)
        generatedInputs(Text, A.Project);
      if (!ModuleFlags.empty())
        Text << ' ' << ninjaPath(ModuleFlags);
      if (A.Kind == ActionKind::Archive)
        Text << ' ' << ninjaPath(Call.Executable);
      if (!ModuleFlags.empty())
        Text << " || " << ninjaPath(Modules.Dyndep);
      Text << '\n' << Binding;
      if (A.Kind == ActionKind::Link && isWindowsTarget(Request.BuildTarget) &&
          Graph.Projects.at(A.Project).Input.Project.Type ==
              ProductType::SharedLibrary)
        Text << "  restat = 1\n";
      if (!Entitlements.empty())
        Text << "  post_link =  && /usr/bin/codesign --force --sign - "
                "--entitlements "
             << ninjaText(quoteArgument(pathText(Entitlements))) << ' '
             << ninjaText(quoteArgument(pathText(Object))) << '\n';
      if (!ModuleFlags.empty())
        Text << "  dyndep = " << ninjaPath(Modules.Dyndep) << '\n';
    }
    std::set<Path> RuntimeOutputs;
    struct RuntimeCopy {
      Path Destination;
      std::set<Path> Sources;
    };
    std::map<std::string, RuntimeCopy> RuntimeSources;
    if (!Plan.Context.CheckOnly)
      for (const auto &[Identity, Node] : Graph.Projects) {
        auto Artifact = Plan.Artifacts.find(Identity);
        if (Artifact == Plan.Artifacts.end() ||
            Node.Input.Project.Type == ProductType::StaticLibrary ||
            Node.Input.Project.Type == ProductType::HeaderOnlyLibrary)
          continue;
        for (const auto &Runtime : Node.Runtimes) {
          auto Source = Plan.Artifacts.at(Runtime);
          auto Destination = Artifact->second.parent_path() / Source.filename();
          if (Source == Destination)
            continue;
          auto [Entry, Added] = RuntimeSources.try_emplace(
              collisionKey(Destination), RuntimeCopy{Destination, {}});
          Entry->second.Sources.insert(Source);
        }
      }
    for (const auto &[Identity, Copy] : RuntimeSources) {
      const auto &Destination = Copy.Destination;
      auto Base = Objects / "projects/root" /
                  pathFromText(moduleKey(pathText(Destination)) + "-" +
                               pathText(Destination.filename()));
      auto Temporary = suffix(Base, ".tmp"), Response = suffix(Base, ".rsp");
      output(Destination);
      output(Temporary);
      output(Response);
      auto Name = "runtime" + std::to_string(RuntimeOutputs.size());
      Text << "rule " << Name << "\n  command = "
           << ninjaText(quoteArgument(pathText(Call.Executable)))
           << " --internal-copy "
           << ninjaText(quoteArgument(pathText(Response)))
           << "\n  rspfile = " << ninjaPath(Response)
           << "\n  rspfile_content = "
           << ninjaText(responseArgument(pathText(Destination))) << ' '
           << ninjaText(responseArgument(pathText(Temporary)));
      for (const auto &Source : Copy.Sources)
        Text << ' ' << ninjaText(responseArgument(pathText(Source)));
      Text << "\n  restat = 1\n  description = Copying "
           << ninjaText(pathText(Destination.filename())) << "\nbuild "
           << ninjaPath(Destination) << ": " << Name;
      for (const auto &Source : Copy.Sources)
        Text << ' ' << ninjaPath(Source);
      Text << " | " << ninjaPath(Call.Executable) << '\n';
      RuntimeOutputs.insert(Destination);
    }

    if (!Modules.Units.empty()) {
      output(Descriptor);
      output(Modules.Dyndep);
      atomicWrite(Root / Descriptor, modulePlanText(Modules));
      Text << "build " << ninjaPath(Modules.Dyndep);
      for (const auto &U : Modules.Units)
        Text << ' ' << ninjaPath(U.Flags);
      for (const auto &L : Modules.Links)
        Text << ' ' << ninjaPath(L.Flags);
      Text << ": collate " << ninjaPath(Descriptor) << " | "
           << ninjaPath(Call.Executable);
      for (const auto &U : Modules.Units)
        Text << ' ' << ninjaPath(U.Scan);
      Text << '\n';
    }
    Text << "build selected: phony";
    for (auto Index : Plan.Roots)
      for (const auto &File : Plan.Actions[Index].Outputs)
        Text << ' ' << ninjaPath(File);
    for (const auto &File : RuntimeOutputs)
      Text << ' ' << ninjaPath(File);
    Text << "\ndefault selected\n";
    return Text.str();
  }
  BuildResult run(BuildDetails Details) {
    auto Text = ninjaGraph(Details);
    output(ManifestFile);
    for (auto Name : {".ninja_log", ".ninja_deps", ".ninja_log.recompact",
                      ".ninja_deps.recompact"})
      output(Objects / Name);
    std::set<Path> CurrentProjects;
    for (const auto &A : Plan.Actions)
      for (const auto &File : A.Outputs) {
        auto Rel = File.lexically_relative(Objects / "projects");
        if (isRelativeSafe(Rel))
          CurrentProjects.insert(*Rel.begin());
      }
    CurrentProjects.insert("root");
    for (const auto &[Owner, Owned] : Ownership) {
      OwnedOutputs Obsolete;
      for (const auto &File : Owned) {
        auto Relative = File.lexically_relative(Objects);
        auto Project = File.lexically_relative(Objects / "projects");
        bool Selected = !isRelativeSafe(Project) ||
                        CurrentProjects.contains(*Project.begin());
        if (Owner == Root && isRelativeSafe(Relative) && Selected &&
            !Reserved[Owner].contains(File))
          Obsolete.insert(File);
        bool InBuildDirectory = false;
        for (auto Target : Targets)
          for (const auto *Profile : {"debug", "release"})
            InBuildDirectory |=
                isRelativeSafe(File.lexically_relative(
                    Path("obj") / pathFromText(targetName(Target)) /
                    Profile)) ||
                isRelativeSafe(File.lexically_relative(
                    Path("bin") / pathFromText(targetName(Target)) / Profile));
        if (!InBuildDirectory && !Reserved[Owner].contains(File) &&
            ownedPath(File))
          Obsolete.insert(File);
      }
      removeOutputs(Owner, Obsolete);
      for (const auto &File : Obsolete)
        Ownership.at(Owner).erase(File);
    }
    saveOwnership();
    atomicWrite(Root / ManifestFile, Text);
    auto Ninja = Toolchain / "bin/ninja";
#ifdef _WIN32
    Ninja += ".exe";
#endif
    Strings Arguments{"-f", pathText(ManifestFile)};
    if (Request.Jobs)
      Arguments.insert(Arguments.end(), {"-j", std::to_string(Request.Jobs)});
    if (Call.Output.Verbose)
      Arguments.push_back("-v");
    if (Call.Output.Quiet)
      Arguments.push_back("--quiet");
    auto Code = runProcess(Call, Ninja, Arguments,
                           {.Directory = Root,
                            .ToolBin = Toolchain / "bin",
                            .OutputToError = true,
                            .Environment = toolEnvironment(
                                targetToolchain(Toolchain, Request.BuildTarget),
                                Request.BuildTarget)});
    if (!Code.success())
      throw BuildFailure(Code.ExitCode, "Build failed");
    BuildResult Result;
    if (auto A = Plan.Artifacts.find(Graph.Root); A != Plan.Artifacts.end())
      Result.Executable = Root / A->second;
    if (Details == BuildDetails::None)
      return Result;
    Result.CompileCommands = std::move(Commands);
    if (!Modules.Units.empty()) {
      for (std::size_t I = 0; I < Modules.Units.size(); ++I) {
        auto &Flags = Result.CompileCommands.at(Modules.Units[I].Object);
        auto Position =
            std::ranges::find(Flags, "@" + pathText(Modules.Units[I].Flags));
        if (Position == Flags.end())
          throw Failure("Missing module argument input");
        auto Offset = Position - Flags.begin();
        Flags.erase(Position);
        auto Resolved =
            responseArguments(readText(Root / Modules.Units[I].Flags));
        Flags.insert(Flags.begin() + Offset, Resolved.begin(), Resolved.end());
      }
      if (Details == BuildDetails::Modules) {
        auto Facts = resolveModules(Modules);
        for (std::size_t I = 0; I < Modules.Units.size(); ++I)
          Result.Modules.emplace(Modules.Units[I].Object, std::move(Facts[I]));
      }
    }
    return Result;
  }
};
}
BuildResult buildProject(Context &Call, const Command &Request,
                         const ProjectGraph &Graph, BuildDetails Details) {
  std::set<Path> Roots;
  for (const auto &[Identity, Node] : Graph.Projects)
    Roots.insert(Node.Input.Project.Root);
  std::vector<std::unique_ptr<FileLock>> Locks;
  for (const auto &Root : Roots) {
    rejectSymlinkAncestors(Root / "obj");
    fs::create_directories(Root / "obj");
    Locks.push_back(
        std::make_unique<FileLock>(Call, Root / "obj/.guard", true));
  }
  return BuildSession(Call, Request, Graph).run(Details);
}

ProjectGraph mergeBuildGraphs(const std::vector<ProjectGraph> &Graphs) {
  if (Graphs.empty())
    throw Failure("No products selected");
  ProjectGraph Result;
  Result.Root = Graphs.front().Root;
  std::set<std::string> Seen;
  for (const auto &Graph : Graphs) {
    for (const auto &[Identity, Node] : Graph.Projects)
      Result.Projects.try_emplace(Identity, Node);
    for (const auto &Identity : Graph.BuildOrder)
      if (Seen.insert(Identity).second)
        Result.BuildOrder.push_back(Identity);
  }
  return Result;
}

BuildResult buildProjects(Context &Call, const Command &Request,
                          const std::vector<ProjectGraph> &Graphs,
                          const Path &Owner, BuildDetails Details) {
  auto Graph = mergeBuildGraphs(Graphs);
  std::set<std::string> Products;
  std::set<Path> Roots{Owner};
  for (const auto &Input : Graphs)
    Products.insert(Input.Root);
  for (const auto &[Identity, Node] : Graph.Projects)
    Roots.insert(Node.Input.Project.Root);
  std::vector<std::unique_ptr<FileLock>> Locks;
  for (const auto &Root : Roots) {
    rejectSymlinkAncestors(Root / "obj");
    fs::create_directories(Root / "obj");
    Locks.push_back(
        std::make_unique<FileLock>(Call, Root / "obj/.guard", true));
  }
  return BuildSession(Call, Request, Graph, Owner, Products).run(Details);
}

}
