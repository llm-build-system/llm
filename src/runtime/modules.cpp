module;
#include <nlohmann/json.hpp>
module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
std::string moduleKey(std::string_view Text) {
  return sha256Text(Text).substr(0, 32);
}
Strings responseArguments(std::string_view Text) {
  Strings Result;
  for (std::size_t Pos = 0; Pos < Text.size();) {
    while (Pos < Text.size() &&
           std::isspace(static_cast<unsigned char>(Text[Pos])))
      ++Pos;
    if (Pos == Text.size())
      break;
    std::string Value;
    bool Quoted = false;
    while (Pos < Text.size() &&
           (Quoted || !std::isspace(static_cast<unsigned char>(Text[Pos])))) {
      std::size_t Slashes = 0;
      while (Pos < Text.size() && Text[Pos] == '\\') {
        ++Slashes;
        ++Pos;
      }
      if (Pos < Text.size() && Text[Pos] == '"') {
        Value.append(Slashes / 2, '\\');
        if (Slashes % 2)
          Value += '"';
        else
          Quoted = !Quoted;
        ++Pos;
      } else {
        Value.append(Slashes, '\\');
        if (Pos < Text.size())
          Value += Text[Pos++];
      }
    }
    if (Quoted)
      throw Failure("Unterminated compiler response argument");
    Result.push_back(std::move(Value));
  }
  return Result;
}

