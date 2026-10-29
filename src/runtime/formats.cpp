module;

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

module llm.runtime;

import :internal;
import std;

namespace llm::runtime {
namespace {
struct ManifestReader {
  Manifest Value;
  Path File;

  [[noreturn]] void fail(const toml::node &Node, std::string Message) const {
    throw Failure(Diagnostic{
        std::move(Message),
        {File, Node.source().begin.line, Node.source().begin.column}});
  }

  std::string string(const toml::node &Node) const {
    auto Value = Node.value<std::string>();
    if (!Value)
      fail(Node, "Expected a string");
    if (Value->find_first_of("\r\n\0", 0, 3) != std::string::npos)
      fail(Node,
           "Newlines and NUL bytes are not supported in arguments or paths");
    return std::move(*Value);
  }

  Strings strings(const toml::node &Node) const {
    auto Array = Node.as_array();
    if (!Array)
      fail(Node, "Expected an array of strings");
    Strings Result;
    for (const auto &Entry : *Array)
      Result.push_back(string(Entry));
    return Result;
  }

  const toml::table &table(const toml::node &Node) const {
    auto Table = Node.as_table();
    if (!Table)
      fail(Node, "Expected a table");
    return *Table;
  }

  void record(const toml::node &Node, const std::string &Key) {
    Value.Origins[Key] = {File, Node.source().begin.line,
                          Node.source().begin.column};
    if (auto Table = Node.as_table())
      for (const auto &[Name, Child] : *Table)
        record(Child, Key.empty() ? std::string(Name.str())
                                  : Key + "." + std::string(Name.str()));
    else if (auto Array = Node.as_array())
      for (std::size_t Index = 0; Index < Array->size(); ++Index)
        record((*Array)[Index], Key + "[" + std::to_string(Index) + "]");
  }

  Visibility visibility(const toml::node &Node) const {
    Visibility Result;
    for (const auto &[Key, Child] : table(Node)) {
      if (Key == "private")
        Result.Private = strings(Child);
      else if (Key == "public")
        Result.Public = strings(Child);
      else
        fail(Child, "Unknown visibility key '" + std::string(Key.str()) + "'");
    }
    return Result;
  }

  Flags flags(const toml::node &Node) const {
    Flags Result;
    for (const auto &[Key, Child] : table(Node)) {
      auto Arguments = strings(Child);
      if (auto Valid = validateFlags(Arguments); !Valid)
        fail(Child, Valid.error().Message);
      if (Key == "compile")
        Result.Compile = std::move(Arguments);
      else if (Key == "c")
        Result.C = std::move(Arguments);
      else if (Key == "cxx")
        Result.Cxx = std::move(Arguments);
      else if (Key == "asm")
        Result.Assembly = std::move(Arguments);
      else if (Key == "link")
        Result.Link = std::move(Arguments);
      else
        fail(Child, "Unknown flags key '" + std::string(Key.str()) + "'");
    }
    return Result;
  }

  void patterns(const toml::node &Node, const Strings &Values) const {
    for (const auto &Text : Values)
      if (!isRelativeSafe(pathFromText(Text)))
        fail(Node, "Source and resource patterns must stay inside the project");
  }

  std::vector<Generator> generators(const toml::node &Node) const {
    auto Array = Node.as_array();
    if (!Array)
      fail(Node, "Expected [[generate]] array of tables");
    std::vector<Generator> Result;
    for (const auto &Entry : *Array) {
      const auto &Definition = table(Entry);
      for (const auto &[Key, Child] : Definition)
        if (Key != "inputs" && Key != "outputs" && Key != "command")
          fail(Child, "Unknown generator key '" + std::string(Key.str()) + "'");
      for (auto Required : {"inputs", "outputs", "command"})
        if (!Definition.contains(Required))
          fail(Entry, "Generator requires inputs, outputs and command");
      Generator Rule{
          strings(*Definition.get("inputs")),
          strings(*Definition.get("outputs")),
          strings(*Definition.get("command")),
          {File, Entry.source().begin.line, Entry.source().begin.column}};
      patterns(Entry, Rule.Inputs);
      Result.push_back(std::move(Rule));
    }
    return Result;
  }

