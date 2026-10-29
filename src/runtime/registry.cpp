module;
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
namespace {
void validToken(std::string_view Token);
RegistrySource registrySource(std::string_view Url) {
  return take(parseRegistrySource(Url));
}
Url registryAddress(std::string_view Text) {
  return take(parseRegistryUrl(Text));
}
std::string registryIdentity(std::string_view Url, std::string_view Name) {
  return packageIdentity(registrySource(Url), Name);
}
std::vector<PackageVersion> packageVersions(std::string_view Text) {
  return take(parsePackageVersions(Text));
}
class Credentials {
  Context &Call;
  Path File;
  std::map<std::string, std::string> load() const;
  void save(const std::map<std::string, std::string> &Values);

public:
  Credentials(Context &Call, Path File = {});
  std::optional<std::string> get(std::string_view Registry) const;
  std::optional<std::string> forArchive(std::string_view Url) const;
  void set(std::string_view Registry, std::string Token);
  bool erase(std::string_view Registry);
};
class Client {
  struct RegistrySnapshot {
    std::string Revision;
    std::map<std::string, std::string> Files;
    bool LocalLoaded = false;
  };
  Context &Call;
  Credentials &Store;
  Path Temporary;
  std::map<std::string, RegistrySnapshot> Snapshots;
  std::map<std::string, std::vector<PackageVersion>> Indexes;
  HttpResponse fetch(const std::string &Url, HttpRequest Request,
                     const Path &Destination);

public:
  Client(Context &Call, Credentials &Credentials, const Path &Work);
  ~Client();
  const std::vector<PackageVersion> &versions(const std::string &Registry,
                                              const std::string &Package);
  void download(const std::string &Registry, const std::string &Url,
                const Path &Destination,
                std::uint64_t Limit = 512ULL * 1024 * 1024);
  void validate(const std::string &Registry, const std::string &Token);
};
class Cache {
  Context &Call;
  Path Root;

public:
  Cache(Context &Call, Path Root = {});
  using Download = std::function<void(const Path &)>;
  Path sources(const std::string &Identity, const std::string &Name,
               const PackageVersion &Version, const Download &Download);
  Path instance(const Path &Source, const std::string &Identity,
                const PackageVersion &Version, const Path &Owner,
                std::string_view Triple, bool Release);
  const Path &root() const { return Root; }
  static Path peek(Context &Call, const std::string &Identity,
                   const PackageVersion &Version, Path Root = {});
};
}
}

