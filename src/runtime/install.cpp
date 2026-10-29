module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
namespace {
struct Cleanup {
  fs::path Path;
  ~Cleanup() {
    std::error_code Ignored;
    fs::remove_all(nativePath(Path), Ignored);
  }
};
Path executableName() {
  return artifactName("llm", ProductType::Executable, nativeTarget());
}
InstallLayout layoutAtPrefix(const Path &Root) {
  auto Layout = systemLayout();
  if (Root == Layout.ProgramRoot ||
      (fs::exists(nativePath(Root)) &&
       fs::exists(nativePath(Layout.ProgramRoot)) &&
       fs::equivalent(nativePath(Root), nativePath(Layout.ProgramRoot))))
    return Layout;
  auto User = userLayout();
  if (Root == User.ProgramRoot ||
      (fs::exists(nativePath(Root)) &&
       fs::exists(nativePath(User.ProgramRoot)) &&
       fs::equivalent(nativePath(Root), nativePath(User.ProgramRoot))))
    return User;
  return {Root, Root, Root / "bin" / executableName(), false};
}
bool safeRelative(const fs::path &Path) {
  if (Path.empty() || Path.is_absolute() || Path.has_root_name())
    return false;
  for (const auto &Part : Path) {
    if (Part == ".." || Part == ".")
      return false;
#ifdef _WIN32
    auto Name = pathText(Part);
    if (Name.empty() || Name.back() == '.' || Name.back() == ' ' ||
        Name.find_first_of(":<>\"|?*") != std::string::npos)
      return false;
#endif
  }
  return true;
}
bool safeLink(const fs::path &Relative, const fs::path &Link) {
  return !Link.is_absolute() &&
         safeRelative((Relative.parent_path() / Link).lexically_normal());
}
void copyReleaseLicenses(const Path &Executable, const Path &Destination) {
  auto Source = Executable.parent_path();
  auto Copy = [&](const Path &Relative) {
    auto Input = Source / Relative;
    rejectSymlinkAncestors(Input);
    if (!fs::is_regular_file(nativePath(Input)) ||
        fs::file_size(nativePath(Input)) == 0)
      throw Failure("Release license file is missing or empty: " +
                    pathText(Input) + ". Extract the complete llm archive.");
    auto Output = Destination / Relative;
    fs::create_directories(nativePath(Output.parent_path()));
    fs::copy_file(nativePath(Input), nativePath(Output));
  };
  Copy("LICENSE");
  auto Licenses = Source / "licenses";
  rejectSymlinkAncestors(Licenses);
  if (!fs::is_directory(nativePath(Licenses)) ||
      fs::is_empty(nativePath(Licenses)))
    throw Failure("Release license directory is missing or empty: " +
                  pathText(Licenses) + ". Extract the complete llm archive.");
  for (const auto &Product : fs::directory_iterator(nativePath(Licenses))) {
    if (!fs::is_directory(Product.symlink_status()))
      throw Failure("Unexpected release license entry: " +
                    pathText(Product.path()));
    auto Relative = Path("licenses") / Product.path().filename();
    Copy(Relative / "LICENSE");
    for (const auto &File : fs::directory_iterator(Product.path())) {
      auto Name = File.path().filename();
      if (Name == "LICENSE")
        continue;
      if (Name != "NOTICE" || !fs::is_regular_file(File.symlink_status()))
        throw Failure("Unexpected release license file: " +
                      pathText(File.path()));
      Copy(Relative / Name);
    }
  }
}
void writeSdkSources(const Path &File, const ToolchainBundle &Bundle) {
  std::ostringstream Sources;
  for (const auto &Package : Bundle.Packages)
    Sources << Package.Id << '\n'
            << Package.Url << "\nSHA-256 " << Package.Sha256 << '\n';
  writeText(File, Sources.str());
}
std::unique_ptr<FileLock> lockInstallation(Context &Call,
                                           const InstallLayout &Layout) {
  auto File = Layout.DataRoot / ".guard";
  rejectSymlinkAncestors(File);
  return std::make_unique<FileLock>(Call, File);
}
void inventory(Context &Call, const Path &Root, std::ostream &Text,
               bool Base = false) {
  auto NativeRoot = nativePath(Root);
  for (fs::recursive_directory_iterator Entry(NativeRoot), End; Entry != End;
       ++Entry) {
    cancelled(Call);
    auto Relative = Entry->path().lexically_relative(NativeRoot);
    if (Relative == ".installation" ||
        (Base && Relative == Path("toolchain/targets"))) {
      Entry.disable_recursion_pending();
      continue;
    }
    auto Status = Entry->symlink_status();
    if (fs::is_symlink(Status)) {
      auto Link = fs::read_symlink(nativePath(Entry->path()));
      if (!safeLink(Relative, Link))
        throw Failure("Installed link escapes component directory");
      Text << "L " << std::quoted(pathText(Relative)) << ' '
           << std::quoted(pathText(Link)) << '\n';
    } else if (fs::is_regular_file(Status)) {
      Text << "F " << std::quoted(pathText(Relative)) << ' '
           << Entry->file_size() << ' '
           << static_cast<unsigned>(Status.permissions()) << '\n';
    } else if (!fs::is_directory(Status))
      throw Failure("Unexpected installed file type");
  }
}
void writeInstallation(Context &Call, const fs::path &Prefix,
                       const InstallRequest &Options) {
  std::ostringstream Text;
  Text << Options.Version << '\n'
       << bundleFingerprint(Options.Bundle) << '\n'
       << sha256File(Prefix / executableName()) << '\n';
  inventory(Call, Prefix, Text, true);
  writeText(Prefix / ".installation", Text.str());
#ifndef _WIN32
  fs::permissions(nativePath(Prefix / ".installation"),
                  static_cast<fs::perms>(0644));
#endif
}
std::vector<Path> readInventory(Context &Call, std::istream &Input,
                                const Path &Root, bool Verify,
                                std::vector<PathEntry> *Entries = nullptr) {
  std::vector<Path> Files;
  std::set<Path> Unique;
  char Type;
  std::string Name;
  while (Input >> Type) {
    cancelled(Call);
    if (!(Input >> std::quoted(Name)) || Files.size() >= 200000)
      throw Failure("Invalid installation inventory");
    if (Type == 'P') {
      std::string Text;
      unsigned Created, Separator;
      auto File = pathFromText(Name);
      if (!(Input >> std::quoted(Text) >> Created >> Separator) ||
          (!File.empty() && !File.is_absolute()) ||
          Name.find_first_of("\r\n") != std::string::npos || Text.empty() ||
          Text.find_first_of("\r\n") != std::string::npos || Created > 1 ||
          Separator > 1)
        throw Failure("Invalid installed PATH registration");
      if (Entries)
        Entries->push_back({File, Text, bool(Created), bool(Separator)});
      continue;
    }
    auto Relative = pathFromText(Name);
    if (!safeRelative(Relative) || Relative == ".installation" ||
        !Unique.insert(Relative).second)
      throw Failure("Unsafe installation inventory");
    auto File = Root / Relative;
    rejectSymlinkAncestors(File.parent_path());
    auto Status = fs::symlink_status(nativePath(File));
    if (Type == 'F') {
      std::uintmax_t Size;
      unsigned Permissions;
      if (!(Input >> Size >> Permissions) ||
          (fs::exists(Status) && !fs::is_regular_file(Status)))
        throw Failure("Changed file type in installation inventory");
      if (Verify &&
          (!fs::is_regular_file(Status) ||
           fs::file_size(nativePath(File)) != Size ||
           static_cast<unsigned>(Status.permissions()) != Permissions))
        throw Failure("Installed file is missing or changed: " +
                      pathText(File));
    } else if (Type == 'L') {
      std::string Link;
      if (!(Input >> std::quoted(Link)) ||
          !safeLink(Relative, pathFromText(Link)) ||
          (fs::exists(Status) && !fs::is_symlink(Status)))
        throw Failure("Unsafe link in installation inventory");
      if (Verify && (!fs::is_symlink(Status) ||
                     pathText(fs::read_symlink(nativePath(File))) != Link))
        throw Failure("Installed link is missing or changed");
    } else
      throw Failure("Invalid installation inventory entry");
    Files.push_back(File);
  }
  if (!Input.eof() || Files.empty())
    throw Failure("Incomplete installation inventory");
  return Files;
}
void removeEmptyDirectories(const Path &Root) {
  std::vector<Path> Directories;
  for (fs::recursive_directory_iterator Entry(nativePath(Root)), End;
       Entry != End; ++Entry) {
    auto Status = Entry->symlink_status();
    if (fs::is_symlink(Status))
      Entry.disable_recursion_pending();
    else if (fs::is_directory(Status))
      Directories.push_back(Entry->path());
  }
  std::ranges::sort(Directories, [](const auto &A, const auto &B) {
    return A.native().size() > B.native().size();
  });
  for (const auto &Directory : Directories)
    if (fs::is_empty(nativePath(Directory)))
      fs::remove(nativePath(Directory));
  if (fs::is_empty(nativePath(Root)))
    fs::remove(nativePath(Root));
}
void fetchBundle(Context &Call, const InstallLayout &Layout,
                 const ToolchainBundle &Bundle, const Path &Destination) {
  for (const auto &Package : Bundle.Packages) {
    cancelled(Call);
    if (Package.Sha256.size() != 64 ||
        !std::ranges::all_of(
            Package.Sha256, [](unsigned char C) { return std::isxdigit(C); }) ||
        (!Package.Destination.empty() && !safeRelative(Package.Destination)))
      throw Failure("Invalid component catalog");
    auto Cache = Layout.cache() / (Package.Sha256 + ".archive");
    rejectSymlinkAncestors(Cache);
    if (fs::exists(nativePath(Cache)) &&
        (!fs::is_regular_file(nativePath(Cache)) ||
         sha256File(Cache) != Package.Sha256))
      throw Failure("Corrupt SDK archive cache: " + pathText(Cache));
    if (!fs::exists(nativePath(Cache))) {
      auto Download = Layout.cache() / (".download-" + uniqueId());
      Cleanup Partial{Download};
      emit(Call, EventKind::Progress, "Downloading " + Package.Id + "...");
      downloadToolchainArchive(Call, Package.Url, Download,
                               Package.MaxDownload);
      if (sha256File(Download) != Package.Sha256)
        throw Failure("SHA-256 mismatch for downloaded package");
      fs::rename(nativePath(Download), nativePath(Cache));
    }
    emit(Call, EventKind::Progress, "Extracting " + Package.Id + "...");
    extractPackage(Call, Cache, Destination / Package.Destination, Package);
  }
}
void readableInstallation(const Path &Root) {
#ifndef _WIN32
  fs::permissions(nativePath(Root), static_cast<fs::perms>(0755));
  for (const auto &Entry : fs::recursive_directory_iterator(nativePath(Root))) {
    auto Status = Entry.symlink_status();
    if (fs::is_directory(Status))
      fs::permissions(nativePath(Entry.path()), static_cast<fs::perms>(0755));
    else if (fs::is_regular_file(Status))
      fs::permissions(
          nativePath(Entry.path()),
          static_cast<fs::perms>(0644) |
              (Status.permissions() & static_cast<fs::perms>(0111)));
  }
#else
  (void)Root;
#endif
}
void targetIdentity(std::istream &Input, Target BuildTarget) {
  std::string Triple, Catalog;
  if (!std::getline(Input, Triple) || !std::getline(Input, Catalog) ||
      Triple != targetName(BuildTarget) ||
      Catalog != bundleFingerprint(targetBundle(BuildTarget)))
    throw Failure("Target installation identity does not match its catalog");
}
void targetRequiredFiles(const Path &Root, Target BuildTarget) {
  for (const auto &Package : targetBundle(BuildTarget).Packages)
    for (const auto &File : Package.RequiredFiles)
      if (!fs::is_regular_file(nativePath(Root / Package.Destination / File)) ||
          fs::file_size(nativePath(Root / Package.Destination / File)) == 0)
        throw Failure("Target SDK component is missing");
  for (auto Name : {"clang.cfg", "clang++.cfg"})
    if (!fs::is_regular_file(nativePath(Root / Name)) ||
        !fs::file_size(nativePath(Root / Name)))
      throw Failure("Target compiler configuration is missing");
}
std::vector<Path> targetInventory(Context &Call, const Path &Root,
                                  Target BuildTarget, bool Verify) {
  rejectSymlinkAncestors(Root / ".installation");
  std::ifstream Input(nativePath(Root / ".installation"), std::ios::binary);
  targetIdentity(Input, BuildTarget);
  auto Files = readInventory(Call, Input, Root, Verify);
  if (Verify)
    targetRequiredFiles(Root, BuildTarget);
  return Files;
}
void prepareTarget(Context &Call, const InstallLayout &Layout,
                   const Path &Tools, const Path &Root, Target BuildTarget) {
  auto Bundle = targetBundle(BuildTarget);
  fs::create_directories(nativePath(Root));
  fetchBundle(Call, Layout, Bundle, Root);
  prepareTargetToolchain(Call, Tools, Root, BuildTarget);
  validateTargetToolchain(Call, Tools, Root, BuildTarget);
  writeSdkSources(Root / "sdk-sources.txt", Bundle);
  readableInstallation(Root);
  std::ostringstream Text;
  Text << targetName(BuildTarget) << '\n' << bundleFingerprint(Bundle) << '\n';
  inventory(Call, Root, Text);
  writeText(Root / ".installation", Text.str());
#ifndef _WIN32
  fs::permissions(nativePath(Root / ".installation"),
                  static_cast<fs::perms>(0644));
#endif
}
void checkCommandOwner(const InstallLayout &Layout) {
  auto Status = fs::symlink_status(nativePath(Layout.CommandPath));
  if (Status.type() == fs::file_type::not_found)
    return;
#ifdef _WIN32
  rejectSymlinkAncestors(Layout.CommandPath);
  if (fs::is_regular_file(Status)) {
    auto Hash = sha256File(Layout.CommandPath);
    for (const auto &Entry :
         fs::directory_iterator(nativePath(Layout.ProgramRoot / "versions"))) {
      auto Snapshot = Entry.path() / executableName();
      rejectSymlinkAncestors(Snapshot);
      if (fs::is_regular_file(nativePath(Snapshot)) &&
          sha256File(Snapshot) == Hash)
        return;
    }
  }
#else
  if (fs::is_symlink(Status)) {
    auto Target = fs::read_symlink(nativePath(Layout.CommandPath));
    if (!Target.is_absolute())
      Target = Layout.CommandPath.parent_path() / Target;
    auto Relative = Target.lexically_normal().lexically_relative(
        Layout.ProgramRoot / "versions");
    if (safeRelative(Relative) &&
        std::distance(Relative.begin(), Relative.end()) == 2 &&
        Relative.filename() == executableName())
      return;
  }
#endif
  throw Failure("Refusing to overwrite an existing foreign command: " +
                pathText(Layout.CommandPath));
}
void checkReleaseIdentity(const fs::path &Prefix,
                          const InstallRequest &Options) {
  rejectSymlinkAncestors(installationFile(Prefix));
  std::ifstream Receipt(nativePath(installationFile(Prefix)), std::ios::binary);
  std::string Version, Catalog, ExecutableHash;
  if (!fs::exists(nativePath(Prefix)))
    return;
  if (!readInstallIdentity(Receipt, Version, Catalog, ExecutableHash))
    throw Failure("Invalid existing installation identity");
  if (Version != Options.Version ||
      Catalog != bundleFingerprint(Options.Bundle))
    throw Failure("This release identity already belongs to a different "
                  "package catalog. Use a new release version");
  if (ExecutableHash != sha256File(Options.Executable))
    throw Failure("This release identity already belongs to a different "
                  "executable. Use a new release version");
}
}
Path installationFile(const Path &Prefix) {
  auto File = Prefix / ".installation";
  rejectSymlinkAncestors(File);
  return File;
}
bool readInstallIdentity(std::istream &Input, std::string &Version,
                         std::string &Catalog, std::string &ExecutableHash) {
  if (!std::getline(Input, Version) || Version.size() > 128)
    return false;
  auto Hash = [](std::string_view Value) {
    return Value.size() == 64 &&
           std::ranges::all_of(Value, [](unsigned char C) {
             return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
           });
  };
  return parseVersion(Version).has_value() && std::getline(Input, Catalog) &&
         std::getline(Input, ExecutableHash) && !Catalog.empty() &&
         Catalog.size() <= 1024 * 1024 && Hash(ExecutableHash);
}
bool completeTargetInstallation(Context &Call, const Path &Root,
                                Target BuildTarget, bool Audit) {
  rejectSymlinkAncestors(Root);
  if (!fs::exists(fs::symlink_status(nativePath(Root))))
    return false;
  cancelled(Call);
  if (Audit)
    targetInventory(Call, Root, BuildTarget, true);
  else {
    rejectSymlinkAncestors(Root / ".installation");
    std::ifstream Input(nativePath(Root / ".installation"), std::ios::binary);
    targetIdentity(Input, BuildTarget);
    targetRequiredFiles(Root, BuildTarget);
  }
  return true;
}
bool completeInstallation(Context &Call, const Path &Prefix,
                          const std::string &Version,
                          const ToolchainBundle &Bundle) {
  rejectSymlinkAncestors(Prefix);
  if (!fs::exists(fs::symlink_status(nativePath(Prefix))))
    return false;
  rejectSymlinkAncestors(installationFile(Prefix));
  std::ifstream Input(nativePath(installationFile(Prefix)), std::ios::binary);
  std::string Stored, Fingerprint, Hash;
  if (!readInstallIdentity(Input, Stored, Fingerprint, Hash) ||
      Stored != Version || Fingerprint != bundleFingerprint(Bundle))
    throw Failure("Installed release does not match its required identity");
  auto Files = readInventory(Call, Input, Prefix, true);
  if (std::ranges::find(Files, Prefix / executableName()) == Files.end())
    throw Failure("Installed executable is missing from inventory");
  for (const auto &Package : Bundle.Packages)
    for (const auto &File : Package.RequiredFiles) {
      auto Required = Prefix / "toolchain" / Package.Destination / File;
      if (!fs::is_regular_file(nativePath(Required)) ||
          !fs::file_size(nativePath(Required)))
        throw Failure("Required installed component is missing: " +
                      pathText(Required));
    }
  if (bundleFingerprint(Bundle) ==
          bundleFingerprint(hostBundle(nativeTarget())) &&
      !completeTargetInstallation(Call,
                                  Prefix / "toolchain/targets" /
                                      pathFromText(targetName(nativeTarget())),
                                  nativeTarget()))
    throw Failure("Native target is not installed");
  if (sha256File(Prefix / executableName()) != Hash)
    throw Failure("Installed executable hash mismatch");
  return true;
}
std::vector<PathEntry> registeredPaths(Context &Call,
                                       const InstallLayout &Layout) {
  std::vector<PathEntry> Result;
  auto Versions = Layout.ProgramRoot / "versions";
  if (!fs::exists(nativePath(Versions)))
    return Result;
  for (const auto &Entry : fs::directory_iterator(nativePath(Versions))) {
    auto File = Entry.path() / ".installation";
    rejectSymlinkAncestors(File);
    if (!fs::is_regular_file(nativePath(File)))
      continue;
    std::ifstream Input(nativePath(File), std::ios::binary);
    std::string Version, Catalog, Hash;
    if (!readInstallIdentity(Input, Version, Catalog, Hash))
      throw Failure("Invalid installed release identity");
    readInventory(Call, Input, Entry.path(), false, &Result);
  }
  return Result;
}
void saveRegisteredPaths(const Path &Prefix,
                         std::span<const PathEntry> Entries) {
  auto File = Prefix / ".installation";
  std::istringstream Input(readText(File));
  std::ostringstream Output;
  std::string Line;
  while (std::getline(Input, Line))
    if (!Line.starts_with("P "))
      Output << Line << '\n';
  for (const auto &Entry : Entries)
    Output << "P " << std::quoted(pathText(Entry.File)) << ' '
           << std::quoted(Entry.Text) << ' ' << Entry.Created << ' '
           << Entry.Separator << '\n';
  writeText(File, Output.str());
}
InstallResult installRelease(Context &Call, const InstallRequest &Options) {
  cancelled(Call);
  if (Options.Version.empty() || Options.Version == "." ||
      Options.Version == ".." ||
      !std::ranges::all_of(Options.Version, [](unsigned char C) {
        return std::isalnum(C) || C == '.' || C == '-';
      }))
    throw Failure("Invalid release identity");
  if (Options.Bundle.Packages.empty())
    throw Failure("Empty package catalog");
  for (const auto &Package : Options.Bundle.Packages) {
    if (Package.Sha256.size() != 64 ||
        !std::ranges::all_of(Package.Sha256,
                             [](unsigned char C) { return std::isxdigit(C); }))
      throw Failure("Invalid package digest");
    if (!Package.Destination.empty() && !safeRelative(Package.Destination))
      throw Failure("Invalid component destination");
  }
  if (!Options.Executable.is_absolute() ||
      !fs::is_regular_file(nativePath(Options.Executable)))
    throw Failure("Installer executable must be an existing absolute path");
  const auto &Layout = Options.Layout;
  validateCommandRegistration(Layout);
  rejectSymlinkAncestors(Layout.DataRoot);
  rejectSymlinkAncestors(Layout.ProgramRoot);
  secureDataDirectory(Layout.DataRoot, Layout.System);
  auto Lock = lockInstallation(Call, Layout);
  for (const auto &Directory :
       {Layout.ProgramRoot, Layout.ProgramRoot / "versions",
        Layout.CommandPath.parent_path(), Layout.cache()}) {
    rejectSymlinkAncestors(Directory);
    secureDataDirectory(Directory, Layout.System);
  }
  checkCommandOwner(Layout);
  auto Prefix = Layout.ProgramRoot / "versions" / pathFromText(Options.Version);
  checkReleaseIdentity(Prefix, Options);
  if (fs::exists(nativePath(Prefix)) &&
      !fs::is_regular_file(nativePath(installationFile(Prefix))))
    throw Failure("Existing version directory has no .installation file. "
                  "refusing to replace foreign files");
  bool Reused =
      completeInstallation(Call, Prefix, Options.Version, Options.Bundle);
  if (Reused &&
      sha256File(Prefix / executableName()) != sha256File(Options.Executable))
    throw Failure("This release identity already belongs to a different "
                  "executable. Use a new release version");
  if (!Reused) {
    auto Stage = Layout.ProgramRoot / (".staging-" + uniqueId());
    if (!fs::create_directory(nativePath(Stage)))
      throw Failure("Cannot create installer staging directory");
    Cleanup StageCleanup{Stage};
    auto Prepared = Stage / "version";
    fs::create_directory(nativePath(Prepared));
    copyReleaseLicenses(Options.Executable, Prepared);
    fetchBundle(Call, Layout, Options.Bundle, Prepared / "toolchain");
    auto Native = Prepared / "toolchain/targets" /
                  pathFromText(targetName(nativeTarget()));
    prepareTarget(Call, Layout, Prepared / "toolchain", Native, nativeTarget());
    prepareToolchain(Call, Prepared / "toolchain");
    fs::copy_file(nativePath(Options.Executable),
                  nativePath(Prepared / executableName()));
#ifdef _WIN32
    fs::permissions(nativePath(Prepared / executableName()),
                    fs::perms::owner_write, fs::perm_options::add);
#endif
    writeSdkSources(Prepared / "toolchain/sdk-sources.txt", Options.Bundle);
    readableInstallation(Prepared);
#ifndef _WIN32
    fs::permissions(nativePath(Prepared / executableName()),
                    static_cast<fs::perms>(0755));
#endif
    writeInstallation(Call, Prepared, Options);
    validateToolchain(Call, Prepared / "toolchain");
    if (!completeInstallation(Call, Prepared, Options.Version, Options.Bundle))
      throw Failure("Staged installation failed completeness check");
    cancelled(Call);
    rejectSymlinkAncestors(Prefix);
    fs::rename(nativePath(Prepared), nativePath(Prefix));
  }
  cancelled(Call);
  checkCommandOwner(Layout);
  auto Previous = registeredPaths(Call, Layout);
  auto Entries = registerCommand(Layout, Previous);
  saveRegisteredPaths(Prefix, Entries);
  activateCommand(Prefix / executableName(), Layout.CommandPath);
  return {Prefix, Reused};
}
UninstallResult uninstallRelease(Context &Call, const InstallRequest &Options) {
  cancelled(Call);
  if (Options.Version.empty() || Options.Version == "." ||
      Options.Version == ".." ||
      !std::ranges::all_of(Options.Version, [](unsigned char C) {
        return std::isalnum(C) || C == '.' || C == '-';
      }))
    throw Failure("Invalid release identity");
  const auto &Layout = Options.Layout;
  auto Prefix = Layout.ProgramRoot / "versions" / pathFromText(Options.Version);
  rejectSymlinkAncestors(Layout.DataRoot);
  rejectSymlinkAncestors(Prefix);
  if (!fs::exists(nativePath(Prefix)))
    return {Prefix, false, false};
  if (!fs::exists(nativePath(installationFile(Prefix))) &&
      !fs::exists(nativePath(Prefix / executableName())))
    return {Prefix, false, false};
  if (!fs::is_directory(nativePath(Layout.DataRoot)))
    throw Failure("Installation metadata directory is missing. Refusing an "
                  "unverified uninstall");
  auto Lock = lockInstallation(Call, Layout);
  rejectSymlinkAncestors(installationFile(Prefix));
  std::ifstream Receipt(nativePath(installationFile(Prefix)), std::ios::binary);
  std::string Version, Catalog, Hash;
  if (!readInstallIdentity(Receipt, Version, Catalog, Hash) ||
      Version != Options.Version ||
      Catalog != bundleFingerprint(Options.Bundle))
    throw Failure(
        "No matching .installation file. Refusing to remove foreign files");
  std::vector<PathEntry> Entries;
  auto Files = readInventory(Call, Receipt, Prefix, false, &Entries);
  if (std::ranges::find(Files, Prefix / executableName()) == Files.end())
    throw Failure("Incomplete uninstall inventory");
  Receipt.close();
  auto Targets = Prefix / "toolchain/targets";
  rejectSymlinkAncestors(Targets);
  if (fs::is_directory(nativePath(Targets)))
    for (const auto &Entry : fs::directory_iterator(nativePath(Targets))) {
      auto Status = Entry.symlink_status();
      if (!fs::is_directory(Status))
        continue;
      auto Root = Targets / Entry.path().filename();
      auto Target = parseTarget(pathText(Root.filename()));
      if (!Target || !fs::exists(nativePath(Root / ".installation")))
        continue;
      auto TargetFiles = targetInventory(Call, Root, *Target, false);
      Files.insert(Files.end(), TargetFiles.begin(), TargetFiles.end());
      Files.push_back(Root / ".installation");
    }
  bool Active = false;
  auto CommandStatus = fs::symlink_status(nativePath(Layout.CommandPath));
#ifdef _WIN32
  if (fs::is_regular_file(CommandStatus)) {
    rejectSymlinkAncestors(Layout.CommandPath);
    Active = sha256File(Layout.CommandPath) == Hash;
    for (const auto &Other :
         fs::directory_iterator(nativePath(Layout.ProgramRoot / "versions"))) {
      auto OtherRoot =
          Layout.ProgramRoot / "versions" / Other.path().filename();
      if (OtherRoot == Prefix)
        continue;
      auto OtherExe = OtherRoot / executableName();
      rejectSymlinkAncestors(OtherExe);
      if (fs::is_regular_file(nativePath(OtherExe)) &&
          sha256File(OtherExe) == Hash &&
          Options.Executable.lexically_normal() == OtherExe.lexically_normal())
        Active = false;
    }
  }
#else
  if (fs::is_symlink(CommandStatus)) {
    auto Target = fs::read_symlink(nativePath(Layout.CommandPath));
    if (!Target.is_absolute())
      Target = Layout.CommandPath.parent_path() / Target;
    Active = Target.lexically_normal() ==
             (Prefix / executableName()).lexically_normal();
  }
#endif
  bool OtherRelease = false;
  for (const auto &Other :
       fs::directory_iterator(nativePath(Layout.ProgramRoot / "versions")))
    if (Layout.ProgramRoot / "versions" / Other.path().filename() != Prefix &&
        fs::is_directory(Other.symlink_status()) &&
        fs::is_regular_file(nativePath(installationFile(
            Layout.ProgramRoot / "versions" / Other.path().filename()))))
      OtherRelease = true;
  cancelled(Call);
  if ((Active || !fs::exists(CommandStatus)) && !OtherRelease)
    unregisterCommand(Layout, Entries);
  if (Active) {
    rejectSymlinkAncestors(Layout.CommandPath.parent_path());
    removeInstalledFile(Layout.CommandPath);
  }
  for (const auto &File : Files)
    if (fs::exists(fs::symlink_status(nativePath(File))))
      removeInstalledFile(File);
  removeInstalledFile(installationFile(Prefix));
  removeEmptyDirectories(Prefix);
  if (!fs::exists(fs::symlink_status(nativePath(Layout.CommandPath))) &&
      !OtherRelease) {
    if (fs::is_empty(nativePath(Layout.ProgramRoot / "versions")))
      fs::remove(nativePath(Layout.ProgramRoot / "versions"));
    if (Layout.CommandPath.parent_path().lexically_relative(
            Layout.ProgramRoot) == "bin" &&
        fs::is_empty(nativePath(Layout.CommandPath.parent_path())))
      fs::remove(nativePath(Layout.CommandPath.parent_path()));
    if (fs::is_empty(nativePath(Layout.ProgramRoot)))
      fs::remove(nativePath(Layout.ProgramRoot));
  }
  finishUninstall(Layout, Prefix);
  return {Prefix, true, Active};
}
}