  Layer layer(const toml::table &Definition, std::string_view Prefix, bool Root,
              bool Os) {
    Layer Result;
    for (const auto &[Key, Child] : Definition) {
      auto Name = std::string(Key.str());
      if (Root && (Name == "type" || Name == "name" || Name == "registries" ||
                   Name == "windows" || Name == "linux" || Name == "macos"))
        continue;
      if (Os && Prefix != "macos" &&
          (Name == "x86_64" || Name == "aarch64" || Name == "gnu" ||
           (Prefix == "windows" && Name == "msvc") ||
           (Prefix == "linux" && Name == "musl")))
        continue;
      if (Name == "sources" || Name == "excludes" || Name == "resources") {
        auto Values = strings(Child);
        patterns(Child, Values);
        (Name == "sources"    ? Result.Sources
         : Name == "excludes" ? Result.Excludes
                              : Result.Resources) = std::move(Values);
      } else if (Name == "inputs") {
        for (const auto &[Source, Files] : table(Child)) {
          auto Path = std::string(Source.str());
          if (!isRelativeSafe(pathFromText(Path)) ||
              Path.find_first_of("*?") != std::string::npos)
            fail(Files, "Input mappings require explicit project-relative "
                        "source paths");
          auto Values = strings(Files);
          patterns(Files, Values);
          for (const auto &Value : Values)
            if (Value.find_first_of("*?") != std::string::npos)
              fail(Files, "Additional inputs must be explicit files");
          Result.Inputs.emplace(std::move(Path), std::move(Values));
        }
      } else if (Name == "libraries") {
        Result.Libraries = strings(Child);
      } else if (Name == "standards") {
        for (const auto &Text : strings(Child)) {
          auto Standard = parseStandard(Text);
          if (!Standard)
            fail(Child, Standard.error().Message);
          auto &Destination = standardLanguage(*Standard) == Language::C
                                  ? Result.CStandard
                                  : Result.CxxStandard;
          if (Destination)
            fail(Child, "Choose only one standard per language");
          Destination = *Standard;
        }
      } else if (Name == "includes") {
        Result.Includes = visibility(Child);
      } else if (Name == "defines") {
        Result.Defines = visibility(Child);
      } else if (Name == "dependencies") {
        Result.Dependencies = visibility(Child);
      } else if (Name == "flags") {
        Result.Arguments = flags(Child);
      } else if (Name == "generate") {
        Result.Generators = generators(Child);
      } else if (Name == "arguments" || Name == "working-directory" ||
                 Name == "timeout" || Name == "environment") {
        if (Value.Type != ProductType::Test)
          fail(Child, "'" + Name + "' is only valid for type = 'test'");
        if (Name == "arguments")
          Result.TestArguments = strings(Child);
        else if (Name == "working-directory")
          Result.WorkingDirectory = string(Child);
        else if (Name == "timeout") {
          if (!Child.is_integer() && !Child.is_floating_point())
            fail(Child, "Expected positive finite timeout in seconds");
          auto Seconds = Child.value<double>();
          if (!Seconds || !std::isfinite(*Seconds) || *Seconds < 0.001 ||
              *Seconds > 9223372036854775.0)
            fail(Child, "Timeout must be finite and at least 0.001 seconds");
          Result.Timeout = *Seconds;
        } else {
          for (const auto &[Variable, Item] : table(Child)) {
            auto VariableName = std::string(Variable.str());
            auto Text = Item.value<std::string>();
            if (VariableName.empty() ||
                VariableName.find_first_of("=\r\n\0", 0, 4) !=
                    std::string::npos ||
                !Text || Text->find('\0') != std::string::npos)
              fail(Item, "Invalid test environment entry");
            Result.Environment.emplace(std::move(VariableName),
                                       std::move(*Text));
          }
        }
      } else {
        fail(Child, "Unknown option '" + Name + "'");
      }
    }
    return Result;
  }
};

bool validHash(std::string_view Text) {
  return Text.size() == 64 && std::ranges::all_of(Text, [](char Byte) {
           return (Byte >= '0' && Byte <= '9') || (Byte >= 'a' && Byte <= 'f');
         });
}

void validateIdentity(const std::string &Identity) {
  auto Separator = Identity.rfind('|');
  if (Separator == std::string::npos)
    throw Failure("Invalid source-bound package identity in .lock");
  auto Source = take(parseRegistrySource(Identity.substr(0, Separator)));
  Manifest Owner;
  Owner.Registries.emplace("source", Source.Url);
  auto Reference = take(dependencyReference(
      Owner, "source:" + Identity.substr(Separator + 1), false));
  if (packageIdentity(Source, Reference.Package) != Identity)
    throw Failure("Noncanonical package identity in .lock");
}
}

Manifest parseManifest(std::string_view Text, const Path &File) {
  toml::table Document;
  try {
    Document = toml::parse(Text, pathText(File));
  } catch (const toml::parse_error &Error) {
    throw Failure(Diagnostic{
        std::string(Error.description()),
        {File, Error.source().begin.line, Error.source().begin.column}});
  }
  ManifestReader Reader{{}, File};
  Reader.Value.Root =
      std::filesystem::absolute(File).parent_path().lexically_normal();
  auto Type = Document.get("type");
  if (!Type)
    Reader.fail(Document, "Manifest requires type");
  auto ParsedType = parseProductType(Reader.string(*Type));
  if (!ParsedType)
    Reader.fail(*Type, ParsedType.error().Message);
  Reader.Value.Type = *ParsedType;
  Reader.Value.Name = pathText(Reader.Value.Root.filename());
  if (auto Name = Document.get("name"))
    Reader.Value.Name = Reader.string(*Name);
  Reader.Value.Base = Reader.layer(Document, "", true, false);
  for (auto [Name, Scope, X64, Arm64] :
       {std::tuple{"windows", PlatformLayer::Windows, PlatformLayer::WindowsX64,
                   PlatformLayer::WindowsArm64},
        std::tuple{"linux", PlatformLayer::Linux, PlatformLayer::LinuxX64,
                   PlatformLayer::LinuxArm64},
        std::tuple{"macos", PlatformLayer::Macos, PlatformLayer::Macos,
                   PlatformLayer::Macos}}) {
    auto Node = Document.get(Name);
    if (!Node)
      continue;
    const auto &Definition = Reader.table(*Node);
    Reader.Value.Platforms.emplace(Scope,
                                   Reader.layer(Definition, Name, false, true));
    if (Scope == PlatformLayer::Macos)
      continue;
    for (auto [Abi, AbiScope] :
         {std::pair{"gnu", Scope == PlatformLayer::Windows
                               ? PlatformLayer::WindowsGnu
                               : PlatformLayer::LinuxGnu},
          std::pair{Scope == PlatformLayer::Windows ? "msvc" : "musl",
                    Scope == PlatformLayer::Windows
                        ? PlatformLayer::WindowsMsvc
                        : PlatformLayer::LinuxMusl}})
      if (auto Child = Definition.get(Abi))
        Reader.Value.Platforms.emplace(
            AbiScope, Reader.layer(Reader.table(*Child), Abi, false, false));
    for (auto [Arch, ArchScope] :
         {std::pair{"x86_64", X64}, std::pair{"aarch64", Arm64}})
      if (auto Child = Definition.get(Arch))
        Reader.Value.Platforms.emplace(
            ArchScope, Reader.layer(Reader.table(*Child), Arch, false, false));
  }
  if (auto Registries = Document.get("registries"))
    for (const auto &[Alias, Value] : Reader.table(*Registries)) {
      auto Url = Reader.string(Value);
      if (auto Source = parseRegistrySource(Url); !Source)
        Reader.fail(Value, Source.error().Message);
      Reader.Value.Registries.emplace(std::string(Alias.str()), std::move(Url));
    }
  Reader.record(Document, "");
  if (auto Valid = validateManifest(Reader.Value); !Valid) {
    auto Error = std::move(Valid.error());
    if (Error.Location.File.empty())
      Error.Location = {File, Document.source().begin.line,
                        Document.source().begin.column};
    throw Failure(std::move(Error));
  }
  return std::move(Reader.Value);
}

Manifest readManifest(const Path &Root) {
  return parseManifest(readText(Root / ".llm"), Root / ".llm");
}

std::string jsonString(std::string_view Text) {
  return nlohmann::json(std::string(Text)).dump();
}

LockData parseLock(std::string_view Text) {
  LockData Result;
  std::istringstream Stream{std::string(Text)};
  std::string Line;
  std::string Registry;
  std::set<std::string> Registries;
  std::size_t Records = 0;
  while (std::getline(Stream, Line)) {
    if (++Records > 20000 || Line.size() > 16384)
      throw Failure(".lock exceeds size limits");
    if (Line.ends_with('\r'))
      Line.pop_back();
    if (Line.empty())
      continue;
    if (Line.contains("://")) {
      Registry = take(parseRegistrySource(Line)).Url;
      if (!Registries.insert(Registry).second)
        throw Failure("Duplicate registry block in .lock");
    } else {
      if (Registry.empty())
        throw Failure("Package precedes registry address in .lock");
      auto At = Line.rfind('@');
      auto Colon = Line.rfind(':');
      if (At == std::string::npos || Colon == std::string::npos || Colon < At)
        throw Failure("Invalid package record in .lock");
      auto Identity = Registry + "|" + Line.substr(0, At);
      validateIdentity(Identity);
      PackageVersion Value{
          take(parseVersion(Line.substr(At + 1, Colon - At - 1))),
          {},
          Line.substr(Colon + 1)};
      if (!validHash(Value.Sha256) ||
          !Result.Packages.emplace(Identity, std::move(Value)).second)
        throw Failure("Invalid or duplicate package in .lock");
      continue;
    }
  }
  return Result;
}

std::string lockText(const LockData &Lock) {
  std::ostringstream Text;
  std::map<std::string, std::map<std::string, PackageVersion>> Registries;
  for (const auto &[Identity, Value] : Lock.Packages) {
    auto Separator = Identity.rfind('|');
    Registries[Identity.substr(0, Separator)].emplace(
        Identity.substr(Separator + 1), Value);
  }
  bool First = true;
  for (const auto &[Registry, Packages] : Registries) {
    if (!First)
      Text << '\n';
    First = false;
    Text << Registry << '\n';
    for (const auto &[Name, Value] : Packages)
      Text << Name << '@' << Value.Selected.Text << ':' << Value.Sha256 << '\n';
  }
  return Text.str();
}

}

