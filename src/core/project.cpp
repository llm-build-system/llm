module llm.core;

import std;

namespace llm {
namespace {
template <class T>
void append(std::vector<T> &Into, const std::vector<T> &From) {
  Into.insert(Into.end(), From.begin(), From.end());
}

bool hasControls(std::string_view Text) {
  return Text.find_first_of("\r\n\0", 0, 3) != std::string_view::npos;
}

Result<void> validateLayer(const Layer &Value, ProductType Type) {
  if (Type != ProductType::Test &&
      (Value.TestArguments || Value.WorkingDirectory || Value.Timeout ||
       !Value.Environment.empty()))
    return std::unexpected(Diagnostic{"Test settings require type = 'test'"});
  if (Value.Timeout &&
      (!std::isfinite(*Value.Timeout) || *Value.Timeout < 0.001 ||
       *Value.Timeout > 9223372036854775.0))
    return std::unexpected(Diagnostic{"Timeout must be finite and positive"});
  if ((Value.CStandard && standardLanguage(*Value.CStandard) != Language::C) ||
      (Value.CxxStandard &&
       standardLanguage(*Value.CxxStandard) != Language::Cxx))
    return std::unexpected(
        Diagnostic{"Language standard is assigned to the wrong language"});
  for (const auto *Patterns :
       {&Value.Sources, &Value.Excludes, &Value.Resources})
    for (const auto &Pattern : *Patterns)
      if (!isRelativeSafe(pathFromText(Pattern)) || Pattern.size() > 4096)
        return std::unexpected(Diagnostic{
            "Source and resource patterns must stay inside the project"});
  for (const auto *Arguments :
       {&Value.Arguments.Compile, &Value.Arguments.C, &Value.Arguments.Cxx,
        &Value.Arguments.Assembly, &Value.Arguments.Link})
    if (auto Valid = validateFlags(*Arguments); !Valid)
      return Valid;
  for (const auto &[Name, Text] : Value.Environment)
    if (Name.empty() ||
        Name.find_first_of("=\r\n\0", 0, 4) != std::string::npos ||
        Text.find('\0') != std::string::npos)
      return std::unexpected(Diagnostic{"Invalid test environment entry"});
  for (const auto &Rule : Value.Generators) {
    if (Rule.Outputs.empty() || Rule.Command.empty() ||
        Rule.Command.front().empty())
      return std::unexpected(Diagnostic{
          "Generator outputs and command must be nonempty", Rule.Location});
    for (const auto &Input : Rule.Inputs)
      if (!isRelativeSafe(pathFromText(Input)))
        return std::unexpected(Diagnostic{
            "Generator inputs must stay inside the project", Rule.Location});
    std::set<std::string> Seen;
    for (const auto &Output : Rule.Outputs) {
      auto File = pathFromText(Output);
      if (!isRelativeSafe(File) || !Output.starts_with("obj/") ||
          Output.starts_with("obj/.llm") || Output == "obj/.owned" ||
          Output == "obj/.guard" ||
          Output.find_first_of("*?") != std::string::npos ||
          !Seen.insert(Output).second)
        return std::unexpected(Diagnostic{
            "Generator outputs must be unique explicit files under obj/",
            Rule.Location});
    }
    for (const auto &Argument : Rule.Command)
      if (hasControls(Argument))
        return std::unexpected(Diagnostic{
            "Generator arguments contain a newline or NUL", Rule.Location});
  }
  return {};
}

void merge(EffectiveSettings &Into, const Layer &From) {
  append(Into.Sources, From.Sources);
  for (const auto &[Source, Inputs] : From.Inputs)
    Into.Inputs.insert_or_assign(Source, Inputs);
  append(Into.Excludes, From.Excludes);
  append(Into.Libraries, From.Libraries);
  append(Into.Resources, From.Resources);
  for (auto Pair : {std::pair{&Into.Includes, &From.Includes},
                    std::pair{&Into.Defines, &From.Defines},
                    std::pair{&Into.Dependencies, &From.Dependencies}}) {
    append(Pair.first->Private, Pair.second->Private);
    append(Pair.first->Public, Pair.second->Public);
  }
  if (From.CStandard)
    Into.CStandard = *From.CStandard;
  if (From.CxxStandard)
    Into.CxxStandard = *From.CxxStandard;
  append(Into.Arguments.C, From.Arguments.Compile);
  append(Into.Arguments.C, From.Arguments.C);
  append(Into.Arguments.Cxx, From.Arguments.Compile);
  append(Into.Arguments.Cxx, From.Arguments.Cxx);
  append(Into.Arguments.Assembly, From.Arguments.Assembly);
  append(Into.Arguments.Link, From.Arguments.Link);
  append(Into.Generators, From.Generators);
  if (From.TestArguments)
    Into.TestArguments = *From.TestArguments;
  if (From.WorkingDirectory)
    Into.WorkingDirectory = From.WorkingDirectory;
  if (From.Timeout)
    Into.Timeout = From.Timeout;
  for (const auto &[Name, Value] : From.Environment)
    Into.Environment[Name] = Value;
}
}

std::string_view productTypeName(ProductType Type) {
  switch (Type) {
  case ProductType::Executable:
    return "executable";
  case ProductType::StaticLibrary:
    return "static-library";
  case ProductType::SharedLibrary:
    return "shared-library";
  case ProductType::HeaderOnlyLibrary:
    return "header-only-library";
  case ProductType::Test:
    return "test";
  }
  std::unreachable();
}

Result<ProductType> parseProductType(std::string_view Name) {
  for (auto Type : {ProductType::Executable, ProductType::StaticLibrary,
                    ProductType::SharedLibrary, ProductType::HeaderOnlyLibrary,
                    ProductType::Test})
    if (productTypeName(Type) == Name)
      return Type;
  return std::unexpected(
      Diagnostic{"Unknown product type '" + std::string(Name) + "'"});
}

bool isLibrary(ProductType Type) {
  return Type == ProductType::StaticLibrary ||
         Type == ProductType::SharedLibrary ||
         Type == ProductType::HeaderOnlyLibrary;
}

std::string_view targetName(Target Value) {
  switch (Value) {
  case Target::WindowsX64:
    return "x86_64-pc-windows-msvc";
  case Target::WindowsArm64:
    return "aarch64-pc-windows-msvc";
  case Target::LinuxX64:
    return "x86_64-unknown-linux-gnu";
  case Target::LinuxArm64:
    return "aarch64-unknown-linux-gnu";
  case Target::MacosArm64:
    return "aarch64-apple-darwin";
  case Target::WindowsGnuX64:
    return "x86_64-w64-windows-gnu";
  case Target::WindowsGnuArm64:
    return "aarch64-w64-windows-gnu";
  case Target::LinuxMuslX64:
    return "x86_64-unknown-linux-musl";
  case Target::LinuxMuslArm64:
    return "aarch64-unknown-linux-musl";
  }
  std::unreachable();
}

Result<Target> parseTarget(std::string_view Name) {
  std::vector<std::string_view> Parts;
  for (auto Part : Name | std::views::split('-'))
    Parts.emplace_back(Part.begin(), Part.end());
  if (Parts.size() == 3 || Parts.size() == 4) {
    auto Arch = Parts[0];
    bool Arm = Arch == "aarch64" || Arch == "arm64";
    bool X64 = Arch == "x86_64" || Arch == "amd64";
    auto Vendor = Parts[1], Os = Parts[2];
    if (Arm || X64) {
      if (Parts.size() == 3 && Arm && Vendor == "apple" &&
          (Os == "darwin" || Os == "macos" || Os == "macosx"))
        return Target::MacosArm64;
      if (Parts.size() == 3 && Vendor == "w64" && Os == "mingw32")
        return Arm ? Target::WindowsGnuArm64 : Target::WindowsGnuX64;
      if (Parts.size() == 4) {
        auto Abi = Parts[3];
        if (Os == "windows" || Os == "win32") {
          if (Vendor == "pc" && Abi == "msvc")
            return Arm ? Target::WindowsArm64 : Target::WindowsX64;
          if ((Vendor == "pc" || Vendor == "w64") && Abi == "gnu")
            return Arm ? Target::WindowsGnuArm64 : Target::WindowsGnuX64;
        }
        if (Vendor == "unknown" && Os == "linux") {
          if (Abi == "gnu")
            return Arm ? Target::LinuxArm64 : Target::LinuxX64;
          if (Abi == "musl")
            return Arm ? Target::LinuxMuslArm64 : Target::LinuxMuslX64;
        }
      }
    }
  }
  return std::unexpected(
      Diagnostic{"Unsupported target: " + std::string(Name)});
}

bool isMsvcTarget(Target Value) {
  return Value == Target::WindowsX64 || Value == Target::WindowsArm64;
}

bool isWindowsTarget(Target Value) {
  return isMsvcTarget(Value) || Value == Target::WindowsGnuX64 ||
         Value == Target::WindowsGnuArm64;
}

bool isMuslTarget(Target Value) {
  return Value == Target::LinuxMuslX64 || Value == Target::LinuxMuslArm64;
}

bool isArm64Target(Target Value) {
  return Value == Target::WindowsArm64 || Value == Target::WindowsGnuArm64 ||
         Value == Target::LinuxArm64 || Value == Target::LinuxMuslArm64 ||
         Value == Target::MacosArm64;
}

std::string_view standardName(Standard Value) {
  switch (Value) {
  case Standard::C89:
    return "C89";
  case Standard::C90:
    return "C90";
  case Standard::C99:
    return "C99";
  case Standard::C11:
    return "C11";
  case Standard::C17:
    return "C17";
  case Standard::C23:
    return "C23";
  case Standard::Cxx98:
    return "C++98";
  case Standard::Cxx03:
    return "C++03";
  case Standard::Cxx11:
    return "C++11";
  case Standard::Cxx14:
    return "C++14";
  case Standard::Cxx17:
    return "C++17";
  case Standard::Cxx20:
    return "C++20";
  case Standard::Cxx23:
    return "C++23";
  }
  std::unreachable();
}

Result<Standard> parseStandard(std::string_view Name) {
  for (auto Value : {Standard::C89, Standard::C90, Standard::C99, Standard::C11,
                     Standard::C17, Standard::C23, Standard::Cxx98,
                     Standard::Cxx03, Standard::Cxx11, Standard::Cxx14,
                     Standard::Cxx17, Standard::Cxx20, Standard::Cxx23})
    if (standardName(Value) == Name)
      return Value;
  return std::unexpected(
      Diagnostic{"Unknown published language standard: " + std::string(Name)});
}

Language standardLanguage(Standard Value) {
  return Value <= Standard::C23 ? Language::C : Language::Cxx;
}

std::string pathText(const Path &Value) {
  auto Text = Value.generic_u8string();
  return {reinterpret_cast<const char *>(Text.data()), Text.size()};
}

Path pathFromText(std::string_view Value) {
  return Path(std::u8string(reinterpret_cast<const char8_t *>(Value.data()),
                            Value.size()));
}

bool isRelativeSafe(const Path &Value) {
  auto Text = pathText(Value);
  if (Value.empty() || Value.is_absolute() || Value.has_root_name() ||
      Value.lexically_normal() != Value || hasControls(Text) ||
      Text.starts_with('/') || Text.starts_with('\\') ||
      (Text.size() > 1 && Text[1] == ':'))
    return false;
  for (const auto &Part : Value)
    if (Part == ".." || Part == ".")
      return false;
  return true;
}

bool isProductName(std::string_view Name) {
  if (Name.empty() || Name.size() > 128 || Name.front() == '-' ||
      Name.back() == '.' || Name.back() == ' ')
    return false;
  for (unsigned char Byte : Name)
    if (!((Byte >= 'a' && Byte <= 'z') || (Byte >= 'A' && Byte <= 'Z') ||
          (Byte >= '0' && Byte <= '9') || Byte >= 128 || Byte == '_' ||
          Byte == '-' || Byte == '.' || Byte == ' '))
      return false;
  std::string Stem(Name.substr(0, Name.find('.')));
  for (auto &Byte : Stem)
    if (Byte >= 'a' && Byte <= 'z')
      Byte -= 'a' - 'A';
  return Stem != "CON" && Stem != "PRN" && Stem != "AUX" && Stem != "NUL" &&
         !(Stem.size() == 4 &&
           (Stem.starts_with("COM") || Stem.starts_with("LPT")) &&
           Stem[3] >= '1' && Stem[3] <= '9');
}

std::optional<Language> sourceLanguage(const Path &File) {
  auto Extension = pathText(File.extension());
  if (Extension == ".c")
    return Language::C;
  if (Extension == ".s" || Extension == ".S")
    return Language::Assembly;
  for (auto Candidate :
       {".cpp", ".cc", ".cxx", ".C", ".CPP", ".CC", ".CXX", ".cp", ".c++",
        ".ixx", ".cppm", ".mpp", ".ccm", ".cxxm", ".c++m"})
    if (Extension == Candidate)
      return Language::Cxx;
  return {};
}

bool isModuleSource(const Path &File) {
  auto Extension = pathText(File.extension());
  return Extension == ".ixx" || Extension == ".cppm" || Extension == ".mpp" ||
         Extension == ".ccm" || Extension == ".cxxm" || Extension == ".c++m";
}

bool isHeader(const Path &File) {
  auto Extension = pathText(File.extension());
  for (auto Candidate : {".h", ".H", ".hpp", ".hxx", ".hh", ".h++", ".inl",
                         ".ipp", ".tpp", ".inc"})
    if (Extension == Candidate)
      return true;
  return false;
}

bool matchesGlob(std::string_view Pattern, std::string_view Text) {
  if (Pattern.size() > 4096 || Text.size() > 32768)
    return false;
  std::array<std::vector<unsigned char>, 4> Rows;
  for (auto &Row : Rows)
    Row.resize(Text.size() + 1);
  Rows[Pattern.size() % 4][Text.size()] = 1;
  for (std::size_t Position = Pattern.size(); Position-- > 0;) {
    auto &Row = Rows[Position % 4];
    std::ranges::fill(Row, 0);
    for (std::size_t Offset = Text.size() + 1; Offset-- > 0;) {
      if (Pattern[Position] == '*') {
        bool Recursive =
            Position + 1 < Pattern.size() && Pattern[Position + 1] == '*';
        auto Next = Position + (Recursive ? 2 : 1);
        Row[Offset] = Rows[Next % 4][Offset] ||
                      (Recursive && Next < Pattern.size() &&
                       Pattern[Next] == '/' && Rows[(Next + 1) % 4][Offset]) ||
                      (Offset < Text.size() &&
                       (Recursive || Text[Offset] != '/') && Row[Offset + 1]);
      } else if (Offset < Text.size()) {
        Row[Offset] = ((Pattern[Position] == '?' && Text[Offset] != '/') ||
                       Pattern[Position] == Text[Offset]) &&
                      Rows[(Position + 1) % 4][Offset + 1];
      }
    }
  }
  return Rows[0][0];
}

Result<void> validateFlags(const Strings &Flags) {
  for (const auto &Flag : Flags) {
    if (hasControls(Flag))
      return std::unexpected(Diagnostic{
          "Newlines and NUL bytes are not supported in build arguments"});
    for (auto Exact :
         {"-c", "-S", "-E", "-x", "-target", "--target", "-arch", "--sysroot",
          "--output", "--config", "-stdlib", "-rtlib", "-unwindlib"})
      if (Flag == Exact)
        return std::unexpected(Diagnostic{
            "Reserved compiler control option in raw flags: " + Flag});
    for (auto Prefix : {"@",
                        "-o",
                        "--output=",
                        "-target=",
                        "--target=",
                        "-arch=",
                        "--sysroot=",
                        "-isysroot",
                        "-resource-dir",
                        "--config=",
                        "--no-default-config",
                        "--rsp-quoting",
                        "-B",
                        "-fuse-ld",
                        "--ld-path",
                        "-MF",
                        "-MT",
                        "-MQ",
                        "-MD",
                        "-MMD",
                        "-MJ",
                        "-save-temps",
                        "-fmodule",
                        "-fprebuilt-module",
                        "-std=",
                        "-stdlib=",
                        "-rtlib=",
                        "-unwindlib=",
                        "--gcc-toolchain",
                        "--gcc-install-dir",
                        "--driver-mode=",
                        "-m32",
                        "-mx32"})
      if (Flag.starts_with(Prefix))
        return std::unexpected(Diagnostic{
            "Reserved compiler control option in raw flags: " + Flag});
    if (Flag.starts_with("-Wl,") &&
        (Flag.contains("/out:") || Flag.contains("/pdb:") ||
         Flag.contains("--output") || Flag.contains(",-o")))
      return std::unexpected(Diagnostic{"Reserved linker output option"});
  }
  return {};
}

Result<void> validateManifest(const Manifest &Project) {
  if (!isProductName(Project.Name))
    return std::unexpected(
        Diagnostic{"Product name must be a portable filename"});
  if (auto Valid = validateLayer(Project.Base, Project.Type); !Valid)
    return Valid;
  for (const auto &[Scope, Value] : Project.Platforms)
    if (auto Valid = validateLayer(Value, Project.Type); !Valid)
      return Valid;
  for (const auto &[Alias, Url] : Project.Registries) {
    if (Alias.empty() || Alias.front() < 'a' || Alias.front() > 'z' ||
        !std::ranges::all_of(Alias, [](char Byte) {
          return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
                 Byte == '-';
        }))
      return std::unexpected(Diagnostic{"Invalid registry alias: " + Alias});
    if (auto Source = parseRegistrySource(Url); !Source)
      return std::unexpected(Source.error());
  }
  return {};
}

Result<EffectiveSettings> effectiveSettings(const Manifest &Project,
                                            Target BuildTarget) {
  if (auto Valid = validateManifest(Project); !Valid)
    return std::unexpected(Valid.error());
  EffectiveSettings Result;
  std::set<PlatformLayer> Active;
  merge(Result, Project.Base);
  auto Add = [&](PlatformLayer Scope) {
    Active.insert(Scope);
    if (auto Found = Project.Platforms.find(Scope);
        Found != Project.Platforms.end())
      merge(Result, Found->second);
  };
  switch (BuildTarget) {
  case Target::WindowsX64:
  case Target::WindowsGnuX64:
    Add(PlatformLayer::Windows);
    Add(PlatformLayer::WindowsX64);
    break;
  case Target::WindowsArm64:
  case Target::WindowsGnuArm64:
    Add(PlatformLayer::Windows);
    Add(PlatformLayer::WindowsArm64);
    break;
  case Target::LinuxX64:
  case Target::LinuxMuslX64:
    Add(PlatformLayer::Linux);
    Add(PlatformLayer::LinuxX64);
    break;
  case Target::LinuxArm64:
  case Target::LinuxMuslArm64:
    Add(PlatformLayer::Linux);
    Add(PlatformLayer::LinuxArm64);
    break;
  case Target::MacosArm64:
    Add(PlatformLayer::Macos);
    break;
  }
  if (isWindowsTarget(BuildTarget))
    Add(isMsvcTarget(BuildTarget) ? PlatformLayer::WindowsMsvc
                                  : PlatformLayer::WindowsGnu);
  else if (BuildTarget != Target::MacosArm64)
    Add(isMuslTarget(BuildTarget) ? PlatformLayer::LinuxMusl
                                  : PlatformLayer::LinuxGnu);
  for (const auto &[Scope, Layer] : Project.Platforms)
    if (!Active.contains(Scope))
      append(Result.InactiveSources, Layer.Sources);
  std::set<std::string> Outputs;
  for (const auto &Rule : Result.Generators)
    for (auto Output : Rule.Outputs) {
      if (isWindowsTarget(BuildTarget))
        for (auto &Byte : Output)
          if (Byte >= 'A' && Byte <= 'Z')
            Byte += 'a' - 'A';
      if (!Outputs.insert(Output).second)
        return std::unexpected(Diagnostic{
            "Conflicting generator output: " + Output, Rule.Location});
    }
  return Result;
}

Result<std::vector<Path>> selectSources(const EffectiveSettings &Settings,
                                        const std::vector<Path> &Inventory) {
  std::set<Path> Available(Inventory.begin(), Inventory.end()), Selected;
  for (const auto &File : Inventory) {
    if (!isRelativeSafe(File))
      return std::unexpected(
          Diagnostic{"Source inventory escapes its project"});
    if (!sourceLanguage(File))
      continue;
    auto Text = pathText(File);
    bool Inactive =
        std::ranges::any_of(Settings.InactiveSources, [&](const auto &Pattern) {
          return matchesGlob(Pattern, Text);
        });
    bool Additional =
        std::ranges::any_of(Settings.Sources, [&](const auto &Pattern) {
          return matchesGlob(Pattern, Text);
        });
    bool Excluded =
        std::ranges::any_of(Settings.Excludes, [&](const auto &Pattern) {
          return matchesGlob(Pattern, Text);
        });
    if ((!Inactive || Additional) && !Excluded)
      Selected.insert(File);
  }
  std::set<Path> Generated;
  for (const auto &Rule : Settings.Generators)
    for (const auto &Output : Rule.Outputs)
      Generated.insert(pathFromText(Output));
  for (const auto &Pattern : Settings.Sources)
    if (Pattern.find_first_of("*?") == std::string::npos) {
      auto File = pathFromText(Pattern);
      if (!Generated.contains(File) &&
          (!Available.contains(File) || !sourceLanguage(File)))
        return std::unexpected(Diagnostic{
            "Source is missing or has an unsupported extension: " + Pattern});
    }
  for (const auto &File : Generated)
    if (sourceLanguage(File) &&
        !std::ranges::any_of(Settings.Excludes, [&](const auto &Pattern) {
          return matchesGlob(Pattern, pathText(File));
        }))
      Selected.insert(File);
  return std::vector<Path>(Selected.begin(), Selected.end());
}

}