namespace {
std::string pathToken(const Path &File) {
  std::string Out;
  for (char C : pathText(File)) {
    if (C == '$' || C == ' ' || C == ':')
      Out += '$';
    Out += C;
  }
  return Out;
}
std::string flagsText(const Strings &Flags) {
  std::string Text;
  for (const auto &Flag : Flags)
    Text += responseArgument(Flag) + "\n";
  return Text;
}
Path standardObject(Path File) {
  File.replace_extension(".o");
  return File;
}
}
std::string modulePlanText(const ModulePlan &Plan) {
  Strings Values{pathText(Plan.Root), pathText(Plan.Dyndep)};
  for (const auto &[Project, Visible] : Plan.Visible) {
    Values.insert(Values.end(),
                  {"--visible", Project, std::to_string(Visible.size())});
    Values.insert(Values.end(), Visible.begin(), Visible.end());
  }
  for (const auto &Unit : Plan.Units)
    Values.insert(Values.end(),
                  {"--unit", Unit.Project, pathText(Unit.Source),
                   pathText(Unit.Object), pathText(Unit.Bmi),
                   pathText(Unit.Scan), pathText(Unit.Flags),
                   pathText(Unit.Standard), pathText(Unit.Compat)});
  for (const auto &Link : Plan.Links) {
    Values.insert(Values.end(),
                  {"--link", pathText(Link.Output), pathText(Link.Flags),
                   std::to_string(Link.Projects.size())});
    Values.insert(Values.end(), Link.Projects.begin(), Link.Projects.end());
  }
  return flagsText(Values);
}
ModulePlan readModulePlan(const Path &File) {
  auto Values = responseArguments(readText(File));
  std::size_t Index = 0;
  auto Next = [&]() -> std::string {
    if (Index == Values.size())
      throw Failure("Incomplete module plan");
    return Values[Index++];
  };
  auto Count = [&]() {
    auto Text = Next();
    std::size_t Value = 0;
    auto R = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    if (R.ec != std::errc{} || R.ptr != Text.data() + Text.size() ||
        Value > 100000)
      throw Failure("Invalid module plan count");
    return Value;
  };
  ModulePlan Plan;
  Plan.Root = pathFromText(Next());
  Plan.Dyndep = pathFromText(Next());
  while (Index < Values.size()) {
    auto Kind = Next();
    if (Kind == "--visible") {
      auto Project = Next();
      auto N = Count();
      auto [It, Added] = Plan.Visible.emplace(Project, std::set<std::string>{});
      if (!Added)
        throw Failure("Duplicate project visibility");
      while (N--)
        It->second.insert(Next());
    } else if (Kind == "--unit") {
      ModuleUnit Unit;
      Unit.Project = Next();
      Unit.Source = pathFromText(Next());
      Unit.Object = pathFromText(Next());
      Unit.Bmi = pathFromText(Next());
      Unit.Scan = pathFromText(Next());
      Unit.Flags = pathFromText(Next());
      Unit.Standard = pathFromText(Next());
      Unit.Compat = pathFromText(Next());
      Plan.Units.push_back(std::move(Unit));
    } else if (Kind == "--link") {
      ModuleLink Link;
      Link.Output = pathFromText(Next());
      Link.Flags = pathFromText(Next());
      auto N = Count();
      while (N--)
        Link.Projects.insert(Next());
      Plan.Links.push_back(std::move(Link));
    } else
      throw Failure("Invalid module plan argument: " + Kind);
  }
  return Plan;
}
std::vector<ModuleFacts> resolveModules(const ModulePlan &Plan) {
  std::vector<ModuleFacts> Facts;
  for (const auto &Unit : Plan.Units) {
    auto Fact = readModuleScan(Plan.Root / Unit.Scan, Unit.Object, Plan.Root);
    Fact.Project = Unit.Project;
    Fact.Source = Unit.Source;
    Facts.push_back(std::move(Fact));
  }
  return Facts;
}
int collateModules(Context &Call, const Path &Descriptor) {
  auto Plan = readModulePlan(Descriptor);
  if (fs::canonical(Call.InvocationDirectory) != Plan.Root)
    throw Failure("Module plan belongs to another workspace");
  auto Owned = readOwned(Plan.Root);
  auto Validate = [&](const Path &File) {
    checkOutput(Plan.Root, File);
    if (!Owned.contains(File))
      throw Failure("Module output is not owned: " + pathText(File));
  };
  Validate(Plan.Dyndep);
  auto Facts = resolveModules(Plan);
  ProjectGraph Scopes;
  for (const auto &[Id, Visible] : Plan.Visible)
    Scopes.Projects[Id].Compile.VisibleProjects = Visible;
  auto Order = take(orderModules(Scopes, Facts));
  const auto &Imports = Order.Dependencies;
  std::vector<std::map<std::string, Path>> Mappings(Facts.size());
  for (auto I : Order.Order) {
    for (auto J : Imports[I]) {
      for (const auto &[Name, File] : Mappings[J]) {
        auto [It, Added] = Mappings[I].emplace(Name, File);
        if (!Added && It->second != File)
          throw Failure("Incompatible transitive module mapping: " + Name);
      }
      Mappings[I].emplace(Facts[J].Provider->Name, Plan.Units[J].Bmi);
    }
    for (const auto &Name : Facts[I].Imports)
      if (Name == "std" || Name == "std.compat") {
        const auto &Unit = Plan.Units[I];
        for (const auto &[Logical, File] : std::map<std::string, Path>{
                 {"std", Unit.Standard}, {"std.compat", Unit.Compat}}) {
          if (Logical == "std.compat" && Name != "std.compat")
            continue;
          auto [It, Added] = Mappings[I].emplace(Logical, File);
          if (!Added && It->second != File)
            throw Failure("Incompatible standard module contexts in " +
                          pathText(Unit.Source));
        }
      }
  }
  std::ostringstream Dyndep;
  Dyndep << "ninja_dyndep_version = 1\n";
  for (std::size_t I = 0; I < Facts.size(); ++I) {
    const auto &Unit = Plan.Units[I];
    Validate(Unit.Flags);
    Validate(Unit.Bmi);
    Strings Flags;
    Dyndep << "build " << pathToken(Unit.Object);
    if (Facts[I].Provider) {
      Dyndep << " | " << pathToken(Unit.Bmi);
      Flags.insert(Flags.end(),
                   {"-x", "c++-module",
                    "-fmodule-output=" + pathText(Plan.Root / Unit.Bmi)});
    }
    Dyndep << ": dyndep";
    if (!Mappings[I].empty())
      Dyndep << " |";
    for (const auto &[Name, File] : Mappings[I]) {
      Dyndep << ' ' << pathToken(File);
      Flags.push_back("-fmodule-file=" + Name + "=" +
                      pathText(Plan.Root / File));
    }
    Dyndep << "\n";
    atomicWrite(Plan.Root / Unit.Flags, flagsText(Flags));
  }
  for (const auto &Link : Plan.Links) {
    Validate(Link.Flags);
    std::set<Path> Objects;
    for (std::size_t I = 0; I < Facts.size(); ++I)
      if (Link.Projects.contains(Facts[I].Project))
        for (const auto &[Name, File] : Mappings[I])
          if (Name == "std" || Name == "std.compat")
            Objects.insert(standardObject(File));
    Strings Flags;
    Dyndep << "build " << pathToken(Link.Output) << ": dyndep";
    if (!Objects.empty())
      Dyndep << " |";
    for (const auto &File : Objects) {
      Dyndep << ' ' << pathToken(File);
      Flags.push_back(pathText(File));
    }
    Dyndep << "\n";
    atomicWrite(Plan.Root / Link.Flags, flagsText(Flags));
  }
  atomicWrite(Plan.Root / Plan.Dyndep, Dyndep.str());
  return 0;
}
}
