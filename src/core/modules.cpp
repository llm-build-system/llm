module llm.core;

import std;

namespace llm {

Result<ModuleOrder> orderModules(const ProjectGraph &Graph,
                                 const std::vector<ModuleFacts> &Modules) {
  ModuleOrder Result;
  Result.Dependencies.resize(Modules.size());
  std::map<std::pair<std::string, std::string>, std::size_t> Providers;
  std::set<std::pair<std::string, Path>> Units;
  for (std::size_t Index = 0; Index < Modules.size(); ++Index) {
    const auto &Unit = Modules[Index];
    if (!Graph.Projects.contains(Unit.Project))
      return std::unexpected(Diagnostic{
          "Module facts reference an unknown project: " + Unit.Project});
    if (!Units.emplace(Unit.Project, Unit.Source).second)
      return std::unexpected(
          Diagnostic{"Duplicate module scan for " + pathText(Unit.Source)});
    if (!Unit.Provider)
      continue;
    const auto &Name = Unit.Provider->Name;
    if (Name == "std" || Name.starts_with("std."))
      return std::unexpected(
          Diagnostic{"The module name '" + Name +
                     "' is reserved for the standard library"});
    if (Name.empty() || Name.find_first_of("\r\n\0", 0, 3) != std::string::npos)
      return std::unexpected(Diagnostic{"Invalid module provider name"});
    if (!Providers.emplace(std::pair{Unit.Project, Name}, Index).second)
      return std::unexpected(Diagnostic{"Duplicate module provider: " + Name});
  }
  for (std::size_t Index = 0; Index < Modules.size(); ++Index) {
    const auto &Unit = Modules[Index];
    const auto &Visible =
        Graph.Projects.at(Unit.Project).Compile.VisibleProjects;
    for (const auto &Name : Unit.Imports) {
      if (Name == "std" || Name == "std.compat")
        continue;
      std::optional<std::size_t> Provider;
      for (const auto &[Key, Candidate] : Providers) {
        if (Key.second != Name || !Visible.contains(Key.first))
          continue;
        const auto &Declaration = *Modules[Candidate].Provider;
        if (Key.first != Unit.Project &&
            (!Declaration.Interface || Name.contains(':')))
          continue;
        if (Provider)
          return std::unexpected(
              Diagnostic{"Ambiguous visible module provider: " + Name});
        Provider = Candidate;
      }
      if (!Provider)
        return std::unexpected(Diagnostic{"No visible module provider for '" +
                                          Name + "' in " +
                                          pathText(Unit.Source)});
      auto &Dependencies = Result.Dependencies[Index];
      if (std::ranges::find(Dependencies, *Provider) == Dependencies.end())
        Dependencies.push_back(*Provider);
    }
  }
  std::vector<unsigned> State(Modules.size());
  auto Visit = [&](auto &&Self, std::size_t Index) -> llm::Result<void> {
    if (State[Index] == 1)
      return std::unexpected(Diagnostic{"C++ module dependency cycle at " +
                                        pathText(Modules[Index].Source)});
    if (State[Index] == 2)
      return {};
    State[Index] = 1;
    for (auto Provider : Result.Dependencies[Index])
      if (auto Valid = Self(Self, Provider); !Valid)
        return Valid;
    State[Index] = 2;
    Result.Order.push_back(Index);
    return {};
  };
  for (std::size_t Index = 0; Index < Modules.size(); ++Index)
    if (auto Valid = Visit(Visit, Index); !Valid)
      return std::unexpected(Valid.error());
  return Result;
}

}