namespace llm::runtime {
namespace {
std::pair<std::string, std::string> splitIdentity(const std::string &Identity) {
  auto Separator = Identity.rfind('|');
  if (Separator == std::string::npos)
    throw Failure("Invalid package identity");
  auto Source = registrySource(Identity.substr(0, Separator));
  auto Name = Identity.substr(Separator + 1);
  if (packageIdentity(Source, Name) != Identity)
    throw Failure("Noncanonical package identity");
  return {Source.Url, Name};
}

struct CollectedRequirements {
  std::vector<PackageRequirement> Requirements;
};

CollectedRequirements collectRequirements(Context &Call, const Command &Request,
                                          const Path &Owner,
                                          const std::vector<Manifest> &Projects,
                                          const std::string &Package = {},
                                          const Path &Boundary = {}) {
  CollectedRequirements Result;
  std::set<Path> Visited, Visiting;
  auto Visit = [&](auto &&Self, const Manifest &Project,
                   const std::string &Chain) -> void {
    cancelled(Call);
    auto Canonical = fs::canonical(Project.Root);
    if (Visiting.contains(Canonical))
      throw Failure("Dependency cycle during package resolution: " + Chain);
    if (!Visited.insert(Canonical).second)
      return;
    if (Visited.size() > 4096)
      throw Failure("Package graph exceeds 4096 projects");
    Visiting.insert(Canonical);
    auto Settings = take(effectiveSettings(Project, Request.BuildTarget));
    auto References = Settings.Dependencies.Private;
    References.insert(References.end(), Settings.Dependencies.Public.begin(),
                      Settings.Dependencies.Public.end());
    for (const auto &Text : References) {
      auto Reference = take(dependencyReference(Project, Text, false));
      auto Next = Chain + " -> " + Text;
      if (Reference.Local) {
        auto Directory = fs::canonical(*Reference.Local);
        if (!Boundary.empty()) {
          auto Relative = Directory.lexically_relative(Boundary);
          if (Relative != "." && !isRelativeSafe(Relative))
            throw Failure("Registry local dependency escapes its archive: " +
                          Next);
        }
        auto Child = readManifest(Directory);
        if (!isLibrary(Child.Type))
          throw Failure("Dependency must be a library: " + Next);
        Self(Self, Child, Next);
      } else
        Result.Requirements.push_back(
            {registryIdentity(Reference.Registry, Reference.Package),
             Reference.Requirement, Next});
    }
    Visiting.erase(Canonical);
  };
  for (const auto &Project : Projects)
    Visit(Visit, Project, Project.Name);
  return Result;
}

struct RegistryResolution {
  std::map<std::string, Path> Projects;
  std::optional<std::string> Lock;
};

RegistryResolution resolveRegistry(Context &Call, const Command &Request,
                                   const Path &Owner,
                                   const std::vector<Manifest> &Projects,
                                   const std::set<std::string> &Unlocked = {}) {
  auto LockFile = Owner / ".lock";
  rejectSymlinkAncestors(LockFile);
  LockData Previous;
  if (fs::exists(LockFile))
    Previous = parseLock(readText(LockFile));
  auto RootRequirements = collectRequirements(Call, Request, Owner, Projects);
  PackageCatalog Catalog;
  Catalog.Unlocked = Unlocked;
  if (Request.Action != Operation::Update)
    for (const auto &[Identity, Version] : Previous.Packages)
      Catalog.LockedVersions.emplace(Identity, Version.Selected.Text);
  std::unique_ptr<Credentials> CredentialsStore;
  std::unique_ptr<Cache> SourceCache;
  std::unique_ptr<Client> Network;
  auto Ensure = [&] {
    if (SourceCache)
      return;
    CredentialsStore = std::make_unique<Credentials>(Call);
    SourceCache = std::make_unique<Cache>(Call);
    Network =
        std::make_unique<Client>(Call, *CredentialsStore, SourceCache->root());
  };
  std::map<std::string, Path> Sources;
  auto VersionKey = [](const std::string &Identity,
                       const PackageVersion &Version) {
    return Identity + '@' + Version.Selected.Text + ':' + Version.Sha256;
  };
  auto GetSources = [&](const std::string &Identity,
                        const PackageVersion &Version) {
    auto Key = VersionKey(Identity, Version);
    if (auto Found = Sources.find(Key); Found != Sources.end())
      return Found->second;
    Ensure();
    auto [Registry, Name] = splitIdentity(Identity);
    auto Source = SourceCache->sources(
        Identity, Name, Version, [&](const Path &Destination) {
          auto Url = Version.Url;
          if (Url.empty()) {
            const auto &Index = Network->versions(Registry, Name);
            auto Found =
                std::ranges::find_if(Index, [&](const auto &Candidate) {
                  return Candidate.Selected.Text == Version.Selected.Text;
                });
            if (Found == Index.end() || Found->Sha256 != Version.Sha256)
              throw Failure(
                  "Registry no longer supplies the locked archive hash for " +
                  Name);
            Url = Found->Url;
          }
          Network->download(Registry, Url, Destination);
        });
    Sources.emplace(Key, Source);
    return Source;
  };
  auto CheckSource = [&](const std::string &Identity) {
    if (Request.Action == Operation::Update ||
        Previous.Packages.contains(Identity) || Unlocked.contains(Identity))
      return;
    auto [Registry, Name] = splitIdentity(Identity);
    for (const auto &[OldIdentity, Version] : Previous.Packages)
      if (auto Old = splitIdentity(OldIdentity);
          Old.second == Name && Old.first != Registry)
        throw Failure("Registry source changed for '" + Name +
                      "'. Run llm update explicitly");
  };
  PackageResolution Selection;
  for (std::size_t Step = 0;; ++Step) {
    cancelled(Call);
    if (Step > 100000)
      throw Failure("Package metadata expansion exceeds its limit");
    auto Decision =
        take(resolvePackageStep(RootRequirements.Requirements, Catalog));
    if (auto Done = std::get_if<PackageResolution>(&Decision)) {
      Selection = std::move(*Done);
      break;
    }
    const auto &Needed = std::get<PackageMetadataRequest>(Decision);
    CheckSource(Needed.Identity);
    if (!Needed.Selected) {
      std::vector<PackageVersion> Versions;
      if (Previous.Packages.contains(Needed.Identity) &&
          Catalog.LockedVersions.contains(Needed.Identity) &&
          !Unlocked.contains(Needed.Identity))
        Versions.push_back(Previous.Packages.at(Needed.Identity));
      else {
        Ensure();
        auto [Registry, Name] = splitIdentity(Needed.Identity);
        Versions = Network->versions(Registry, Name);
      }
      auto &Candidates = Catalog.Packages[Needed.Identity];
      for (auto &Version : Versions)
        Candidates.push_back({std::move(Version), {}, false});
    } else {
      auto Directory = GetSources(Needed.Identity, *Needed.Selected);
      auto Project = readManifest(Directory);
      if (!isLibrary(Project.Type))
        throw Failure("Registry package must declare a library product");
      auto Requirements = collectRequirements(Call, Request, Owner, {Project},
                                              Needed.Identity, Directory);
      auto &Candidates = Catalog.Packages.at(Needed.Identity);
      auto Candidate = std::ranges::find_if(Candidates, [&](const auto &Value) {
        return Value.Package.Selected.Text == Needed.Selected->Selected.Text &&
               Value.Package.Sha256 == Needed.Selected->Sha256;
      });
      if (Candidate == Candidates.end())
        throw Failure("Invalid package metadata request");
      Candidate->Dependencies = Requirements.Requirements;
      Candidate->MetadataAvailable = true;
    }
  }
  LockData Result = Previous;
  std::map<std::string, Path> SelectedSources;
  for (const auto &[Identity, Candidate] : Selection.Packages) {
    Result.Packages.insert_or_assign(Identity, Candidate.Package);
    SelectedSources.emplace(Identity, GetSources(Identity, Candidate.Package));
  }
  for (const auto &Project : Projects)
    loadGraph(Call, Project, Request.BuildTarget, SelectedSources);
  std::map<std::string, std::set<std::string>> Edges;
  std::set<std::string> Seeds, Scanned;
  auto Declarations = [&](auto &&Self, const Manifest &Project,
                          const std::string &Package,
                          const Path &Boundary) -> void {
    auto Key = Package + '\n' + pathText(fs::canonical(Project.Root));
    if (!Scanned.insert(Key).second)
      return;
    auto Layer = [&](const llm::Layer &Layer) {
      auto References = Layer.Dependencies.Private;
      References.insert(References.end(), Layer.Dependencies.Public.begin(),
                        Layer.Dependencies.Public.end());
      for (const auto &Text : References) {
        auto Reference = take(dependencyReference(Project, Text, false));
        if (Reference.Local) {
          if (!fs::is_regular_file(*Reference.Local / ".llm")) {
            throw Failure("Declared local dependency is missing: " +
                          pathText(*Reference.Local));
          }
          auto Directory = fs::canonical(*Reference.Local);
          if (!Boundary.empty()) {
            auto Relative = Directory.lexically_relative(Boundary);
            if (Relative != "." && !isRelativeSafe(Relative))
              throw Failure("Registry local dependency escapes its archive");
          }
          Self(Self, readManifest(Directory), Package, Boundary);
        } else {
          auto Identity =
              registryIdentity(Reference.Registry, Reference.Package);
          if (Package.empty())
            Seeds.insert(Identity);
          else
            Edges[Package].insert(Identity);
        }
      }
    };
    Layer(Project.Base);
    for (const auto &[Platform, Settings] : Project.Platforms)
      Layer(Settings);
  };
  for (const auto &Project : Projects) {
    Declarations(Declarations, Project, {}, {});
    if (Project.Type != ProductType::Test && !Result.Packages.empty()) {
      for (const auto &Suite : discoverTests(Project))
        Declarations(Declarations, Suite.Project, {}, {});
    }
  }
  for (const auto &[Identity, Version] : Result.Packages) {
    auto Found = SelectedSources.find(Identity);
    auto Directory = Found == SelectedSources.end()
                         ? Cache::peek(Call, Identity, Version)
                         : Found->second;
    if (Directory.empty())
      Directory = GetSources(Identity, Version);
    Declarations(Declarations, readManifest(Directory), Identity, Directory);
  }
  {
    std::set<std::string> Reachable;
    auto Keep = [&](auto &&Self, const std::string &Identity) -> void {
      if (!Reachable.insert(Identity).second)
        return;
      if (auto Found = Edges.find(Identity); Found != Edges.end())
        for (const auto &Child : Found->second)
          Self(Self, Child);
    };
    for (const auto &Identity : Seeds)
      Keep(Keep, Identity);
    std::erase_if(Result.Packages, [&](const auto &Entry) {
      return !Reachable.contains(Entry.first);
    });
    std::erase_if(Edges, [&](const auto &Entry) {
      return !Reachable.contains(Entry.first);
    });
  }
  RegistryResolution Resolution;
  for (const auto &[Identity, Directory] : SelectedSources) {
    Ensure();
    Resolution.Projects.emplace(
        Identity, SourceCache->instance(Directory, Identity,
                                        Selection.Packages.at(Identity).Package,
                                        Owner, targetName(Request.BuildTarget),
                                        Request.Mode == Profile::Release));
  }
  if (!Result.Packages.empty() || fs::exists(LockFile))
    Resolution.Lock = lockText(Result);
  return Resolution;
}

void recoverTransaction(Context &Call, const Path &Owner, bool Held) {
  auto Journal = Owner / "obj/.llm-dependency-transaction";
  rejectSymlinkAncestors(Journal);
  if (!fs::exists(Journal))
    return;
  std::unique_ptr<FileLock> Lock;
  if (!Held) {
    Lock = std::make_unique<FileLock>(Call, Owner / "obj/.guard", true);
    if (!fs::exists(Journal))
      return;
  }
  auto Record = toml::parse(readText(Journal));
  auto File = Record["manifest"].value<std::string>(),
       Old = Record["old_manifest"].value<std::string>(),
       OldLock = Record["old_lock"].value<std::string>();
  auto HadLock = Record["had_lock"].value<bool>();
  if (!File || !Old || !OldLock || !HadLock)
    throw Failure(
        "Invalid dependency transaction journal. Preserve it for recovery");
  auto Relative = pathFromText(*File);
  if (!isRelativeSafe(Relative) || Relative.filename() != ".llm")
    throw Failure("Unsafe dependency transaction journal");
  rejectSymlinkAncestors(Owner / Relative);
  rejectSymlinkAncestors(Owner / ".lock");
  parseManifest(*Old, Owner / Relative);
  if (*HadLock)
    parseLock(*OldLock);
  atomicWrite(Owner / Relative, *Old);
  if (*HadLock)
    atomicWrite(Owner / ".lock", *OldLock);
  else
    fs::remove(Owner / ".lock");
  fs::remove(Journal);
}

std::string editDependency(std::string_view Text, const Path &File,
                           const std::string &Dependency, bool Remove,
                           bool Public);
}

std::map<std::string, Path>
resolveDependencies(Context &Call, const Command &Request, const Path &Owner,
                    const std::vector<Manifest> &Projects) {
  rejectSymlinkAncestors(Owner / "obj");
  fs::create_directories(Owner / "obj");
  FileLock Lock(Call, Owner / "obj/.guard", true);
  recoverTransaction(Call, Owner, true);
  auto Resolution = resolveRegistry(Call, Request, Owner, Projects);
  if (Resolution.Lock)
    atomicWrite(Owner / ".lock", *Resolution.Lock);
  return Resolution.Projects;
}

std::map<std::string, Path> cachedDependencies(Context &Call,
                                               const Path &Owner) {
  std::map<std::string, Path> Result;
  auto File = Owner / ".lock";
  rejectSymlinkAncestors(File);
  if (!fs::is_regular_file(File))
    return Result;
  auto Lock = parseLock(readText(File));
  for (const auto &[Identity, Version] : Lock.Packages) {
    auto Source = Cache::peek(Call, Identity, Version);
    if (!Source.empty())
      Result.emplace(Identity, Source);
  }
  return Result;
}

int registryCommand(Context &Call, const Command &Request,
                    const Manifest &Project) {
  if (Request.Action == Operation::Login ||
      Request.Action == Operation::Logout) {
    auto Found = Project.Registries.find(Request.Selection);
    if (Found == Project.Registries.end())
      throw Failure("Unknown registry '" + Request.Selection + "'");
    auto Url = registrySource(Found->second).Url;
    Credentials Store(Call);
    if (Request.Action == Operation::Logout) {
      Store.erase(Url);
      emit(Call, EventKind::Progress,
           "Credentials removed for " + Request.Selection);
      return 0;
    }
    if (!Url.starts_with("https://"))
      throw Failure("Token authentication requires an HTTPS registry");
    auto Prompt = "Token for " + Request.Selection + ": ";
    auto Token = Call.ReadSecret ? Call.ReadSecret(Prompt) : readSecret(Prompt);
    validToken(Token);
    Cache Cache(Call);
    Client Network(Call, Store, Cache.root());
    Network.validate(Url, Token);
    Store.set(Url, std::move(Token));
    emit(Call, EventKind::Progress,
         "Credentials saved for " + Request.Selection);
    return 0;
  }
  auto Owner = Project.Root;
  rejectSymlinkAncestors(Owner / "obj");
  fs::create_directories(Owner / "obj");
  FileLock Lock(Call, Owner / "obj/.guard", true);
  recoverTransaction(Call, Owner, true);
  std::vector<Manifest> Roots{Project};
  if (Owner != Project.Root)
    Roots.push_back(readManifest(Owner));
  if (Request.Action == Operation::Update) {
    if (Project.Type != ProductType::Test)
      for (const auto &Suite : discoverTests(Project))
        Roots.push_back(Suite.Project);
    auto Resolution = resolveRegistry(Call, Request, Owner, Roots);
    if (Resolution.Lock)
      atomicWrite(Owner / ".lock", *Resolution.Lock);
    emit(Call, EventKind::Progress, "Dependency lock updated.");
    return 0;
  }
  auto Original = readText(Project.Root / ".llm");
  auto Changed =
      editDependency(Original, Project.Root / ".llm", Request.Selection,
                     Request.Action == Operation::Remove, Request.Public);
  Roots.front() = parseManifest(Changed, Project.Root / ".llm");
  std::set<std::string> Unlocked;
  if (Request.Action == Operation::Add) {
    auto Reference = take(
        dependencyReference(Roots.front(), Request.Selection, Request.Public));
    if (!Reference.Local)
      Unlocked.insert(registryIdentity(Reference.Registry, Reference.Package));
  }
  auto Resolution = resolveRegistry(Call, Request, Owner, Roots, Unlocked);
  auto Relative = (Project.Root / ".llm").lexically_relative(Owner);
  if (!isRelativeSafe(Relative) || Relative.filename() != ".llm")
    throw Failure("Manifest transaction must stay inside the build root");
  auto Journal = Owner / "obj/.llm-dependency-transaction";
  rejectSymlinkAncestors(Journal);
  rejectSymlinkAncestors(Owner / ".lock");
  bool HadLock = fs::exists(Owner / ".lock");
  toml::table Record{
      {"manifest", pathText(Relative)},
      {"old_manifest", Original},
      {"had_lock", HadLock},
      {"old_lock", HadLock ? readText(Owner / ".lock") : std::string{}}};
  std::ostringstream Encoded;
  Encoded << Record;
  atomicWrite(Journal, Encoded.str());
  try {
    atomicWrite(Project.Root / ".llm", Changed);
    if (Resolution.Lock)
      atomicWrite(Owner / ".lock", *Resolution.Lock);
    fs::remove(Journal);
  } catch (...) {
    recoverTransaction(Call, Owner, true);
    throw;
  }
  emit(Call, EventKind::Progress,
       Request.Action == Operation::Remove ? "Dependency removed."
                                           : "Dependency saved.");
  return 0;
}
}

