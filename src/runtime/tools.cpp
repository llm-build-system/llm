module;
#include <nlohmann/json.hpp>
module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
namespace {
bool formatIgnored(const Path &File);
bool below(const Path &Root, const Path &File) {
  auto Relative = File.lexically_relative(Root);
  return Relative == "." || isRelativeSafe(Relative);
}
Path privateTool(const Path &Toolchain, std::string Name) {
#ifdef _WIN32
  Name += ".exe";
#endif
  auto File = Toolchain / "bin" / pathFromText(Name);
  if (!fs::is_regular_file(File))
    throw Failure("Private tool is missing: " + Name + ". Run llm --install.");
  return File;
}
void showCommand(Context &Call, const Path &Program, const Strings &Arguments) {
  if (!Call.Output.Verbose)
    return;
  auto Text = quoteArgument(pathText(Program));
  for (const auto &Argument : Arguments)
    Text += ' ' + quoteArgument(Argument);
  emit(Call, EventKind::Progress, Text);
}

int format(Context &Call, const Command &Request, const Manifest &Project) {
  std::vector<Path> Requested;
  for (const auto &Path : Request.FormatPaths) {
    auto File = (Path.is_absolute() ? Path : Call.InvocationDirectory / Path)
                    .lexically_normal();
    rejectSymlinkAncestors(File);
    if (!below(Project.Root, File))
      throw Failure("Format paths must stay inside the selected project");
    if (!fs::is_directory(File) && !fs::is_regular_file(File))
      throw Failure("Format path does not exist: " + pathText(File));
    Requested.push_back(File);
  }
  std::vector<Manifest> Scopes{Project};
  if (Project.Type != ProductType::Test)
    for (const auto &Suite : discoverTests(Project))
      Scopes.push_back(Suite.Project);
  std::set<Path> Files, Owners;
  for (const auto &Scope : Scopes) {
    auto Settings = take(effectiveSettings(Scope, Request.BuildTarget));
    auto Inventory = sourceInventory(Scope.Root);
    for (const auto &Relative : Inventory) {
      auto File = Scope.Root / Relative;
      auto Text = pathText(Relative);
      if (std::ranges::any_of(Settings.Excludes, [&](const auto &Pattern) {
            return matchesGlob(Pattern, Text);
          }))
        continue;
      auto Language = sourceLanguage(File);
      if (Language == llm::Language::Assembly) {
        continue;
      }
      if (!Language && !isHeader(File))
        continue;
      if (!Requested.empty() &&
          std::ranges::none_of(Requested, [&](const Path &Path) {
            return File == Path ||
                   (fs::is_directory(Path) && below(Path, File));
          }))
        continue;
      if (!formatIgnored(File)) {
        Files.insert(File);
        Owners.insert(Scope.Root);
      }
    }
  }
  if (Files.empty()) {
    emit(Call, EventKind::Progress, "No project files to format.");
    return 0;
  }
  auto Toolchain = Call.Toolchain ? *Call.Toolchain : installedToolchain(Call);
  auto Tool = privateTool(Toolchain, "clang-format");
  std::vector<std::unique_ptr<FileLock>> Locks;
  for (const auto &Owner : Owners) {
    rejectSymlinkAncestors(Owner / "obj");
    fs::create_directories(Owner / "obj");
    Locks.push_back(
        std::make_unique<FileLock>(Call, Owner / "obj/.guard", true));
  }
  bool Failed = false;
  for (const auto &File : Files) {
    auto Style = File.parent_path();
    while (!fs::is_regular_file(Style / ".clang-format") &&
           !fs::is_regular_file(Style / "_clang-format")) {
      if (Style == Style.root_path())
        throw Failure("No explicit clang-format style for " + pathText(File));
      Style = Style.parent_path();
    }
    Strings Arguments{"--style=file", "--fallback-style=none",
                      "--fno-color-diagnostics"};
    if (Request.FormatCheck)
      Arguments.insert(Arguments.end(), {"--dry-run", "--Werror"});
    else
      Arguments.emplace_back("-i");
    Arguments.insert(Arguments.end(), {"--", pathText(File)});
    showCommand(Call, Tool, Arguments);
    if (!runProcess(Call, Tool, Arguments,
                    {.Directory = Project.Root,
                     .ToolBin = Toolchain / "bin",
                     .OutputToError = true})
             .success())
      Failed = true;
  }
  if (!Failed)
    emit(Call, EventKind::Progress,
         Request.FormatCheck ? "Formatting is up to date."
                             : "Formatting finished.");
  return Failed ? 1 : 0;
}

int analyze(Context &Call, const Command &Request, const Manifest &Project) {
  auto Owner = Project.Root;
  std::vector<Manifest> Roots{Project};
  if (Request.IncludeTests && Project.Type != ProductType::Test)
    for (const auto &Suite : discoverTests(Project))
      Roots.push_back(Suite.Project);
  auto Packages = resolveDependencies(Call, Request, Owner, Roots);
  auto Toolchain = Call.Toolchain ? *Call.Toolchain : installedToolchain(Call);
  auto Tool = privateTool(Toolchain, "clang-tidy");
  std::vector<ProjectGraph> Graphs{
      loadGraph(Call, Project, Request.BuildTarget, Packages)};
  if (Request.IncludeTests && Project.Type != ProductType::Test)
    for (const auto &Suite : discoverTests(Project))
      Graphs.push_back(
          loadGraph(Call, Suite.Project, Request.BuildTarget, Packages));
  auto Check = Request;
  Check.Action = Operation::Check;
  auto Build =
      buildProjects(Call, Check, Graphs, Owner, BuildDetails::Arguments);
  const auto &Commands = Build.CompileCommands;
  FileLock Lock(Call, Owner / "obj/.guard", true);
  bool Failed = false;
  for (const auto &Graph : Graphs) {
    const auto &Node = Graph.Projects.at(Graph.Root);
    auto Plan =
        take(planBuild(Graph, {Request.BuildTarget, Request.Mode, true}));
    for (const auto &Action : Plan.Actions) {
      if (Action.Project != Graph.Root || Action.Kind != ActionKind::Compile)
        continue;
      const auto &Unit = std::get<CompileUnit>(Action.Details);
      if (Unit.SourceLanguage == Language::Assembly)
        continue;
      Strings Arguments{"--use-color=false"};
      if (Call.Output.Quiet)
        Arguments.emplace_back("--quiet");
      if (!Request.Checks.empty())
        Arguments.emplace_back("--checks=" + Request.Checks);
      if (!Request.WarningsAsErrors.empty())
        Arguments.emplace_back("--warnings-as-errors=" +
                               Request.WarningsAsErrors);
      Arguments.push_back(pathText(Unit.Source));
      Arguments.emplace_back("--");
      const auto &Flags = Commands.at(Action.Outputs.front());
      for (std::size_t Index = 0; Index < Flags.size(); ++Index) {
        const auto &Flag = Flags[Index];
        if (Flag == "-o" || Flag == "-MF" || Flag == "-MQ" || Flag == "-MT") {
          ++Index;
          continue;
        }
        if (Flag == "-c" || Flag == "-MMD" || Flag == "-MD" ||
            Flag == pathText(Unit.Source) ||
            Flag.starts_with("-fmodule-output="))
          continue;
        Arguments.push_back(Flag);
      }
      showCommand(Call, Tool, Arguments);
      if (!runProcess(Call, Tool, Arguments,
                      {.Directory = Owner,
                       .ToolBin = Toolchain / "bin",
                       .OutputToError = true})
               .success())
        Failed = true;
    }
  }
  return Failed ? 1 : 0;
}

int list(Context &Call, const Command &Request, const Manifest &Project) {
  auto Owner = Project.Root;
  auto Packages = cachedDependencies(Call, Owner);
  auto Graph = loadGraph(Call, Project, Request.BuildTarget, Packages, true);
  const auto &Node = Graph.Projects.at(Graph.Root);
  const auto &Settings = Node.Settings;
  auto Suites = discoverTests(Project);
  nlohmann::json Dependencies = nlohmann::json::array();
  Strings Sources;
  for (const auto &File : Node.Sources)
    Sources.push_back(pathText(File));
  std::optional<LockData> Lock;
  if (fs::is_regular_file(Owner / ".lock"))
    Lock = parseLock(readText(Owner / ".lock"));
  auto Add = [&](const Strings &Values, bool Public) {
    for (const auto &Text : Values) {
      auto Reference = take(dependencyReference(Project, Text, Public));
      Path Resolved;
      std::string Identity;
      if (Reference.Local)
        Resolved = *Reference.Local;
      else {
        Identity = packageIdentity(
            take(parseRegistrySource(Reference.Registry)), Reference.Package);
        if (auto Found = Packages.find(Identity); Found != Packages.end())
          Resolved = Found->second;
      }
      nlohmann::json Value{{"declaration", Text},
                           {"visibility", Public ? "public" : "private"},
                           {"kind", Reference.Local ? "local" : "registry"},
                           {"resolved", !Resolved.empty()}};
      if (!Resolved.empty())
        Value["path"] = pathText(Resolved);
      if (!Reference.Local) {
        Value["source"] = Reference.Registry;
        Value["package"] = Reference.Package;
        Value["requirement"] = Reference.RequirementText;
        if (!Resolved.empty() && Lock && Lock->Packages.contains(Identity)) {
          Value["version"] = Lock->Packages.at(Identity).Selected.Text;
          Value["sha256"] = Lock->Packages.at(Identity).Sha256;
        }
      }
      Dependencies.push_back(std::move(Value));
    }
  };
  Add(Settings.Dependencies.Private, false);
  Add(Settings.Dependencies.Public, true);
  auto Visibility = [](const llm::Visibility &Value) {
    return nlohmann::json{{"private", Value.Private}, {"public", Value.Public}};
  };
  nlohmann::json Value{
      {"name", Project.Name},
      {"type", productTypeName(Project.Type)},
      {"root", pathText(Project.Root)},
      {"target", targetName(Request.BuildTarget)},
      {"profile", Request.Mode == Profile::Release ? "release" : "debug"},
      {"resolved", Graph.Complete},
      {"sources", Sources},
      {"declarations",
       {{"sources", Settings.Sources},
        {"excludes", Settings.Excludes},
        {"includes", Visibility(Settings.Includes)},
        {"defines", Visibility(Settings.Defines)}}},
      {"effective",
       {{"standards",
         {{"c", standardName(Settings.CStandard)},
          {"cxx", standardName(Settings.CxxStandard)}}},
        {"includes", Node.Compile.Includes},
        {"defines", Node.Compile.Defines},
        {"flags",
         {{"c", Settings.Arguments.C},
          {"cxx", Settings.Arguments.Cxx},
          {"asm", Settings.Arguments.Assembly},
          {"link", Settings.Arguments.Link}}},
        {"libraries", Settings.Libraries},
        {"resources", Settings.Resources}}},
      {"dependencies", Dependencies},
      {"generators", nlohmann::json::array()},
      {"tests", nlohmann::json::array()}};
  for (const auto &Rule : Settings.Generators) {
    nlohmann::json Outputs = nlohmann::json::array();
    for (const auto &File : Rule.Outputs)
      Outputs.push_back({{"path", File},
                         {"present", fs::is_regular_file(fs::symlink_status(
                                         Project.Root / pathFromText(File)))}});
    Value["generators"].push_back({{"inputs", Rule.Inputs},
                                   {"command", Rule.Command},
                                   {"outputs", Outputs}});
  }
  for (const auto &Suite : Suites)
    Value["tests"].push_back(
        {{"selector", Suite.Selector}, {"root", pathText(Suite.Project.Root)}});
  if (Request.Json)
    emit(Call, EventKind::StandardOutput, Value.dump() + '\n');
  else {
    std::ostringstream Text;
    Text << Project.Name << " (" << productTypeName(Project.Type) << ")\n"
         << targetName(Request.BuildTarget) << " / "
         << (Request.Mode == Profile::Release ? "release" : "debug") << '\n';
    for (const auto &Dependency : Dependencies)
      Text << "  " << Dependency.at("declaration").get<std::string>()
           << (Dependency.at("resolved").get<bool>() ? "" : " (unresolved)")
           << '\n';
    for (const auto &File : Sources)
      Text << "  source: " << File << '\n';
    for (const auto &Rule : Settings.Generators)
      for (const auto &File : Rule.Outputs)
        Text << "  declared output: " << File
             << (fs::is_regular_file(Project.Root / pathFromText(File))
                     ? " (present)"
                     : " (missing)")
             << '\n';
    for (const auto &Suite : Suites)
      Text << "  test: " << Suite.Selector << '\n';
    if (Call.Output.Verbose) {
      Text << "  standards: " << standardName(Settings.CStandard) << ", "
           << standardName(Settings.CxxStandard) << '\n';
      for (const auto &Include : Node.Compile.Includes)
        Text << "  include: " << Include << '\n';
      for (const auto &Define : Node.Compile.Defines)
        Text << "  define: " << Define << '\n';
      for (const auto *Flags :
           {&Settings.Arguments.C, &Settings.Arguments.Cxx,
            &Settings.Arguments.Assembly, &Settings.Arguments.Link})
        for (const auto &Flag : *Flags)
          Text << "  flag: " << Flag << '\n';
    }
    emit(Call, EventKind::StandardOutput, Text.str());
  }
  return 0;
}
}