namespace llm::runtime {
namespace {
ToolchainBundle installedBundle(const Path &Prefix) {
  std::ifstream Input(nativePath(installationFile(Prefix)), std::ios::binary);
  std::string Version, Catalog, Hash;
  if (!readInstallIdentity(Input, Version, Catalog, Hash))
    throw Failure("No matching base installation. Run llm --install first.");
  auto Bundle = hostBundle(nativeTarget());
  if (Catalog == bundleFingerprint(Bundle))
    return Bundle;
  throw Failure("Installed toolchain catalog does not match this release");
}
void requireBase(Context &Call, const InstallRequest &Options) {
  auto Prefix =
      Options.Layout.ProgramRoot / "versions" / pathFromText(Options.Version);
  if (!completeInstallation(Call, Prefix, Options.Version, Options.Bundle))
    throw Failure(
        "The native installation is incomplete. Run llm --install first.");
}
}
InstallResult installTarget(Context &Call, const InstallRequest &Options,
                            Target BuildTarget) {
  if (!parseVersion(Options.Version))
    throw Failure("Invalid release identity");
  const auto &Layout = Options.Layout;
  rejectSymlinkAncestors(Layout.DataRoot);
  rejectSymlinkAncestors(Layout.ProgramRoot);
  if (!fs::is_directory(nativePath(Layout.DataRoot)))
    throw Failure(
        "The native installation is missing. Run llm --install first.");
  auto Lock = lockInstallation(Call, Layout);
  requireBase(Call, Options);
  rejectSymlinkAncestors(Layout.cache());
  secureDataDirectory(Layout.cache(), Layout.System);
  auto Tools = Layout.ProgramRoot / "versions" / pathFromText(Options.Version) /
               "toolchain";
  auto Parent = Tools / "targets";
  auto Root = Parent / pathFromText(targetName(BuildTarget));
  rejectSymlinkAncestors(Root);
  if (completeTargetInstallation(Call, Root, BuildTarget))
    return {Root, true};
  fs::create_directories(nativePath(Parent));
  auto Stage = Parent / (".staging-" + uniqueId());
  Cleanup StageCleanup{Stage};
  prepareTarget(Call, Layout, Tools, Stage, BuildTarget);
  if (!completeTargetInstallation(Call, Stage, BuildTarget))
    throw Failure("Staged target installation failed completeness check");
  cancelled(Call);
  fs::rename(nativePath(Stage), nativePath(Root));
  return {Root, false};
}
UninstallResult uninstallTarget(Context &Call, const InstallRequest &Options,
                                Target BuildTarget) {
  if (BuildTarget == nativeTarget())
    throw Failure("The native target is required. Use llm --uninstall to "
                  "remove the release");
  if (!parseVersion(Options.Version))
    throw Failure("Invalid release identity");
  const auto &Layout = Options.Layout;
  rejectSymlinkAncestors(Layout.DataRoot);
  if (!fs::is_directory(nativePath(Layout.DataRoot)))
    return {{}, false, false};
  auto Lock = lockInstallation(Call, Layout);
  auto Root = Layout.ProgramRoot / "versions" / pathFromText(Options.Version) /
              "toolchain/targets" / pathFromText(targetName(BuildTarget));
  rejectSymlinkAncestors(Root);
  if (!fs::exists(nativePath(Root)))
    return {Root, false, false};
  auto Files = targetInventory(Call, Root, BuildTarget, false);
  cancelled(Call);
  for (const auto &File : Files)
    if (fs::exists(fs::symlink_status(nativePath(File))))
      removeInstalledFile(File);
  removeInstalledFile(Root / ".installation");
  removeEmptyDirectories(Root);
  return {Root, true, false};
}
InstallLayout installationLayout(const Context &Call) {
  auto Layout = systemLayout();
  auto Executable = fs::canonical(nativePath(Call.Executable));
  auto Parent = Executable.parent_path();
  Path Root;
  if (Parent.filename() == pathFromText(Call.Version) &&
      Parent.parent_path().filename() == "versions")
    Root = Parent.parent_path().parent_path();
#ifdef _WIN32
  else if (Parent.filename() == "bin")
    Root = Parent.parent_path();
#endif
  if (Root.empty())
    return userLayout();
  return layoutAtPrefix(Root);
}
int installationCommand(Context &Call, const Command &Request) {
  auto Action = Request.Action;
  if (Request.Global && Request.InstallPath)
    throw Failure("--global and --path cannot be used together");
  auto Layout = Request.Global ? systemLayout() : installationLayout(Call);
  if (Request.InstallPath) {
    auto Root = fs::weakly_canonical(
        nativePath(Request.InstallPath->is_absolute()
                       ? *Request.InstallPath
                       : Call.InvocationDirectory / *Request.InstallPath));
    if (Root == Root.root_path())
      throw Failure("The filesystem root cannot be an installation directory");
    Layout = layoutAtPrefix(Root);
  }
  auto Prefix = Layout.ProgramRoot / "versions" / pathFromText(Call.Version);
  auto Bundle =
      (Action == Operation::Install && !Request.TargetExplicit) ||
              (Action == Operation::Uninstall && !Request.TargetExplicit &&
               !fs::exists(nativePath(Prefix)))
          ? hostBundle(nativeTarget())
          : installedBundle(Prefix);
  if (Action == Operation::CheckInstallation) {
    bool Complete = completeInstallation(Call, Prefix, Call.Version, Bundle);
    auto Root = Prefix / "toolchain/targets" /
                pathFromText(targetName(Request.BuildTarget));
    if (Request.TargetExplicit && Request.BuildTarget != nativeTarget())
      Complete = Complete &&
                 completeTargetInstallation(Call, Root, Request.BuildTarget);
    if (!Complete) {
      emit(
          Call, EventKind::Error,
          "Installation is incomplete. Run llm --install" +
              (Request.TargetExplicit
                   ? " --target " + std::string(targetName(Request.BuildTarget))
                   : "") +
              ".");
      return 1;
    }
    emit(Call, EventKind::Progress,
         "Installation complete: " +
             pathText(Request.TargetExplicit
                          ? targetToolchain(Prefix / "toolchain",
                                            Request.BuildTarget)
                          : Prefix));
    if (Call.Output.Verbose) {
      emit(Call, EventKind::Note, "Host tools: " + Bundle.Id);
      for (const auto &Package : Bundle.Packages)
        emit(Call, EventKind::Note, Package.Id + " SHA-256 " + Package.Sha256);
      if (Request.TargetExplicit)
        for (const auto &Package : targetBundle(Request.BuildTarget).Packages)
          emit(Call, EventKind::Note,
               Package.Id + " SHA-256 " + Package.Sha256);
    }
    return 0;
  }
  if (Action == Operation::Uninstall && Request.TargetExplicit &&
      Request.BuildTarget == nativeTarget())
    throw Failure("The native target is required. Use llm --uninstall to "
                  "remove the release");
  if (Layout.System && !elevated())
    return elevateOperation(
        Call, Action == Operation::Uninstall ? "--uninstall" : "--install",
        Request.TargetExplicit ? std::optional(Request.BuildTarget)
                               : std::nullopt);
  InstallRequest Options{Layout, Call.Executable, Call.Version,
                         std::move(Bundle)};
  if (Action == Operation::Uninstall) {
    auto Result = Request.TargetExplicit
                      ? uninstallTarget(Call, Options, Request.BuildTarget)
                      : uninstallRelease(Call, Options);
    emit(Call, EventKind::Progress,
         (Request.TargetExplicit ? std::string(targetName(Request.BuildTarget))
                                 : "llm " + Call.Version) +
             (Result.Removed ? " removed: " : " is not installed: ") +
             pathText(Result.VersionRoot));
  } else {
    auto Result = Request.TargetExplicit
                      ? installTarget(Call, Options, Request.BuildTarget)
                      : installRelease(Call, Options);
    emit(Call, EventKind::Progress,
         (Request.TargetExplicit ? std::string(targetName(Request.BuildTarget))
                                 : "llm " + Call.Version) +
             (Result.Reused ? " already installed: " : " installed: ") +
             pathText(Result.VersionRoot));
    if (!Request.TargetExplicit) {
      emit(Call, EventKind::Note, "Open a new terminal to use llm.");
    }
  }
  return 0;
}
}