namespace llm::runtime {
namespace {
void validToken(std::string_view Token) {
  if (Token.empty() || Token.size() > 16384 ||
      !std::ranges::all_of(Token, [](unsigned char Byte) {
        return Byte >= 0x21 && Byte <= 0x7e;
      }))
    throw Failure(
        "Registry token must be a nonempty printable token without whitespace");
}
Credentials::Credentials(Context &Call, fs::path File)
    : Call(Call),
      File(File.empty() ? (Call.UserDirectory.empty() ? userHome()
                                                      : Call.UserDirectory) /
                              ".llm/credentials"
                        : fs::absolute(File)) {}
std::map<std::string, std::string> Credentials::load() const {
  rejectSymlinkAncestors(File);
  if (!fs::exists(File))
    return {};
  checkPrivateFile(File);
  if (fs::file_size(File) > 4 * 1024 * 1024)
    throw Failure("Credential store exceeds size limit");
  std::map<std::string, std::string> Result;
  try {
    auto Document = toml::parse_file(pathText(File));
    for (const auto &[Key, Value] : Document)
      if (Key != "registries")
        throw Failure("Invalid credential store");
    auto Values = Document["registries"].as_table();
    if (!Values)
      throw Failure("Invalid credential store");
    for (const auto &[Key, Value] : *Values) {
      auto Token = Value.value<std::string>();
      if (!Token)
        throw Failure("Invalid credential store");
      validToken(*Token);
      if (!Result.emplace(registrySource(Key.str()).Url, std::move(*Token))
               .second)
        throw Failure("Duplicate credential source");
    }
  } catch (...) {
    throw Failure("Invalid credential store. Its contents were not changed");
  }
  return Result;
}
void Credentials::save(const std::map<std::string, std::string> &Values) {
  auto Temporary = File.parent_path() / (".credential-write-" + uniqueId());
  struct Remove {
    fs::path File;
    ~Remove() {
      std::error_code Error;
      fs::remove(File, Error);
    }
  } Remove{Temporary};
  rejectSymlinkAncestors(Temporary);
  std::ofstream Output(Temporary,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!Output)
    throw Failure("Cannot create private credential staging file");
  protectPrivatePath(Temporary, false);
  Output << "[registries]\n";
  for (const auto &[Url, Token] : Values)
    Output << jsonString(Url) << " = " << jsonString(Token) << '\n';
  Output.close();
  if (!Output)
    throw Failure("Cannot finish credential store");
  if (fs::exists(File))
    checkPrivateFile(File);
  replaceFile(Temporary, File);
}
std::optional<std::string> Credentials::get(std::string_view Registry) const {
  auto Values = load();
  auto Found = Values.find(registrySource(Registry).Url);
  return Found == Values.end() ? std::optional<std::string>{} : Found->second;
}
std::optional<std::string> Credentials::forArchive(std::string_view Url) const {
  auto Parsed = registryAddress(Url);
  auto Canonical = Parsed.Origin + Parsed.Resource;
  auto Values = load();
  const std::string *Result = nullptr;
  std::size_t Specificity = 0;
  for (const auto &[Base, Token] : Values)
    if ((Canonical == Base || Canonical.starts_with(Base + "/") ||
         Canonical.starts_with(Base + "?")) &&
        Base.size() > Specificity) {
      Specificity = Base.size();
      Result = &Token;
    }
  return Result ? std::optional<std::string>(*Result)
                : std::optional<std::string>{};
}
void Credentials::set(std::string_view Registry, std::string Token) {
  auto Url = registrySource(Registry).Url;
  validToken(Token);
  rejectSymlinkAncestors(File.parent_path());
  fs::create_directories(File.parent_path());
  protectPrivatePath(File.parent_path(), true);
  auto Lockfile = File.parent_path() / ".credentials.lock";
  rejectSymlinkAncestors(Lockfile);
  FileLock Lock(Call, Lockfile, true);
  auto Values = load();
  Values[Url] = std::move(Token);
  save(Values);
}
bool Credentials::erase(std::string_view Registry) {
  auto Url = registrySource(Registry).Url;
  rejectSymlinkAncestors(File);
  if (!fs::exists(File))
    return false;
  auto Lockfile = File.parent_path() / ".credentials.lock";
  rejectSymlinkAncestors(Lockfile);
  FileLock Lock(Call, Lockfile, true);
  auto Values = load();
  if (!Values.erase(Url))
    return false;
  save(Values);
  return true;
}
}
}

