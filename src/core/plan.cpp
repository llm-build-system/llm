module llm.core;

import std;

namespace llm {
namespace {
std::string projectKey(std::string_view Identity) {
  std::uint64_t Hash = 14695981039346656037ULL;
  for (unsigned char Byte : Identity) {
    Hash ^= Byte;
    Hash *= 1099511628211ULL;
  }
  std::ostringstream Text;
  Text << std::hex << Hash;
  return Text.str();
}
}

Path artifactName(std::string_view Name, ProductType Type, Target BuildTarget) {
  std::string Base(Name);
  switch (Type) {
  case ProductType::StaticLibrary:
    return pathFromText(isMsvcTarget(BuildTarget) ? Base + ".lib"
                                                  : "lib" + Base + ".a");
  case ProductType::SharedLibrary:
    if (isWindowsTarget(BuildTarget))
      return pathFromText(Base + ".dll");
    return pathFromText("lib" + Base +
                        (BuildTarget == Target::MacosArm64 ? ".dylib" : ".so"));
  case ProductType::HeaderOnlyLibrary:
    return {};
  case ProductType::Executable:
  case ProductType::Test:
    return pathFromText(Base + (isWindowsTarget(BuildTarget) ? ".exe" : ""));
  }
  std::unreachable();
}

Path linkArtifactName(std::string_view Name, ProductType Type,
                      Target BuildTarget) {
  if (Type == ProductType::SharedLibrary && isWindowsTarget(BuildTarget))
    return pathFromText(isMsvcTarget(BuildTarget)
                            ? std::string(Name) + ".lib"
                            : "lib" + std::string(Name) + ".dll.a");
  return artifactName(Name, Type, BuildTarget);
}

Result<BuildPlan> planBuild(const ProjectGraph &Graph,
                            const BuildContext &Context) {
  BuildPlan Plan;
  Plan.Context = Context;
  auto Relative = pathFromText(std::string(targetName(Context.BuildTarget))) /
                  (Context.Mode == Profile::Release ? "release" : "debug");
  auto ObjectRoot = Path("obj") / Relative;
  auto BinaryRoot = Path("bin") / Relative;
  std::map<Path, std::size_t> Producers;
  std::map<std::string, std::size_t> Finals;
  std::map<std::pair<std::string, Path>, std::size_t> Compilations;
  const auto &Root = Context.OutputRoot.empty()
                         ? Graph.Projects.at(Graph.Root).Input.Project.Root
                         : Context.OutputRoot;
  auto OutputKey = [&](const Path &File) {
    return (File.is_absolute() ? File : Root / File).lexically_normal();
  };
  auto Add = [&](BuildAction Action) -> Result<std::size_t> {
    auto Index = Plan.Actions.size();
    for (const auto &Output : Action.Outputs)
      if (!Producers.emplace(OutputKey(Output), Index).second)
        return std::unexpected(
            Diagnostic{"Conflicting build output: " + pathText(Output)});
    Plan.Actions.push_back(std::move(Action));
    return Index;
  };
  for (const auto &Identity : Graph.BuildOrder) {
    const auto &Node = Graph.Projects.at(Identity);
    for (const auto &Rule : Node.Settings.Generators) {
      BuildAction Action;
      Action.Kind = ActionKind::Generate;
      Action.Project = Identity;
      Action.Details = Rule;
      for (const auto &Input : Rule.Inputs)
        if (Input.find_first_of("*?") == std::string::npos)
          Action.Inputs.push_back(Node.Input.Project.Root /
                                  pathFromText(Input));
        else
          for (const auto &File : Node.Input.Inventory)
            if (matchesGlob(Input, pathText(File)))
              Action.Inputs.push_back(Node.Input.Project.Root / File);
      for (const auto &Output : Rule.Outputs)
        Action.Outputs.push_back(Node.Input.Project.Root /
                                 pathFromText(Output));
      if (auto Added = Add(std::move(Action)); !Added)
        return std::unexpected(Added.error());
    }
  }
  for (auto &Action : Plan.Actions) {
    const auto &Rule = std::get<Generator>(Action.Details);
    const auto &Root = Graph.Projects.at(Action.Project).Input.Project.Root;
    for (const auto &Pattern : Rule.Inputs)
      if (Pattern.find_first_of("*?") != std::string::npos)
        for (const auto &[Output, Producer] : Producers) {
          auto RelativeOutput = Output.lexically_relative(Root);
          if (isRelativeSafe(RelativeOutput) &&
              matchesGlob(Pattern, pathText(RelativeOutput)) &&
              std::ranges::find(Action.Inputs, Output) == Action.Inputs.end())
            Action.Inputs.push_back(Output);
        }
  }
  for (const auto &Identity : Graph.BuildOrder) {
    const auto &Node = Graph.Projects.at(Identity);
    const auto &Project = Node.Input.Project;
    const auto &Settings = Node.Settings;
    auto Key = projectKey(Identity);
    if (Node.Input.Imported) {
      const auto &Imported = *Node.Input.Imported;
      if (Imported.BuildTarget != Context.BuildTarget ||
          Imported.Mode != Context.Mode)
        return std::unexpected(Diagnostic{
            "Development package target/profile does not match its consumer: " +
            Identity});
      Plan.Artifacts[Identity] = Project.Root / Imported.Artifact;
    }
    if (Project.Type == ProductType::HeaderOnlyLibrary)
      continue;
    if (Node.Sources.empty() && !Node.Input.Imported)
      return std::unexpected(Diagnostic{"No compiled sources in " +
                                        pathText(Project.Root / ".llm")});
    std::vector<Path> Objects;
    std::vector<std::size_t> Dependencies;
    for (const auto &Source : Node.Sources) {
      auto SourceKind = sourceLanguage(Source);
      if (!SourceKind)
        return std::unexpected(
            Diagnostic{"Unknown source language: " + pathText(Source)});
      auto Additional = Settings.Inputs.find(pathText(Source));
      auto Output = ObjectRoot / "projects" / Key / Source;
      Output += ".o";
      CompileUnit Unit;
      Unit.Source = Project.Root / Source;
      Unit.SourceLanguage = *SourceKind;
      Unit.LanguageStandard = *SourceKind == Language::C ? Settings.CStandard
                                                         : Settings.CxxStandard;
      Unit.Includes = Node.Compile.Includes;
      Unit.Defines = Node.Compile.Defines;
      Unit.Arguments = *SourceKind == Language::C ? Settings.Arguments.C
                       : *SourceKind == Language::Cxx
                           ? Settings.Arguments.Cxx
                           : Settings.Arguments.Assembly;
      BuildAction Action;
      Action.Kind = ActionKind::Compile;
      Action.Project = Identity;
      Action.Inputs.push_back(Unit.Source);
      if (Additional != Settings.Inputs.end())
        for (const auto &Input : Additional->second)
          Action.Inputs.push_back(Project.Root / pathFromText(Input));
      Action.Outputs.push_back(Output);
      Action.Details = std::move(Unit);
      auto Index = Add(std::move(Action));
      if (!Index)
        return std::unexpected(Index.error());
      Compilations.emplace(std::pair{Identity, Source}, *Index);
      Dependencies.push_back(*Index);
      Objects.push_back(std::move(Output));
    }
    if (Node.Input.Imported) {
      if (Context.CheckOnly)
        Plan.Roots.insert(Plan.Roots.end(), Dependencies.begin(),
                          Dependencies.end());
      continue;
    }
    bool Selected = Context.Products.empty()
                        ? Identity == Graph.Root
                        : Context.Products.contains(Identity);
    auto Directory =
        Selected ? (Context.OutputRoot.empty() ? BinaryRoot
                                               : Project.Root / BinaryRoot)
                 : ObjectRoot / "projects" / Key;
    auto Artifact = Directory / artifactName(Project.Name, Project.Type,
                                             Context.BuildTarget);
    Plan.Artifacts.emplace(Identity, Artifact);
    if (Context.CheckOnly) {
      Plan.Roots.insert(Plan.Roots.end(), Dependencies.begin(),
                        Dependencies.end());
      continue;
    }
    BuildAction Final;
    Final.Kind = Project.Type == ProductType::StaticLibrary
                     ? ActionKind::Archive
                     : ActionKind::Link;
    Final.Project = Identity;
    Final.Inputs = std::move(Objects);
    Final.Dependencies = std::move(Dependencies);
    Final.Outputs.push_back(Artifact);
    if (Project.Type == ProductType::SharedLibrary &&
        isWindowsTarget(Context.BuildTarget))
      Final.Outputs.push_back(
          Directory /
          linkArtifactName(Project.Name, Project.Type, Context.BuildTarget));
    if (Final.Kind == ActionKind::Link && Context.Mode == Profile::Debug &&
        isMsvcTarget(Context.BuildTarget)) {
      auto Pdb =
          ObjectRoot / "projects" / Key / pathFromText(Project.Name + ".pdb");
      Final.Outputs.push_back(std::move(Pdb));
    }
    if (Final.Kind == ActionKind::Link)
      for (const auto &Provider : Node.LinkOrder) {
        const auto &Dependency = Graph.Projects.at(Provider);
        if (Dependency.Input.Project.Type == ProductType::HeaderOnlyLibrary)
          continue;
        if (Dependency.Input.Imported)
          for (const auto &[Name, File] :
               Dependency.Input.Imported->StandardObjects)
            Final.Inputs.push_back(Dependency.Input.Project.Root / File);
        auto Input = Plan.Artifacts.at(Provider);
        if (Dependency.Input.Project.Type == ProductType::SharedLibrary &&
            isWindowsTarget(Context.BuildTarget))
          Input = Dependency.Input.Imported
                      ? Dependency.Input.Project.Root /
                            Dependency.Input.Imported->LinkArtifact
                      : Input.parent_path() /
                            linkArtifactName(Dependency.Input.Project.Name,
                                             Dependency.Input.Project.Type,
                                             Context.BuildTarget);
        Final.Inputs.push_back(Input);
        if (auto Found = Finals.find(Provider); Found != Finals.end())
          Final.Dependencies.push_back(Found->second);
      }
    if (Final.Kind == ActionKind::Link) {
      auto ExternalInputs = [&](const ProjectNode &Provider) {
        for (const auto &Library : Provider.Settings.Libraries) {
          auto File = pathFromText(Library);
          auto Extension = pathText(File.extension());
          if (Extension == ".framework")
            continue;
          if (File.has_parent_path() || File.is_absolute() ||
              Extension == ".a" || Extension == ".lib" || Extension == ".so" ||
              Extension == ".dylib" || Library.contains(".so."))
            Final.Inputs.push_back(
                (File.is_absolute() ? File : Provider.Input.Project.Root / File)
                    .lexically_normal());
        }
      };
      ExternalInputs(Node);
      for (const auto &Provider : Node.LinkOrder)
        if (Graph.Projects.at(Provider).Input.Project.Type !=
            ProductType::SharedLibrary)
          ExternalInputs(Graph.Projects.at(Provider));
    }
    auto Index = Add(std::move(Final));
    if (!Index)
      return std::unexpected(Index.error());
    Finals.emplace(Identity, *Index);
    if (Selected)
      Plan.Roots.push_back(*Index);
  }
  for (std::size_t Index = 0; Index < Plan.Actions.size(); ++Index) {
    auto &Action = Plan.Actions[Index];
    if (Action.Kind == ActionKind::Compile) {
      const auto &Visible =
          Graph.Projects.at(Action.Project).Compile.VisibleProjects;
      for (std::size_t Candidate = 0; Candidate < Plan.Actions.size();
           ++Candidate) {
        const auto &Other = Plan.Actions[Candidate];
        if (Other.Kind == ActionKind::Generate &&
            Visible.contains(Other.Project))
          Action.Dependencies.push_back(Candidate);
      }
    }
    for (const auto &Input : Action.Inputs)
      if (auto Producer = Producers.find(OutputKey(Input));
          Producer != Producers.end())
        Action.Dependencies.push_back(Producer->second);
    std::ranges::sort(Action.Dependencies);
    auto [Begin, End] = std::ranges::unique(Action.Dependencies);
    Action.Dependencies.erase(Begin, End);
  }
  for (std::size_t Index = 0; Index < Plan.Actions.size(); ++Index)
    if (Plan.Actions[Index].Kind == ActionKind::Generate)
      Plan.Roots.push_back(Index);
  std::vector<unsigned> State(Plan.Actions.size());
  auto Visit = [&](auto &&Self, std::size_t Index) -> Result<void> {
    if (State[Index] == 1)
      return std::unexpected(
          Diagnostic{"Generator or build-action dependency cycle"});
    if (State[Index] == 2)
      return {};
    State[Index] = 1;
    for (auto Dependency : Plan.Actions[Index].Dependencies)
      if (auto Valid = Self(Self, Dependency); !Valid)
        return Valid;
    State[Index] = 2;
    return {};
  };
  for (std::size_t Index = 0; Index < Plan.Actions.size(); ++Index)
    if (auto Valid = Visit(Visit, Index); !Valid)
      return std::unexpected(Valid.error());
  return Plan;
}

}
