module llm.core;

import std;

namespace llm {
namespace {
void uniqueAppend(Strings &Into, const Strings &From) {
  for (const auto &Value : From)
    if (std::ranges::find(Into, Value) == Into.end())
      Into.push_back(Value);
}

void appendRequirements(UsageRequirements &Into,
                        const UsageRequirements &From) {
  uniqueAppend(Into.Includes, From.Includes);
  uniqueAppend(Into.Defines, From.Defines);
  Into.VisibleProjects.insert(From.VisibleProjects.begin(),
                              From.VisibleProjects.end());
}

void appendPaths(Strings &Into, const Path &Root, const Strings &From) {
  for (const auto &Value : From) {
    auto File = pathFromText(Value);
    auto Absolute =
        pathText((File.is_absolute() ? File : Root / File).lexically_normal());
    if (std::ranges::find(Into, Absolute) == Into.end())
      Into.push_back(std::move(Absolute));
  }
}
}

Result<ProjectGraph> buildGraph(std::string Root,
                                std::vector<ProjectInput> Projects,
                                Target BuildTarget) {
  if (Projects.size() > 4096)
    return std::unexpected(
        Diagnostic{"Dependency graph exceeds 4096 projects"});
  ProjectGraph Graph;
  Graph.Root = std::move(Root);
  for (auto &Input : Projects) {
    auto Identity = Input.Identity;
    if (Identity.empty() || Graph.Projects.contains(Identity))
      return std::unexpected(Diagnostic{
          "Duplicate or empty canonical project identity: " + Identity});
    auto Settings = effectiveSettings(Input.Project, BuildTarget);
    if (!Settings)
      return std::unexpected(Settings.error());
    auto Sources = selectSources(*Settings, Input.Inventory);
    if (!Sources)
      return std::unexpected(Sources.error());
    if (!Input.Imported &&
        Input.Project.Type == ProductType::HeaderOnlyLibrary &&
        !Sources->empty())
      return std::unexpected(
          Diagnostic{"Header-only libraries cannot contain compiled sources"});
    Graph.Projects.emplace(Identity,
                           ProjectNode{std::move(Input), std::move(*Settings),
                                       std::move(*Sources)});
  }
  if (!Graph.Projects.contains(Graph.Root))
    return std::unexpected(
        Diagnostic{"Root project is absent from the supplied graph"});
  std::map<std::string, unsigned> State;
  Strings Chain;
  auto Visit = [&](auto &&Self, const std::string &Identity) -> Result<void> {
    if (State[Identity] == 1) {
      std::string Cycle;
      for (const auto &Element : Chain)
        Cycle += Element + " -> ";
      return std::unexpected(
          Diagnostic{"Dependency cycle: " + Cycle + Identity});
    }
    if (State[Identity] == 2)
      return {};
    auto Found = Graph.Projects.find(Identity);
    if (Found == Graph.Projects.end())
      return std::unexpected(
          Diagnostic{"Unresolved dependency project: " + Identity});
    State[Identity] = 1;
    Chain.push_back(Identity);
    auto &Node = Found->second;
    Graph.Complete &= Node.Input.Complete;
    const auto &RootPath = Node.Input.Project.Root;
    std::set<std::string> Direct;
    for (const auto &Edge : Node.Input.Dependencies) {
      if (!Direct.insert(Edge.Provider).second)
        return std::unexpected(
            Diagnostic{"Duplicate dependency project: " + Edge.Provider});
      if (auto Valid = Self(Self, Edge.Provider); !Valid)
        return Valid;
      if (!isLibrary(Graph.Projects.at(Edge.Provider).Input.Project.Type))
        return std::unexpected(
            Diagnostic{"Dependency must be a library: " + Edge.Provider});
    }
    auto HasDirectory = [&](std::string_view Name) {
      return std::ranges::any_of(Node.Input.Inventory, [&](const auto &File) {
        return !File.empty() && pathText(*File.begin()) == Name;
      });
    };
    if (HasDirectory("src"))
      appendPaths(Node.Compile.Includes, RootPath, {"src"});
    if (HasDirectory("include")) {
      appendPaths(Node.Compile.Includes, RootPath, {"include"});
      if (isLibrary(Node.Input.Project.Type))
        appendPaths(Node.Exported.Includes, RootPath, {"include"});
    }
    appendPaths(Node.Compile.Includes, RootPath,
                Node.Settings.Includes.Private);
    appendPaths(Node.Compile.Includes, RootPath, Node.Settings.Includes.Public);
    appendPaths(Node.Exported.Includes, RootPath,
                Node.Settings.Includes.Public);
    for (const auto &Rule : Node.Settings.Generators)
      for (const auto &Output : Rule.Outputs)
        if (auto File = pathFromText(Output); isHeader(File))
          appendPaths(Node.Compile.Includes, RootPath,
                      {pathText(File.parent_path())});
    uniqueAppend(Node.Compile.Defines, Node.Settings.Defines.Private);
    uniqueAppend(Node.Compile.Defines, Node.Settings.Defines.Public);
    uniqueAppend(Node.Exported.Defines, Node.Settings.Defines.Public);
    Node.Exported.VisibleProjects.insert(Identity);
    Node.Compile.VisibleProjects.insert(Identity);
    for (const auto &Edge : Node.Input.Dependencies) {
      const auto &Provider = Graph.Projects.at(Edge.Provider);
      appendRequirements(Node.Compile, Provider.Exported);
      if (Edge.Public)
        appendRequirements(Node.Exported, Provider.Exported);
      uniqueAppend(Node.Runtimes, Provider.Runtimes);
      if (Provider.Input.Project.Type == ProductType::SharedLibrary)
        uniqueAppend(Node.Runtimes, {Edge.Provider});
    }
    Strings Postorder;
    std::set<std::string> Linked;
    auto Link = [&](auto &&SelfLink, const std::string &Provider) -> void {
      if (!Linked.insert(Provider).second)
        return;
      const auto &Dependency = Graph.Projects.at(Provider);
      for (const auto &Edge : Dependency.Input.Dependencies)
        if (Dependency.Input.Project.Type != ProductType::SharedLibrary ||
            Edge.Public)
          SelfLink(SelfLink, Edge.Provider);
      Postorder.push_back(Provider);
    };
    for (const auto &Edge : Node.Input.Dependencies)
      Link(Link, Edge.Provider);
    std::ranges::reverse(Postorder);
    Node.LinkOrder = std::move(Postorder);
    std::map<std::string, std::string> Defines;
    for (const auto &Define : Node.Compile.Defines) {
      auto Name = Define.substr(0, Define.find('='));
      auto [Previous, Inserted] = Defines.emplace(Name, Define);
      if (!Inserted && Previous->second != Define)
        return std::unexpected(Diagnostic{"Conflicting define '" + Name +
                                          "' in " +
                                          pathText(RootPath / ".llm")});
    }
    Chain.pop_back();
    State[Identity] = 2;
    Graph.BuildOrder.push_back(Identity);
    return {};
  };
  if (auto Valid = Visit(Visit, Graph.Root); !Valid)
    return std::unexpected(Valid.error());
  if (Graph.BuildOrder.size() != Graph.Projects.size())
    return std::unexpected(Diagnostic{
        "Supplied graph contains projects unreachable from its root"});
  return Graph;
}

}