namespace llm::runtime {
namespace {
namespace {
std::string encoded(std::string_view Value) {
  constexpr char Hex[] = "0123456789ABCDEF";
  std::string Result;
  for (unsigned char C : Value) {
    if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
        (C >= '0' && C <= '9') || C == '-' || C == '.' || C == '_' || C == '~')
      Result += static_cast<char>(C);
    else {
      Result += '%';
      Result += Hex[C >> 4];
      Result += Hex[C & 15];
    }
  }
  return Result;
}
std::string decoded(std::string_view Value) {
  auto Digit = [](char C) -> unsigned {
    if (C >= '0' && C <= '9')
      return C - '0';
    if (C >= 'a' && C <= 'f')
      return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
      return C - 'A' + 10;
    throw Failure("Invalid URL escape");
  };
  std::string Result;
  for (std::size_t I = 0; I < Value.size(); ++I) {
    if (Value[I] == '%') {
      if (I + 2 >= Value.size())
        throw Failure("Invalid URL escape");
      Result +=
          static_cast<char>((Digit(Value[I + 1]) << 4) | Digit(Value[I + 2]));
      I += 2;
    } else
      Result += Value[I];
  }
  return Result;
}
Path localFile(std::string_view Text) {
  auto Address = registryAddress(Text);
  if (Address.Scheme != "file")
    throw Failure("Expected a local file URL");
  auto Name = decoded(Address.Resource);
#ifdef _WIN32
  if (Name.size() < 4 || Name[0] != '/' || Name[2] != ':' || Name[3] != '/')
    throw Failure("File URLs on Windows require an absolute drive path");
  Name.erase(0, 1);
#endif
  auto File = pathFromText(Name);
  if (!File.is_absolute())
    throw Failure("File URLs require absolute paths");
  rejectSymlinkAncestors(File);
  return File;
}
HttpRequest githubRequest(const std::optional<std::string> &Token,
                          bool Raw = false) {
  HttpRequest Request;
  Request.Limit = 8ULL * 1024 * 1024;
  Request.Headers["Accept"] =
      Raw ? "application/vnd.github.raw+json" : "application/vnd.github+json";
  Request.Headers["X-GitHub-Api-Version"] = "2026-03-10";
  if (Token)
    Request.Headers["Authorization"] = "Bearer " + *Token;
  return Request;
}
[[noreturn]] void responseError(int Status, const std::string &RegistrySource) {
  if (Status == 401 || Status == 403 || Status == 404)
    throw Failure(
        "Registry endpoint returned HTTP " + std::to_string(Status) + " for " +
        RegistrySource +
        ". Check its address and use llm login REGISTRY for a private source");
  if (Status == 412)
    throw Failure(
        "Registry revision changed during resolution. Retry the operation");
  throw Failure("Registry endpoint returned HTTP " + std::to_string(Status) +
                " for " + RegistrySource);
}
std::vector<std::string> pathParts(std::string_view Path) {
  std::vector<std::string> Result;
  std::size_t Start = Path.starts_with('/') ? 1 : 0;
  for (;;) {
    auto End = Path.find('/', Start);
    Result.emplace_back(
        Path.substr(Start, End == std::string_view::npos ? End : End - Start));
    if (End == std::string_view::npos)
      break;
    Start = End + 1;
  }
  return Result;
}
}
Client::Client(Context &Call, Credentials &Credentials, const fs::path &Work)
    : Call(Call), Store(Credentials) {
  rejectSymlinkAncestors(Work);
  fs::create_directories(Work);
  Temporary = Work / (".requests-" + uniqueId());
  fs::create_directory(Temporary);
  protectPrivatePath(Temporary, true);
}
Client::~Client() {
  std::error_code Error;
  fs::remove_all(Temporary, Error);
}
HttpResponse Client::fetch(const std::string &Url, HttpRequest Request,
                           const fs::path &Destination) {
  return httpGet(Call, Url, Destination, Request);
}
const std::vector<PackageVersion> &
Client::versions(const std::string &Registry, const std::string &Package) {
  auto Descriptor = registrySource(Registry);
  auto Key = registryIdentity(Descriptor.Url, Package);
  if (auto Found = Indexes.find(Key); Found != Indexes.end())
    return Found->second;
  auto Snapshot = Snapshots.try_emplace(Descriptor.Url).first;
  auto &State = Snapshot->second;
  if (Descriptor.Url.starts_with("file://")) {
    if (!State.LocalLoaded) {
      auto Directory = localFile(Descriptor.Url) / "packages";
      rejectSymlinkAncestors(Directory);
      std::uintmax_t Bytes = 0;
      for (const auto &Entry : fs::directory_iterator(Directory)) {
        cancelled(Call);
        if (!fs::is_regular_file(Entry.symlink_status()) ||
            fs::file_size(Entry.path()) > 8ULL * 1024 * 1024 ||
            State.Files.size() >= 20000)
          throw Failure("Invalid local registry index entry");
        Bytes += fs::file_size(Entry.path());
        if (Bytes > 64ULL * 1024 * 1024)
          throw Failure("Local registry index exceeds size limit");
        State.Files.emplace(pathText(Entry.path().filename()),
                            readText(Entry.path()));
      }
      std::size_t Count = 0;
      for (const auto &Entry : fs::directory_iterator(Directory)) {
        rejectSymlinkAncestors(Entry.path());
        auto Found = State.Files.find(pathText(Entry.path().filename()));
        if (Found == State.Files.end() ||
            readText(Entry.path()) != Found->second)
          throw Failure("Local registry changed during resolution. Retry");
        ++Count;
      }
      if (Count != State.Files.size())
        throw Failure("Local registry changed during resolution. Retry");
      State.LocalLoaded = true;
    }
    auto Found = State.Files.find(Package);
    if (Found == State.Files.end())
      throw Failure("Package not found in local registry: " + Package);
    return Indexes.emplace(Key, packageVersions(Found->second)).first->second;
  }
  auto Token = Descriptor.Url.starts_with("https://")
                   ? Store.get(Descriptor.Url)
                   : std::optional<std::string>{};
  auto Body = Temporary / uniqueId();
  if (Descriptor.Github) {
    auto Api = "https://api.github.com/repos/" + Descriptor.Owner + "/" +
               Descriptor.Repository;
    if (State.Revision.empty()) {
      auto Response = fetch(Api, githubRequest(Token), Body);
      if (Response.Status != 200)
        responseError(Response.Status, Descriptor.Url);
      auto Metadata = nlohmann::json::parse(readText(Body));
      auto Branch = Metadata.at("default_branch").get<std::string>();
      Response = fetch(Api + "/commits/" + encoded(Branch),
                       githubRequest(Token), Body);
      if (Response.Status != 200)
        responseError(Response.Status, Descriptor.Url);
      auto Commit = nlohmann::json::parse(readText(Body));
      State.Revision = Commit.at("sha").get<std::string>();
      if ((State.Revision.size() != 40 && State.Revision.size() != 64) ||
          !std::ranges::all_of(State.Revision, [](char C) {
            return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
          }))
        throw Failure("Invalid GitHub registry revision");
    }
    auto Response = fetch(Api + "/contents/packages/" + encoded(Package) +
                              "?ref=" + State.Revision,
                          githubRequest(Token, true), Body);
    if (Response.Status != 200)
      responseError(Response.Status, Descriptor.Url);
  } else {
    HttpRequest Request;
    Request.Limit = 8ULL * 1024 * 1024;
    if (Token)
      Request.Headers["Authorization"] = "Bearer " + *Token;
    if (!State.Revision.empty())
      Request.Headers["If-Match"] = State.Revision;
    auto Response = fetch(Descriptor.Url + "/packages/" + encoded(Package),
                          std::move(Request), Body);
    if (Response.Status != 200)
      responseError(Response.Status, Descriptor.Url);
    const auto &Revision = Response.Headers["etag"];
    if (Revision.size() < 2 || Revision.front() != '"' ||
        Revision.back() != '"' || Revision.size() > 512 ||
        !std::ranges::all_of(
            std::string_view(Revision).substr(1, Revision.size() - 2),
            [](unsigned char Byte) {
              return Byte == 0x21 || (Byte >= 0x23 && Byte != 0x7f);
            }))
      throw Failure(
          "HTTP registries must return one strong global revision ETag");
    if (State.Revision.empty())
      State.Revision = Response.Headers["etag"];
    else if (State.Revision != Response.Headers["etag"])
      throw Failure(
          "HTTPS registry returned an inconsistent snapshot revision");
  }
  return Indexes.emplace(Key, packageVersions(readText(Body))).first->second;
}
void Client::download(const std::string &Registry, const std::string &Url,
                      const fs::path &Destination, std::uint64_t Limit) {
  auto Descriptor = registrySource(Registry);
  auto Parsed = registryAddress(Url);
  if (Parsed.Scheme == "file") {
    if (!Descriptor.Url.starts_with("file://"))
      throw Failure("A remote registry cannot reference local file archives");
    auto Source = localFile(Url);
    if (!fs::is_regular_file(Source) || fs::file_size(Source) > Limit)
      throw Failure("Invalid local package archive");
    rejectSymlinkAncestors(Destination);
    fs::copy_file(Source, Destination, fs::copy_options::overwrite_existing);
    cancelled(Call);
    return;
  }
  HttpRequest Request;
  Request.Limit = Limit;
  std::string Endpoint = Url;
  if (Parsed.Scheme == "https" && Parsed.Host == "github.com" &&
      Parsed.Port == 443) {
    auto Parts =
        pathParts(Parsed.Resource.substr(0, Parsed.Resource.find('?')));
    if (Parts.size() == 6 && Parts[2] == "releases" && Parts[3] == "download") {
      auto Owner = Parts[0], Repository = Parts[1];
      auto ArchiveSource =
          registrySource("https://github.com/" + Owner + "/" + Repository);
      auto Token = Descriptor.Github ? Store.get(Descriptor.Url)
                                     : std::optional<std::string>{};
      auto Api = "https://api.github.com/repos/" + ArchiveSource.Owner + "/" +
                 ArchiveSource.Repository;
      auto Body = Temporary / uniqueId();
      auto Response =
          fetch(Api + "/releases/tags/" + encoded(decoded(Parts[4])),
                githubRequest(Token), Body);
      if (Response.Status != 200)
        responseError(Response.Status, ArchiveSource.Url);
      auto Release = nlohmann::json::parse(readText(Body));
      std::string AssetId;
      for (const auto &Asset : Release.at("assets"))
        if (Asset.at("name").get<std::string>() == decoded(Parts[5])) {
          const auto &Number = Asset.at("id");
          if (!Number.is_number_unsigned() || !AssetId.empty())
            throw Failure("Invalid or ambiguous GitHub release asset");
          AssetId = std::to_string(Number.get<std::uint64_t>());
        }
      if (AssetId.empty())
        throw Failure("Declared GitHub release asset was not found");
      Endpoint = Api + "/releases/assets/" + AssetId;
      Request = githubRequest(Token);
      Request.Limit = Limit;
      Request.Headers["Accept"] = "application/octet-stream";
    }
  } else if (Parsed.Scheme == "https" && Parsed.Host == "api.github.com" &&
             Parsed.Port == 443) {
    auto Parts =
        pathParts(Parsed.Resource.substr(0, Parsed.Resource.find('?')));
    if (Parts.size() == 6 && Parts[0] == "repos" && Parts[3] == "releases" &&
        Parts[4] == "assets" && !Parts[5].empty() &&
        std::ranges::all_of(Parts[5],
                            [](char C) { return C >= '0' && C <= '9'; })) {
      registrySource("https://github.com/" + Parts[1] + "/" + Parts[2]);
      auto Token =
          Descriptor.Github ? Store.get(Descriptor.Url) : Store.forArchive(Url);
      Request = githubRequest(Token);
      Request.Limit = Limit;
      Request.Headers["Accept"] = "application/octet-stream";
    }
  } else {
    if (Parsed.Scheme == "https")
      if (auto Token = Store.forArchive(Url))
        Request.Headers["Authorization"] = "Bearer " + *Token;
  }
  auto Response = fetch(Endpoint, std::move(Request), Destination);
  if (Response.Status != 200)
    responseError(Response.Status, Parsed.Origin);
}
void Client::validate(const std::string &Registry, const std::string &Token) {
  auto Descriptor = registrySource(Registry);
  if (!Descriptor.Url.starts_with("https://"))
    throw Failure("Token authentication requires an HTTPS registry");
  auto Body = Temporary / uniqueId();
  HttpRequest Request;
  std::string Endpoint = Descriptor.Url;
  if (Descriptor.Github) {
    Endpoint = "https://api.github.com/repos/" + Descriptor.Owner + "/" +
               Descriptor.Repository;
    Request = githubRequest(Token);
  } else {
    Request.Limit = 1024 * 1024;
    Request.Headers["Authorization"] = "Bearer " + Token;
  }
  auto Response = fetch(Endpoint, std::move(Request), Body);
  if (Response.Status != 200)
    responseError(Response.Status, Descriptor.Url);
}
}
}