namespace llm::runtime {
std::optional<ImportedProduct> readImportedProduct(const Path &Root,
                                                   Target BuildTarget) {
  auto File = Root / ".llm-export";
  rejectSymlinkAncestors(File);
  if (!std::filesystem::exists(File))
    return {};
  toml::table Document;
  try {
    Document = toml::parse(readText(File), pathText(File));
  } catch (const toml::parse_error &) {
    throw Failure("Invalid development-package metadata: " + pathText(File));
  }
  const std::set<std::string> Keys{
      "artifact", "import-library",  "target", "profile", "toolchain",
      "cpp",      "standard-objects"};
  for (const auto &[Key, Value] : Document)
    if (!Keys.contains(std::string(Key.str())))
      throw Failure("Unknown development-package metadata field");
  auto Architecture = Document["target"].value<std::string>(),
       Mode = Document["profile"].value<std::string>(),
       Toolchain = Document["toolchain"].value<std::string>(),
       Artifact = Document["artifact"].value<std::string>();
  auto Cxx = Document["cpp"].value<bool>();
  if (!Architecture || !Mode || !Toolchain || !Artifact || !Cxx ||
      (*Mode != "debug" && *Mode != "release") ||
      *Architecture != targetName(BuildTarget))
    throw Failure("Development package is incompatible with target " +
                  std::string(targetName(BuildTarget)) + ": " + pathText(Root));
  auto Checked = [&](const std::string &Text) {
    auto Relative = pathFromText(Text);
    if (!isRelativeSafe(Relative))
      throw Failure("Development-package artifact escapes its root");
    rejectSymlinkAncestors(Root / Relative);
    if (!std::filesystem::is_regular_file(Root / Relative))
      throw Failure("Development-package artifact is missing: " +
                    pathText(Root / Relative));
    return Relative;
  };
  ImportedProduct Product;
  Product.Artifact = Checked(*Artifact);
  Product.BuildTarget = BuildTarget;
  Product.Mode = *Mode == "release" ? Profile::Release : Profile::Debug;
  Product.ToolchainIdentity = *Toolchain;
  Product.UsesCxx = *Cxx;
  if (auto Import = Document["import-library"].value<std::string>())
    Product.LinkArtifact = Checked(*Import);
  if (auto Objects = Document.get("standard-objects")) {
    auto Table = Objects->as_table();
    if (!Table)
      throw Failure("Invalid standard-library object metadata");
    for (const auto &[Key, Value] : *Table) {
      auto Name = std::string(Key.str());
      auto Object = Value.value<std::string>();
      if ((Name != "std" && Name != "std.compat") || !Object)
        throw Failure("Invalid standard-library object metadata");
      Product.StandardObjects.emplace(Name, Checked(*Object));
    }
  }
  return Product;
}
}
