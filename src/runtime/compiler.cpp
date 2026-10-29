module;
#include <nlohmann/json.hpp>
module llm.runtime;
import :internal;
import std;

namespace llm::runtime {

std::string responseArgument(std::string_view Text) {
  if (Text.find_first_of("\r\n\0", 0, 3) != std::string_view::npos)
    throw Failure("A tool argument contains a line break or NUL");
  std::string Result = "\"";
  std::size_t Slashes = 0;
  for (char Byte : Text) {
    if (Byte == '\\') {
      ++Slashes;
      continue;
    }
    Result.append(Slashes * (Byte == '"' ? 2 : 1), '\\');
    Slashes = 0;
    if (Byte == '"')
      Result += '\\';
    Result += Byte;
  }
  Result.append(Slashes * 2, '\\');
  return Result + '"';
}

ModuleFacts readModuleScan(const Path &File, const Path &Output,
                           const Path &Root) {
  try {
    if (std::filesystem::file_size(File) > 16 * 1024 * 1024)
      throw Failure("Scanner output exceeds 16 MiB");
    auto Document = nlohmann::json::parse(readText(File));
    if (Document.at("version") != 1 || !Document.at("rules").is_array() ||
        Document.at("rules").size() != 1)
      throw Failure("Invalid P1689 version or rule count");
    auto &Rule = Document.at("rules").front();
    auto Scanned = pathFromText(Rule.at("primary-output").get<std::string>());
    if (Scanned.is_relative())
      Scanned = Root / Scanned;
    if (Scanned.lexically_normal() != (Root / Output).lexically_normal())
      throw Failure("Scanner output does not match its compilation unit");
    auto Name = [](const nlohmann::json &Value) {
      auto Text = Value.at("logical-name").get<std::string>();
      if (Text.empty() || Text.size() > 4096 ||
          Text.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw Failure("Invalid module name in scanner output");
      return Text;
    };
    ModuleFacts Facts;
    if (Rule.contains("provides")) {
      const auto &Provided = Rule.at("provides");
      if (!Provided.is_array() || Provided.size() > 1)
        throw Failure("Multiple modules provided by one source");
      if (!Provided.empty())
        Facts.Provider =
            ModuleProvider{Name(Provided.front()),
                           Provided.front().at("is-interface").get<bool>()};
    }
    std::set<std::string> Names;
    if (Rule.contains("requires")) {
      for (const auto &Required : Rule.at("requires"))
        if (Required.value("lookup-method", "by-name") == "by-name")
          Names.insert(Name(Required));
        else
          throw Failure("Header-unit dependency scanning is not supported by "
                        "this toolchain");
    }
    Facts.Imports.assign(Names.begin(), Names.end());
    return Facts;
  } catch (const std::exception &Error) {
    throw Failure("Module scan '" + pathText(File) + "': " + Error.what());
  }
}

Path compilerPath(const Path &Toolchain, bool Cxx) {
  auto Program = Toolchain / "bin" / (Cxx ? "clang++" : "clang");
#ifdef _WIN32
  Program += ".exe";
#endif
  return Program;
}

Strings compileArguments(const CompileUnit &Unit, const ProjectNode &Node,
                         const BuildContext &Build, const Path &Toolchain) {
  bool Cxx = Unit.SourceLanguage == Language::Cxx;
  bool RawAssembly = Unit.SourceLanguage == Language::Assembly &&
                     Unit.Source.extension() == ".s";
  auto ResourceRoot = isMsvcTarget(Build.BuildTarget)
                          ? Toolchain
                          : targetToolchain(Toolchain, Build.BuildTarget);
  if (!std::filesystem::is_directory(ResourceRoot / "lib/clang/23"))
    throw Failure("Target compiler resources are missing: " +
                  pathText(ResourceRoot / "lib/clang/23"));
  Strings Arguments{
      "--no-default-config",
      "--config=" + pathText(compilerConfig(Toolchain, Build.BuildTarget, Cxx)),
      "-resource-dir=" + pathText(ResourceRoot / "lib/clang/23")};
  if (!RawAssembly) {
    Arguments.emplace_back(Build.Mode == Profile::Release ? "-O2" : "-O0");
    if (Build.Mode == Profile::Release)
      Arguments.emplace_back("-DNDEBUG");
  }
  if (Build.Mode == Profile::Debug) {
    Arguments.emplace_back("-g");
    if (isMsvcTarget(Build.BuildTarget))
      Arguments.emplace_back("-gcodeview");
  }
  if (Unit.SourceLanguage != Language::Assembly) {
    std::string Standard(standardName(Unit.LanguageStandard));
    std::ranges::transform(Standard, Standard.begin(), [](unsigned char Byte) {
      return static_cast<char>(std::tolower(Byte));
    });
    Arguments.emplace_back("-std=" + Standard);
  }
  Arguments.insert(Arguments.end(),
                   {"-x", Cxx                                  ? "c++"
                          : Unit.SourceLanguage == Language::C ? "c"
                          : RawAssembly ? "assembler"
                                        : "assembler-with-cpp"});
  if (!isWindowsTarget(Build.BuildTarget) &&
      isLibrary(Node.Input.Project.Type) &&
      Unit.SourceLanguage != Language::Assembly)
    Arguments.emplace_back("-fPIC");
  if (Unit.SourceLanguage == Language::Assembly)
    Arguments.emplace_back("-I" + pathText(Unit.Source.parent_path()));
  for (const auto &Include : Unit.Includes) {
    auto Directory = pathFromText(Include);
    if (Directory.is_relative())
      Directory = Node.Input.Project.Root / Directory;
    Arguments.emplace_back("-I" + pathText(Directory));
  }
  if (!RawAssembly)
    for (const auto &Define : Unit.Defines)
      Arguments.emplace_back("-D" + Define);
  Arguments.insert(Arguments.end(), Unit.Arguments.begin(),
                   Unit.Arguments.end());
  return Arguments;
}

bool linkUsesCxx(const ProjectGraph &Graph, const std::string &Identity) {
  const auto &Node = Graph.Projects.at(Identity);
  auto HasCxx = [](const ProjectNode &Project) {
    return (Project.Input.Imported && Project.Input.Imported->UsesCxx) ||
           std::ranges::any_of(Project.Sources, [](const Path &File) {
             return sourceLanguage(File) == Language::Cxx;
           });
  };
  bool Cxx = HasCxx(Node);
  for (const auto &Identity : Node.LinkOrder)
    if (Graph.Projects.at(Identity).Input.Project.Type !=
        ProductType::SharedLibrary)
      Cxx |= HasCxx(Graph.Projects.at(Identity));
  return Cxx;
}

Path windowsLibrary(const Path &TargetRoot, std::string Name) {
  const auto Root = TargetRoot / "sdk";
  std::vector<Path> Matches;
  for (const auto *Directory :
       {"msvc/lib", "windows/lib/ucrt", "windows/lib/um"}) {
    auto File = Root / Directory / pathFromText(Name + ".lib");
    if (std::filesystem::is_regular_file(nativePath(File)))
      Matches.push_back(File);
  }
  if (Matches.size() != 1)
    throw Failure("Windows SDK library is missing or ambiguous: " + Name);
  return Matches.front();
}
Strings defaultLinkArguments(const Path &TargetRoot, Target Target, bool Cxx) {
  if (!isMsvcTarget(Target)) {
    if (!Cxx || (!isWindowsTarget(Target) && !isMuslTarget(Target)))
      return {};
    Strings Args;
    for (const auto *Name : {"libc++.a", "libc++abi.a"}) {
      auto File = TargetRoot / "lib" / Name;
      if (!std::filesystem::is_regular_file(nativePath(File)))
        throw Failure("C++ runtime library is missing: " + pathText(File));
      Args.push_back(pathText(File));
    }
    Args.emplace_back("-lm");
    return Args;
  }
  Strings Args{"-nostdlib"};
  if (Cxx) {
    for (const auto *Name : {"libc++.lib", "libc++experimental.lib"}) {
      auto File = TargetRoot / "lib" / Name;
      if (!std::filesystem::is_regular_file(nativePath(File)))
        throw Failure("C++ runtime library is missing: " + pathText(File));
      Args.push_back(pathText(File));
    }
    Args.push_back(pathText(windowsLibrary(TargetRoot, "libcpmt")));
  }
  for (const auto *Name : {"libcmt", "libvcruntime", "libucrt", "oldnames",
                           "kernel32", "iso_stdio_wide_specifiers"})
    Args.push_back(pathText(windowsLibrary(TargetRoot, Name)));
  return Args;
}
std::map<std::string, std::string> toolEnvironment(const Path &TargetRoot,
                                                   Target Target) {
  if (Target != Target::WindowsX64 && Target != Target::WindowsArm64)
    return {};
  const auto Root = TargetRoot / "sdk";
  std::string Libraries;
  for (const auto *Directory :
       {"msvc/lib", "windows/lib/ucrt", "windows/lib/um"}) {
    if (!Libraries.empty())
      Libraries += ';';
    Libraries += pathText(Root / Directory);
  }
  return {{"LIB", std::move(Libraries)}};
}
Strings linkArguments(const BuildAction &Action, const ProjectGraph &Graph,
                      const BuildContext &Build, const Path &Toolchain,
                      const std::map<std::string, Path> &StandardObjects) {
  const auto &Node = Graph.Projects.at(Action.Project);
  bool Cxx = linkUsesCxx(Graph, Action.Project);
  Strings Arguments{"--no-default-config",
                    "--config=" + pathText(compilerConfig(
                                      Toolchain, Build.BuildTarget, Cxx))};
  std::set<Path> External;
  auto Settings = [&](const ProjectNode &Provider) {
    for (const auto &Library : Provider.Settings.Libraries) {
      auto File = pathFromText(Library);
      auto Extension = pathText(File.extension());
      if (File.has_parent_path() || File.is_absolute() || Extension == ".a" ||
          Extension == ".lib" || Extension == ".so" || Extension == ".dylib" ||
          Library.contains(".so."))
        External.insert(
            (File.is_absolute() ? File : Provider.Input.Project.Root / File)
                .lexically_normal());
    }
  };
  Settings(Node);
  for (const auto &Identity : Node.LinkOrder)
    if (Graph.Projects.at(Identity).Input.Project.Type !=
        ProductType::SharedLibrary)
      Settings(Graph.Projects.at(Identity));
  for (const auto &Input : Action.Inputs)
    if (!External.contains(Input.lexically_normal()))
      Arguments.push_back(pathText(Input));
  for (const auto &[Name, Object] : StandardObjects)
    Arguments.push_back(pathText(Object));
  auto Append = [&](const ProjectNode &Provider) {
    for (const auto &Library : Provider.Settings.Libraries) {
      auto File = pathFromText(Library);
      if (File.extension() == ".framework")
        Arguments.insert(Arguments.end(),
                         {"-framework", pathText(File.stem())});
      else if (External.contains((File.is_absolute()
                                      ? File
                                      : Provider.Input.Project.Root / File)
                                     .lexically_normal()))
        Arguments.push_back(pathText(
            File.is_absolute() ? File : Provider.Input.Project.Root / File));
      else if (isMsvcTarget(Build.BuildTarget))
        Arguments.push_back(pathText(windowsLibrary(
            targetToolchain(Toolchain, Build.BuildTarget), Library)));
      else
        Arguments.emplace_back("-l" + Library);
    }
    Arguments.insert(Arguments.end(), Provider.Settings.Arguments.Link.begin(),
                     Provider.Settings.Arguments.Link.end());
  };
  Append(Node);
  for (const auto &Identity : Node.LinkOrder)
    if (Graph.Projects.at(Identity).Input.Project.Type !=
        ProductType::SharedLibrary)
      Append(Graph.Projects.at(Identity));
  bool Windows = isWindowsTarget(Build.BuildTarget);
  const auto &Output = Action.Outputs.front();
  if (isMsvcTarget(Build.BuildTarget) && Build.Mode == Profile::Debug) {
    const auto &Pdb = Action.Outputs.back();
    Arguments.insert(Arguments.end(),
                     {"-Wl,/debug", "-Wl,/pdb:" + pathText(Pdb)});
  }
  if (Node.Input.Project.Type == ProductType::SharedLibrary) {
    if (isMsvcTarget(Build.BuildTarget))
      Arguments.insert(
          Arguments.end(),
          {"-shared", "-Wl,/implib:" + pathText(Action.Outputs.at(1))});
    else if (Windows)
      Arguments.insert(
          Arguments.end(),
          {"-shared", "-Wl,--out-implib," + pathText(Action.Outputs.at(1))});
    else if (Build.BuildTarget == Target::MacosArm64)
      Arguments.insert(Arguments.end(),
                       {"-dynamiclib", "-Wl,-install_name,@rpath/" +
                                           pathText(Output.filename())});
    else
      Arguments.insert(
          Arguments.end(),
          {"-shared", "-Wl,-soname," + pathText(Output.filename())});
  }
  if (!Windows)
    Arguments.emplace_back(Build.BuildTarget == Target::MacosArm64
                               ? "-Wl,-rpath,@loader_path"
                               : "-Wl,-rpath,$ORIGIN");
  auto Defaults = defaultLinkArguments(
      targetToolchain(Toolchain, Build.BuildTarget), Build.BuildTarget, Cxx);
  Arguments.insert(Arguments.end(), Defaults.begin(), Defaults.end());
  Arguments.insert(Arguments.end(), {"-o", pathText(Output)});
  return Arguments;
}
}