namespace llm::runtime {
namespace {
namespace {
struct Cleanup {
  fs::path Root;
  ~Cleanup() {
    std::error_code Error;
    fs::remove_all(Root, Error);
  }
};
std::string inventory(Context &Call, const fs::path &Root) {
  std::map<std::string, std::string> Entries;
  for (const auto &Entry : fs::recursive_directory_iterator(Root)) {
    cancelled(Call);
    auto Relative = pathText(Entry.path().lexically_relative(Root));
    std::ostringstream Line;
    Line << std::quoted(Relative) << ' ';
    auto Status = Entry.symlink_status();
    if (fs::is_symlink(Status))
      Line << "link " << std::quoted(pathText(fs::read_symlink(Entry.path())));
    else if (fs::is_regular_file(Status))
      Line << "file " << sha256File(Entry.path()) << ' '
           << static_cast<unsigned>(Status.permissions() &
                                    (fs::perms::owner_exec |
                                     fs::perms::group_exec |
                                     fs::perms::others_exec));
    else if (fs::is_directory(Status))
      Line << "dir";
    else
      throw Failure("Unsupported registry cache entry");
    Entries.emplace(Relative, Line.str());
  }
  std::string Result;
  for (const auto &[Path, Line] : Entries)
    Result += Line + '\n';
  return Result;
}
void copyTree(Context &Call, const fs::path &RegistrySource,
              const fs::path &Destination) {
  fs::create_directories(Destination);
  for (const auto &Entry : fs::recursive_directory_iterator(RegistrySource)) {
    cancelled(Call);
    auto Target = Destination / Entry.path().lexically_relative(RegistrySource);
    auto Status = Entry.symlink_status();
    if (fs::is_symlink(Status))
      fs::copy_symlink(Entry.path(), Target);
    else if (fs::is_directory(Status))
      fs::create_directories(Target);
    else if (fs::is_regular_file(Status)) {
      fs::create_directories(Target.parent_path());
      fs::copy_file(Entry.path(), Target);
      fs::permissions(Target, Status.permissions());
    } else
      throw Failure("Unsupported registry package entry");
  }
}
bool hashValid(const fs::path &Archive, const std::string &Hash) {
  auto Status = fs::symlink_status(Archive);
  return fs::is_regular_file(Status) && !fs::is_symlink(Status) &&
         sha256File(Archive) == Hash;
}
void rejectBuildOutputs(const fs::path &Root) {
  for (auto It = fs::recursive_directory_iterator(Root);
       It != fs::recursive_directory_iterator(); ++It) {
    auto Name = pathText(It->path().filename());
    if (Name == "obj" || Name == "bin" || Name == ".lock" ||
        Name == "compile_commands.json" ||
        (Name.starts_with(".llm-") && Name != ".llm-export"))
      throw Failure("Source package contains reserved build state: " +
                    pathText(It->path().lexically_relative(Root)));
  }
}
void checkInstance(Context &Call, const fs::path &Pristine,
                   const fs::path &Instance) {
  std::set<fs::path> Expected;
  for (const auto &Entry : fs::recursive_directory_iterator(Pristine)) {
    auto Relative = Entry.path().lexically_relative(Pristine);
    Expected.insert(Relative);
    auto Target = Instance / Relative;
    auto Status = Entry.symlink_status();
    auto Actual = fs::symlink_status(Target);
    if (fs::is_symlink(Status)) {
      if (!fs::is_symlink(Actual) ||
          fs::read_symlink(Entry.path()) != fs::read_symlink(Target))
        throw Failure("Modified package instance link: " + pathText(Target));
    } else if (fs::is_directory(Status)) {
      if (!fs::is_directory(Actual) || fs::is_symlink(Actual))
        throw Failure("Modified package instance directory: " +
                      pathText(Target));
    } else {
      if (fs::is_symlink(Actual) || fs::is_directory(Actual))
        throw Failure("Modified package source type in build instance: " +
                      pathText(Target));
      if (!fs::is_regular_file(Actual) ||
          sha256File(Entry.path()) != sha256File(Target) ||
          Actual.permissions() != Status.permissions())
        throw Failure("Modified package source in build instance: " +
                      pathText(Target));
    }
  }
  for (auto It = fs::recursive_directory_iterator(Instance);
       It != fs::recursive_directory_iterator(); ++It) {
    auto Relative = It->path().lexically_relative(Instance);
    auto Name = pathText(It->path().filename());
    if (Expected.contains(Relative))
      continue;
    if ((Name == "obj" || Name == "bin") &&
        fs::is_directory(It->symlink_status()) &&
        fs::is_regular_file(It->path().parent_path() / ".llm")) {
      It.disable_recursion_pending();
      continue;
    }
    if ((Name == ".lock" || Name == "compile_commands.json") &&
        fs::is_regular_file(It->symlink_status()) &&
        fs::is_regular_file(It->path().parent_path() / ".llm"))
      continue;
    throw Failure("Foreign file in package build instance: " +
                  pathText(It->path()));
  }
}
}
Cache::Cache(Context &Call, fs::path CacheRoot)
    : Call(Call), Root(CacheRoot.empty()
                           ? (Call.UserDirectory.empty() ? userHome()
                                                         : Call.UserDirectory) /
                                 ".llm/cache"
                           : std::move(CacheRoot)) {
  rejectSymlinkAncestors(Root);
  fs::create_directories(Root);
  protectPrivatePath(Root, true);
  Root = fs::canonical(Root);
  auto Marker = Root / ".registry-cache";
  rejectSymlinkAncestors(Root / ".cache.lock");
  FileLock Lock(Call, Root / ".cache.lock", true);
  rejectSymlinkAncestors(Marker);
  if (fs::exists(Marker)) {
    if (!fs::is_regular_file(Marker))
      throw Failure("Unknown registry cache ownership marker");
  } else {
    for (const auto *Directory : {"archives", "sources", "instances", "locks"})
      if (fs::exists(fs::symlink_status(Root / Directory)))
        throw Failure("Refusing to adopt an unowned registry cache directory");
  }
  atomicWrite(Marker, "");
  for (const auto *Directory : {"archives", "sources", "instances", "locks"}) {
    rejectSymlinkAncestors(Root / Directory);
    fs::create_directories(Root / Directory);
  }
}
fs::path Cache::sources(const std::string &Identity, const std::string &Name,
                        const PackageVersion &Version,
                        const Download &Download) {
  if (Version.Sha256.size() != 64 ||
      !std::ranges::all_of(Version.Sha256, [](char C) {
        return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
      }))
    throw Failure("Invalid registry archive hash");
  auto Key = sha256Text(Identity + '\n' + Version.Selected.Text + '\n' +
                        Version.Sha256);
  FileLock Lock(Call, Root / "locks" / Key, true);
  auto Archive = Root / "archives" / Version.Sha256;
  rejectSymlinkAncestors(Archive);
  if (fs::exists(fs::symlink_status(Archive)) &&
      !hashValid(Archive, Version.Sha256))
    throw Failure("Corrupt registry archive: " + pathText(Archive));
  if (!fs::exists(fs::symlink_status(Archive))) {
    auto Temporary = Root / "archives" / (".download-" + uniqueId());
    Cleanup Cleanup{Temporary};
    Download(Temporary);
    if (!hashValid(Temporary, Version.Sha256))
      throw Failure("Registry archive SHA-256 mismatch for " + Identity + "@" +
                    Version.Selected.Text);
    replaceFile(Temporary, Archive);
  }
  auto Container = Root / "sources" / Key, Receipt = Container / "receipt",
       Content = Container / "content";
  rejectSymlinkAncestors(Container);
  if (fs::exists(fs::symlink_status(Container))) {
    if (!fs::is_directory(Content) || !fs::is_regular_file(Receipt) ||
        readText(Receipt) != inventory(Call, Content))
      throw Failure("Corrupt registry source cache: " + pathText(Container));
    std::vector<fs::path> Entries;
    for (const auto &Entry : fs::directory_iterator(Content))
      Entries.push_back(Entry.path());
    if (Entries.size() != 1 || !fs::is_regular_file(Entries.front() / ".llm"))
      throw Failure("Invalid registry source package layout: " +
                    pathText(Container));
    return Entries.front();
  }
  auto Staging = Root / "sources" / (".extract-" + uniqueId());
  fs::create_directory(Staging);
  Cleanup Cleanup{Staging};
  auto Unpacked = Staging / "unpacked";
  fs::create_directory(Unpacked);
  std::ifstream Input(Archive, std::ios::binary);
  unsigned char Magic[4]{};
  Input.read(reinterpret_cast<char *>(Magic), 4);
  SdkPackage Package;
  Package.MaxExpanded = 2ULL * 1024 * 1024 * 1024;
  if (Magic[0] == 0x1f && Magic[1] == 0x8b)
    Package.Format = "tar.gz";
  else if (Magic[0] == 'P' && Magic[1] == 'K' && Magic[2] == 3 && Magic[3] == 4)
    Package.Format = "zip";
  else
    throw Failure("Registry archives must be tar.gz or ZIP");
  extractPackage(Call, Archive, Unpacked, Package);
  fs::path Project = Unpacked;
  std::string Directory = Name;
  if (!fs::is_regular_file(Unpacked / ".llm")) {
    std::vector<fs::path> Entries;
    for (const auto &Entry : fs::directory_iterator(Unpacked))
      Entries.push_back(Entry.path());
    if (Entries.size() != 1 || !fs::is_directory(Entries.front()) ||
        !fs::is_regular_file(Entries.front() / ".llm"))
      throw Failure("Registry archive must contain .llm at its root or in one "
                    "sole wrapper directory");
    Project = Entries.front();
    Directory = pathText(Project.filename());
  }
  rejectBuildOutputs(Project);
  auto Manifest = readManifest(Project);
  if (!isLibrary(Manifest.Type))
    throw Failure("Registry package must declare a library product");
  fs::create_directories(Staging / "content");
  fs::rename(Project, Staging / "content" / pathFromText(Directory));
  fs::remove_all(Unpacked);
  writeText(Staging / "receipt", inventory(Call, Staging / "content"));
  fs::rename(Staging, Container);
  return Container / "content" / pathFromText(Directory);
}
fs::path Cache::instance(const fs::path &RegistrySource,
                         const std::string &Identity,
                         const PackageVersion &Version, const fs::path &Owner,
                         std::string_view Triple, bool Release) {
  auto Key = sha256Text(
      Identity + '\n' + Version.Sha256 + '\n' + pathText(fs::canonical(Owner)) +
      '\n' + std::string(Triple) + (Release ? "\nrelease" : "\ndebug"));
  auto Container = Root / "instances" / Key,
       Instance = Container / RegistrySource.filename();
  FileLock Lock(Call, Root / "locks" / ("instance-" + Key), true);
  rejectSymlinkAncestors(Container);
  if (fs::exists(fs::symlink_status(Container))) {
    if (!fs::is_directory(Container) ||
        !fs::is_directory(fs::symlink_status(Instance)))
      throw Failure("Corrupt registry build instance: " + pathText(Container));
    for (const auto &Entry : fs::directory_iterator(Container))
      if (Entry.path() != Instance)
        throw Failure("Unexpected registry build instance entry: " +
                      pathText(Entry.path()));
    rejectSymlinkAncestors(Instance / "obj");
    fs::create_directories(Instance / "obj");
    FileLock BuildLock(Call, Instance / "obj/.guard", true);
    checkInstance(Call, RegistrySource, Instance);
    return Instance;
  }
  auto Staging = Root / "instances" / (".instance-" + uniqueId());
  Cleanup Cleanup{Staging};
  fs::create_directories(Staging);
  copyTree(Call, RegistrySource, Staging / RegistrySource.filename());
  fs::rename(Staging, Container);
  return Instance;
}
fs::path Cache::peek(Context &Call, const std::string &Identity,
                     const PackageVersion &Version, fs::path Root) {
  if (Root.empty())
    Root = (Call.UserDirectory.empty() ? userHome() : Call.UserDirectory) /
           ".llm/cache";
  auto Key = sha256Text(Identity + '\n' + Version.Selected.Text + '\n' +
                        Version.Sha256);
  auto Container = Root / "sources" / Key, Content = Container / "content";
  if (!fs::exists(fs::symlink_status(Container)))
    return {};
  rejectSymlinkAncestors(Content);
  rejectSymlinkAncestors(Container / "receipt");
  if (!fs::is_regular_file(Container / "receipt") ||
      !fs::is_directory(Content) ||
      readText(Container / "receipt") != inventory(Call, Content))
    throw Failure("Corrupt registry source cache: " + pathText(Container));
  std::vector<fs::path> Entries;
  for (const auto &Entry : fs::directory_iterator(Content))
    Entries.push_back(Entry.path());
  if (Entries.size() != 1 || !fs::is_regular_file(Entries.front() / ".llm"))
    throw Failure("Invalid registry source layout: " + pathText(Container));
  return Entries.front();
}
}
}