int projectTools(Context &Call, const Command &Request,
                 const Manifest &Project) {
  switch (Request.Action) {
  case Operation::Format:
    return format(Call, Request, Project);
  case Operation::Analyze:
    return analyze(Call, Request, Project);
  case Operation::List:
    return list(Call, Request, Project);
  default:
    throw Failure("Invalid project tool operation");
  }
}
}

namespace llm::runtime {
namespace {
bool ignorePattern(std::string_view Pattern, std::string_view Text) {
  if (Pattern.size() > 32768 || Text.size() > 32768)
    throw Failure("Formatting ignore pattern or path is too long");
  std::map<std::pair<size_t, size_t>, bool> Memo;
  auto Match = [&](auto &&Self, size_t P, size_t T) -> bool {
    auto Key = std::pair{P, T};
    if (auto Found = Memo.find(Key); Found != Memo.end())
      return Found->second;
    if (Memo.size() > 200000)
      throw Failure("Formatting ignore pattern is too complex");
    bool Result = false;
    if (P == Pattern.size())
      Result = T == Text.size();
    else if (Pattern[P] == '*') {
      auto Next = P;
      while (Next < Pattern.size() && Pattern[Next] == '*')
        ++Next;
      bool Recursive = Next - P == 2 && (P == 0 || Pattern[P - 1] == '/') &&
                       (Next == Pattern.size() || Pattern[Next] == '/');
      if (Recursive && Next < Pattern.size())
        Result = Self(Self, Next + 1, T);
      Result = Result || Self(Self, Next, T);
      if (T < Text.size() && (Recursive || Text[T] != '/'))
        Result = Result || Self(Self, P, T + 1);
    } else if (T < Text.size()) {
      auto C = Pattern[P];
      if (C == '\\' && P + 1 < Pattern.size())
        Result = Pattern[P + 1] == Text[T] && Self(Self, P + 2, T + 1);
      else if (C == '?')
        Result = Text[T] != '/' && Self(Self, P + 1, T + 1);
      else if (C == '[') {
        auto End = Pattern.find_first_of("]/", P + 1);
        if (End != std::string_view::npos && Pattern[End] == ']' &&
            End > P + 1 && !(Pattern[P + 1] == '!' && End == P + 2)) {
          auto I = P + 1;
          bool Negated = Pattern[I] == '!';
          if (Negated)
            ++I;
          bool Member = false;
          while (I < End) {
            if (I + 2 < End && Pattern[I + 1] == '-') {
              Member = Member ||
                       (Pattern[I] <= Text[T] && Text[T] <= Pattern[I + 2]);
              I += 3;
            } else {
              auto Value = Pattern[I++];
              Member = Member || Value == Text[T];
            }
          }
          Result = Text[T] != '/' && (Member != Negated) &&
                   Self(Self, End + 1, T + 1);
        } else
          Result = C == Text[T] && Self(Self, P + 1, T + 1);
      } else
        Result = C == Text[T] && Self(Self, P + 1, T + 1);
    }
    Memo.emplace(Key, Result);
    return Result;
  };
  return Match(Match, 0, 0);
}
std::string normalizedIgnorePattern(std::string Text) {
  bool Rooted = Text.starts_with('/');
  Strings Components;
  std::istringstream Input(Text);
  std::string Part;
  while (std::getline(Input, Part, '/')) {
    if (Part.empty() || Part == ".")
      continue;
    if (Part == ".." && !Components.empty() && Components.back() != "..")
      Components.pop_back();
    else if (Part != ".." || !Rooted)
      Components.push_back(std::move(Part));
  }
  std::string Result = Rooted ? "/" : "";
  for (const auto &Component : Components) {
    if (!Result.empty() && Result.back() != '/')
      Result += '/';
    Result += Component;
  }
  return Result;
}
}
namespace {
bool formatIgnored(const fs::path &File) {
  if (!fs::is_regular_file(File))
    return false;
  auto Absolute = fs::absolute(File).lexically_normal(),
       Directory = Absolute.parent_path();
  for (;;) {
    auto Ignore = Directory / ".clang-format-ignore";
    if (fs::is_regular_file(Ignore)) {
      if (fs::file_size(Ignore) > 1024 * 1024)
        throw Failure("Formatting ignore file exceeds 1 MiB: " +
                      pathText(Ignore));
      std::istringstream Input(readText(Ignore));
      std::string Line;
      bool Ignored = false;
      while (std::getline(Input, Line)) {
        auto Begin = Line.find_first_not_of(" \t\r\n\f\v");
        if (Begin == std::string::npos || Line[Begin] == '#')
          continue;
        auto End = Line.find_last_not_of(" \t\r\n\f\v");
        std::string Pattern = Line.substr(Begin, End - Begin + 1);
        bool Negated = Pattern.front() == '!';
        if (Negated) {
          Pattern.erase(0, 1);
          auto First = Pattern.find_first_not_of(" \t\r\n\f\v");
          if (First == std::string::npos)
            continue;
          Pattern.erase(0, First);
        }
        if (Pattern.empty())
          continue;
        if (!Pattern.starts_with('/'))
          Pattern =
              normalizedIgnorePattern(pathText(Directory) + "/" + Pattern);
        if (ignorePattern(Pattern, pathText(Absolute)))
          Ignored = !Negated;
      }
      return Ignored;
    }
    auto Parent = Directory.parent_path();
    if (Parent == Directory)
      return false;
    Directory = Parent;
  }
}
}
}