namespace llm::runtime {
namespace {
namespace {
struct Edit {
  std::size_t Begin, End;
  std::string Text;
};
std::size_t offset(std::string_view Text, toml::source_position Position) {
  std::size_t Index = 0;
  for (std::size_t Line = 1; Line < Position.line; ++Line) {
    auto Next = Text.find('\n', Index);
    if (Next == std::string_view::npos)
      throw Failure("Invalid TOML edit position");
    Index = Next + 1;
  }
  for (std::size_t Column = 1; Column < Position.column; ++Column) {
    if (Index >= Text.size())
      throw Failure("Invalid TOML edit column");
    ++Index;
    while (Index < Text.size() &&
           (static_cast<unsigned char>(Text[Index]) & 0xc0) == 0x80)
      ++Index;
  }
  return Index;
}
std::string tomlString(std::string_view Value) {
  std::string Result = "\"";
  for (unsigned char C : Value) {
    if (C == '"' || C == '\\')
      Result += '\\';
    if (C < 0x20 || C == 0x7f)
      throw Failure("Invalid control character in dependency");
    Result += static_cast<char>(C);
  }
  return Result + '"';
}
std::string identity(const DependencyReference &Reference) {
  if (Reference.Local)
    return "local:" + pathText(fs::weakly_canonical(*Reference.Local));
  return packageIdentity(take(parseRegistrySource(Reference.Registry)),
                         Reference.Package);
}
std::size_t comma(std::string_view Text, std::size_t Begin, std::size_t End) {
  bool Comment = false;
  for (auto Index = Begin; Index < End; ++Index) {
    if (Text[Index] == '\n')
      Comment = false;
    else if (!Comment && Text[Index] == '#')
      Comment = true;
    else if (!Comment && Text[Index] == ',')
      return Index;
  }
  return std::string::npos;
}
}
std::string editDependency(std::string_view Text, const fs::path &File,
                           const std::string &Dependency, bool Remove,
                           bool PublicDependency) {
  auto Manifest = parseManifest(Text, File);
  if (PublicDependency && !isLibrary(Manifest.Type))
    throw Failure("--public requires a library product");
  auto Document = toml::parse(Text, pathText(File));
  const bool ShortRemove =
      Remove && Dependency.find_first_of(":/\\@") == std::string::npos;
  std::optional<DependencyReference> Requested;
  if (!ShortRemove)
    Requested = take(dependencyReference(Manifest, Dependency, PublicDependency,
                                         {File, 1, 1}));
  struct Match {
    const toml::node *Value;
    const toml::array *Array;
    bool IsPublic;
  };
  std::vector<Match> Matches;
  auto Equal = [&](const std::string &Spelling) {
    auto Reference =
        take(dependencyReference(Manifest, Spelling, false, {File, 1, 1}));
    return ShortRemove ? !Reference.Local && Reference.Package == Dependency
                       : identity(Reference) == identity(*Requested);
  };
  auto Dependencies = Document.get_as<toml::table>("dependencies");
  if (Dependencies)
    for (bool Visibility : {false, true}) {
      if (auto Array = Dependencies->get_as<toml::array>(
              Visibility ? "public" : "private"))
        for (const auto &Value : *Array)
          if (Equal(*Value.value<std::string>()))
            Matches.push_back({&Value, Array, Visibility});
    }
  bool PlatformMatch = false;
  for (const auto &[Platform, Layer] : Manifest.Platforms)
    for (const auto *Values :
         {&Layer.Dependencies.Private, &Layer.Dependencies.Public})
      for (const auto &Spelling : *Values)
        if (Equal(Spelling))
          PlatformMatch = true;
  if (Matches.size() > 1)
    throw Failure("Dependency matches multiple base declarations. Use a "
                  "qualified name or edit the declarations explicitly");
  if (Matches.empty() && PlatformMatch)
    throw Failure("Dependency exists only in platform-specific declarations. "
                  "edit the intended platform explicitly");
  if (Remove && Matches.empty())
    throw Failure("No matching direct dependency");
  auto Visibility =
      PublicDependency || (Matches.size() == 1 && Matches.front().IsPublic);
  std::vector<Edit> Edits;
  if (!Matches.empty()) {
    auto Match = Matches.front();
    auto Begin = offset(Text, Match.Value->source().begin),
         End = offset(Text, Match.Value->source().end);
    if (!Remove && Match.IsPublic == Visibility)
      Edits.push_back({Begin, End, tomlString(Dependency)});
    else {
      Edits.push_back({Begin, End, {}});
      const toml::node *PreviousValue = nullptr;
      const toml::node *NextValue = nullptr;
      bool Passed = false;
      for (const auto &Value : *Match.Array) {
        if (&Value == Match.Value) {
          Passed = true;
          continue;
        }
        if (Passed) {
          NextValue = &Value;
          break;
        }
        PreviousValue = &Value;
      }
      auto Following =
          comma(Text, End,
                NextValue ? offset(Text, NextValue->source().begin)
                          : offset(Text, Match.Array->source().end) - 1);
      if (Following != std::string::npos)
        Edits.push_back({Following, Following + 1, {}});
      else if (PreviousValue) {
        auto Previous =
            comma(Text, offset(Text, PreviousValue->source().end), Begin);
        if (Previous != std::string::npos)
          Edits.push_back({Previous, Previous + 1, {}});
      }
    }
  }
  if (!Remove && (Matches.empty() || Matches.front().IsPublic != Visibility)) {
    auto Array = Dependencies ? Dependencies->get_as<toml::array>(
                                    Visibility ? "public" : "private")
                              : nullptr;
    if (Array) {
      auto End = offset(Text, Array->source().end) - 1;
      if (Array->empty())
        Edits.push_back({End, End, " " + tomlString(Dependency)});
      else {
        auto Last = offset(Text, Array->back().source().end);
        if (comma(Text, Last, End) == std::string::npos)
          Edits.push_back({Last, Last, ", " + tomlString(Dependency)});
        else
          Edits.push_back({End, End, " " + tomlString(Dependency) + ","});
      }
    } else if (Dependencies && Dependencies->is_inline()) {
      auto End = offset(Text, Dependencies->source().end) - 1;
      Edits.push_back({End, End,
                       (Dependencies->empty() ? "" : ", ") +
                           std::string(Visibility ? "public" : "private") +
                           " = [" + tomlString(Dependency) + "]"});
    } else {
      auto Newline =
          Text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
      std::string Key = Visibility ? "public" : "private";
      std::size_t Insertion = 0;
      bool Header = false;
      if (Dependencies) {
        auto Start = offset(Text, Dependencies->source().begin);
        if (Start < Text.size() && Text[Start] == '[') {
          auto End = Text.find('\n', Start);
          Insertion = End == std::string_view::npos ? Text.size() : End + 1;
          Header = true;
        }
      }
      if (!Header) {
        Key = "dependencies." + Key;
        if (Text.starts_with("\xef\xbb\xbf"))
          Insertion = 3;
      }
      auto Prefix = Insertion && Text[Insertion - 1] != '\n' ? Newline : "";
      Edits.push_back({Insertion, Insertion,
                       std::string(Prefix) + Key + " = [" +
                           tomlString(Dependency) + "]" + Newline});
    }
  }
  std::ranges::sort(Edits, [](const auto &Left, const auto &Right) {
    return Left.Begin > Right.Begin;
  });
  std::string Result(Text);
  for (const auto &Edit : Edits)
    Result.replace(Edit.Begin, Edit.End - Edit.Begin, Edit.Text);
  parseManifest(Result, File);
  return Result;
}
}
void downloadToolchainArchive(Context &Call, const std::string &Url,
                              const Path &Output, std::uint64_t Limit) {
  auto Address = registryAddress(Url);
  if (Address.Scheme != "https")
    throw Failure("Toolchain archives require HTTPS");
  auto Parts = pathParts(Address.Resource);
  if (Address.Host == "github.com" && Address.Port == 443 &&
      Parts.size() == 6 && Parts[2] == "releases" && Parts[3] == "download") {
    auto Source =
        registrySource("https://github.com/" + Parts[0] + "/" + Parts[1]);
    Credentials Store(Call);
    Client Network(Call, Store, Output.parent_path());
    Network.download(Source.Url, Url, Output, Limit);
    return;
  }
  auto Response = httpsGet(Call, Url, Output, HttpRequest{.Limit = Limit});
  if (Response.Status != 200)
    throw Failure("SDK package server returned HTTP " +
                  std::to_string(Response.Status));
}
}
