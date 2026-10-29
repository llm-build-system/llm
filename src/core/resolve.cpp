module llm.core;

import std;

namespace llm {
namespace {
bool digits(std::string_view Text) {
  return !Text.empty() && std::ranges::all_of(Text, [](char Byte) {
    return Byte >= '0' && Byte <= '9';
  });
}

bool number(std::string_view Text) {
  return digits(Text) && (Text.size() == 1 || Text.front() != '0');
}

int compareNumber(std::string_view Left, std::string_view Right) {
  if (Left.size() != Right.size())
    return Left.size() < Right.size() ? -1 : 1;
  return Left == Right ? 0 : Left < Right ? -1 : 1;
}

Strings split(std::string_view Text, char Separator) {
  Strings Result;
  std::size_t Start = 0;
  for (;;) {
    auto End = Text.find(Separator, Start);
    Result.emplace_back(
        Text.substr(Start, End == std::string_view::npos ? End : End - Start));
    if (End == std::string_view::npos)
      return Result;
    Start = End + 1;
  }
}

bool identifier(std::string_view Text) {
  return !Text.empty() && std::ranges::all_of(Text, [](char Byte) {
    return (Byte >= '0' && Byte <= '9') || (Byte >= 'A' && Byte <= 'Z') ||
           (Byte >= 'a' && Byte <= 'z') || Byte == '-';
  });
}

bool packageName(std::string_view Text) {
  if (Text.empty() || Text.front() < 'a' || Text.front() > 'z' ||
      !((Text.back() >= 'a' && Text.back() <= 'z') ||
        (Text.back() >= '0' && Text.back() <= '9')))
    return false;
  return std::ranges::all_of(Text, [](char Byte) {
    return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
           Byte == '-';
  });
}

bool validUtf8(std::string_view Text) {
  for (std::size_t Position = 0; Position < Text.size();) {
    auto First = static_cast<unsigned char>(Text[Position++]);
    if (First < 128)
      continue;
    unsigned Count = First >= 0xc2 && First <= 0xdf   ? 1
                     : First >= 0xe0 && First <= 0xef ? 2
                     : First >= 0xf0 && First <= 0xf4 ? 3
                                                      : 4;
    if (Count == 4 || Position + Count > Text.size())
      return false;
    unsigned Value = First & ((1u << (6 - Count)) - 1);
    for (unsigned Index = 0; Index < Count; ++Index) {
      auto Next = static_cast<unsigned char>(Text[Position++]);
      if ((Next & 0xc0) != 0x80)
        return false;
      Value = (Value << 6) | (Next & 63);
    }
    if ((Count == 1 && Value < 0x80) || (Count == 2 && Value < 0x800) ||
        (Count == 3 && Value < 0x10000) || Value > 0x10ffff ||
        (Value >= 0xd800 && Value <= 0xdfff))
      return false;
  }
  return true;
}

void lowerAscii(std::string &Text) {
  for (auto &Byte : Text)
    if (Byte >= 'A' && Byte <= 'Z')
      Byte += 'a' - 'A';
}
}

Result<Version> parseVersion(std::string_view Text) {
  if (Text.empty() || Text.size() > 4096)
    return std::unexpected(Diagnostic{"Invalid SemVer version"});
  Version Result;
  Result.Text = Text;
  auto Plus = Text.find('+');
  auto Main = Text.substr(0, Plus);
  if (Plus != std::string_view::npos)
    for (const auto &Part : split(Text.substr(Plus + 1), '.'))
      if (!identifier(Part))
        return std::unexpected(Diagnostic{"Invalid SemVer build metadata"});
  auto Minus = Main.find('-');
  auto Parts = split(Main.substr(0, Minus), '.');
  if (Parts.size() != 3)
    return std::unexpected(Diagnostic{"SemVer requires major.minor.patch"});
  for (std::size_t Index = 0; Index < 3; ++Index) {
    if (!number(Parts[Index]))
      return std::unexpected(Diagnostic{"Invalid SemVer numeric component"});
    Result.Components[Index] = Parts[Index];
  }
  if (Minus != std::string_view::npos) {
    Result.Prerelease = split(Main.substr(Minus + 1), '.');
    for (const auto &Part : Result.Prerelease)
      if (!identifier(Part) || (digits(Part) && !number(Part)))
        return std::unexpected(Diagnostic{"Invalid SemVer prerelease"});
  }
  return Result;
}

int compareVersions(const Version &Left, const Version &Right) {
  for (std::size_t Index = 0; Index < 3; ++Index)
    if (auto Compared =
            compareNumber(Left.Components[Index], Right.Components[Index]))
      return Compared;
  if (Left.Prerelease.empty() != Right.Prerelease.empty())
    return Left.Prerelease.empty() ? 1 : -1;
  for (std::size_t Index = 0;
       Index < std::min(Left.Prerelease.size(), Right.Prerelease.size());
       ++Index) {
    const auto &A = Left.Prerelease[Index];
    const auto &B = Right.Prerelease[Index];
    if (A == B)
      continue;
    if (digits(A) && digits(B))
      return compareNumber(A, B);
    if (digits(A) != digits(B))
      return digits(A) ? -1 : 1;
    return A < B ? -1 : 1;
  }
  return Left.Prerelease.size() == Right.Prerelease.size()  ? 0
         : Left.Prerelease.size() < Right.Prerelease.size() ? -1
                                                            : 1;
}

Result<VersionRequirement> parseRequirement(std::string_view Text) {
  VersionRequirement Result;
  if (Text.starts_with('[') || Text.starts_with('(')) {
    auto Comma = Text.find(',');
    if (Text.size() < 5 || (Text.back() != ')' && Text.back() != ']') ||
        Comma == std::string_view::npos ||
        Text.find(',', Comma + 1) != std::string_view::npos)
      return std::unexpected(Diagnostic{
          "Expected an interval with two bounds, such as [1.2.3, 2.0.0)"});
    auto Trim = [](std::string_view Bound) {
      auto First = Bound.find_first_not_of(" \t");
      if (First == std::string_view::npos)
        return std::string_view{};
      return Bound.substr(First, Bound.find_last_not_of(" \t") - First + 1);
    };
    Result.IncludeMinimum = Text.front() == '[';
    Result.IncludeMaximum = Text.back() == ']';
    auto Bounds =
        std::array{Trim(Text.substr(1, Comma - 1)),
                   Trim(Text.substr(Comma + 1, Text.size() - Comma - 2))};
    for (std::size_t Index = 0; Index < Bounds.size(); ++Index) {
      bool Included =
          Index == 0 ? Result.IncludeMinimum : Result.IncludeMaximum;
      if (Bounds[Index].empty())
        return std::unexpected(Diagnostic{
            "Interval bounds cannot be empty; use * for an unbounded limit"});
      if (Bounds[Index] == "*") {
        if (Included)
          return std::unexpected(Diagnostic{
              "An unbounded interval limit requires a round bracket"});
        continue;
      }
      auto Bound = parseVersion(Bounds[Index]);
      if (!Bound)
        return std::unexpected(Bound.error());
      if (!Bound->Prerelease.empty())
        return std::unexpected(
            Diagnostic{"Prereleases require an exact version requirement"});
      (Index == 0 ? Result.Minimum : Result.Maximum) = std::move(*Bound);
    }
    if (Result.Minimum && Result.Maximum) {
      auto Order = compareVersions(*Result.Minimum, *Result.Maximum);
      if (Order > 0 ||
          (Order == 0 && (!Result.IncludeMinimum || !Result.IncludeMaximum)))
        return std::unexpected(Diagnostic{
            "Interval bounds are reversed or exclude their only version"});
    }
    return Result;
  }
  if (Text.starts_with('>') || Text.starts_with('<') || Text.starts_with('='))
    return std::unexpected(
        Diagnostic{"Comparison operators are not supported; use an interval"});
  std::string_view Operator;
  if (Text.starts_with('^') || Text.starts_with('~'))
    Operator = Text.substr(0, 1);
  if (!Operator.empty()) {
    auto Minimum = parseVersion(Text.substr(Operator.size()));
    if (!Minimum)
      return std::unexpected(Minimum.error());
    if (!Minimum->Prerelease.empty())
      return std::unexpected(
          Diagnostic{"Prereleases require an exact version requirement"});
    for (std::size_t Index = 0; Index < 3; ++Index) {
      if (Operator == "~" && Index == 2)
        break;
      Result.Components[Index] = Minimum->Components[Index];
      if (Operator == "^" && Minimum->Components[Index] != "0")
        break;
    }
    Result.Minimum = std::move(*Minimum);
    return Result;
  }
  if (!Text.contains('*')) {
    auto Exact = parseVersion(Text);
    if (!Exact)
      return std::unexpected(Exact.error());
    Result.Exact = std::move(*Exact);
    return Result;
  }
  if (Text == "*")
    return Result;
  auto Parts = split(Text, '.');
  if (Parts.size() != 3)
    return std::unexpected(
        Diagnostic{"Wildcard requirements require three components"});
  for (std::size_t Index = 0; Index < 3; ++Index) {
    if (Parts[Index] == "*")
      continue;
    if (!number(Parts[Index]))
      return std::unexpected(
          Diagnostic{"Invalid wildcard version requirement"});
    Result.Components[Index] = Parts[Index];
  }
  return Result;
}

bool VersionRequirement::matches(const Version &Candidate) const {
  if (Exact)
    return Exact->Text == Candidate.Text;
  if (!Candidate.Prerelease.empty())
    return false;
  if (Minimum) {
    auto Order = compareVersions(Candidate, *Minimum);
    if (Order < 0 || (Order == 0 && !IncludeMinimum))
      return false;
  }
  if (Maximum) {
    auto Order = compareVersions(Candidate, *Maximum);
    if (Order > 0 || (Order == 0 && !IncludeMaximum))
      return false;
  }
  for (std::size_t Index = 0; Index < 3; ++Index)
    if (Components[Index] && *Components[Index] != Candidate.Components[Index])
      return false;
  return true;
}

Result<Url> parseRegistryUrl(std::string_view Text) {
  if (Text.starts_with("file://")) {
    auto Resource = Text.substr(7);
    if (Resource.starts_with("localhost/"))
      Resource.remove_prefix(9);
    if (!Resource.starts_with('/') || Resource.starts_with("//") ||
        Resource.find_first_of("\r\n\t #?\\\0", 0, 9) != std::string_view::npos)
      return std::unexpected(Diagnostic{"Expected an absolute local file URL"});
    for (std::size_t I = 0; I < Resource.size(); ++I)
      if (Resource[I] == '%') {
        auto Hex = [](char C) {
          return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') ||
                 (C >= 'A' && C <= 'F');
        };
        if (I + 2 >= Resource.size() || !Hex(Resource[I + 1]) ||
            !Hex(Resource[I + 2]))
          return std::unexpected(Diagnostic{"Invalid file URL escape"});
        auto Escape = std::string(Resource.substr(I + 1, 2));
        lowerAscii(Escape);
        if (Escape == "00" || Escape == "2f" || Escape == "5c" ||
            Escape == "0a" || Escape == "0d")
          return std::unexpected(Diagnostic{"Unsafe file URL escape"});
        I += 2;
      }
    return Url{"file", {}, 0, "file://", std::string(Resource)};
  }
  auto Secure = Text.starts_with("https://");
  auto Prefix = Secure ? 8U : 7U;
  if ((!Secure && !Text.starts_with("http://")) ||
      Text.find_first_of("\r\n\t #\\\0", 0, 8) != std::string_view::npos)
    return std::unexpected(
        Diagnostic{"Expected an HTTP, HTTPS or local file URL without "
                   "credentials, whitespace or fragments"});
  auto End = Text.find_first_of("/?", Prefix);
  auto Authority =
      Text.substr(Prefix, End == std::string_view::npos ? Text.size() - Prefix
                                                        : End - Prefix);
  if (Authority.empty() || Authority.contains('@'))
    return std::unexpected(Diagnostic{"URL credentials are forbidden"});
  Url Result;
  Result.Scheme = Secure ? "https" : "http";
  Result.Port = Secure ? 443 : 80;
  std::string_view Port;
  if (Authority.starts_with('[')) {
    auto Close = Authority.find(']');
    if (Close == std::string_view::npos)
      return std::unexpected(Diagnostic{"Invalid IPv6 URL"});
    Result.Host = Authority.substr(1, Close - 1);
    if (Close + 1 < Authority.size()) {
      if (Authority[Close + 1] != ':')
        return std::unexpected(Diagnostic{"Invalid URL authority"});
      Port = Authority.substr(Close + 2);
    }
  } else {
    auto Colon = Authority.find(':');
    Result.Host = Authority.substr(0, Colon);
    if (Colon != std::string_view::npos)
      Port = Authority.substr(Colon + 1);
  }
  if (Result.Host.empty() ||
      std::ranges::any_of(Result.Host, [](unsigned char Byte) {
        return Byte <= 32 || Byte >= 127 || Byte == '\\' || Byte == '%';
      }))
    return std::unexpected(Diagnostic{"Invalid HTTPS host"});
  lowerAscii(Result.Host);
  if (!Port.empty()) {
    auto [Last, Error] =
        std::from_chars(Port.data(), Port.data() + Port.size(), Result.Port);
    if (Error != std::errc{} || Last != Port.data() + Port.size() ||
        Result.Port == 0 || Result.Port > 65535)
      return std::unexpected(Diagnostic{"Invalid HTTPS port"});
  } else if (Authority.ends_with(':')) {
    return std::unexpected(Diagnostic{"Empty HTTPS port"});
  }
  Result.Origin =
      Result.Scheme + "://" +
      (Result.Host.contains(':') ? "[" + Result.Host + "]" : Result.Host);
  if (Result.Port != (Secure ? 443U : 80U))
    Result.Origin += ":" + std::to_string(Result.Port);
  Result.Resource =
      End == std::string_view::npos ? "/" : std::string(Text.substr(End));
  if (Result.Resource.starts_with('?'))
    Result.Resource.insert(Result.Resource.begin(), '/');
  return Result;
}

Result<Url> parseHttpsUrl(std::string_view Text) {
  if (!Text.starts_with("https://"))
    return std::unexpected(Diagnostic{"Expected an HTTPS URL"});
  return parseRegistryUrl(Text);
}

Result<RegistrySource> parseRegistrySource(std::string_view Text) {
  auto Parsed = parseRegistryUrl(Text);
  if (!Parsed)
    return std::unexpected(Parsed.error());
  if (Parsed->Resource.contains('?') ||
      Text.find_first_of("|\"") != std::string_view::npos)
    return std::unexpected(
        Diagnostic{"Registry URLs cannot contain queries or lock delimiters"});
  auto Resource = Parsed->Resource;
  while (Resource.size() > 1 && Resource.back() == '/')
    Resource.pop_back();
  auto Parts = split(Resource, '/');
  for (const auto &Part : Parts)
    if (Part == "." || Part == "..")
      return std::unexpected(Diagnostic{"Registry URL contains dot segments"});
  RegistrySource Result;
  Result.Github = Parsed->Scheme == "https" && Parsed->Host == "github.com" &&
                  Parsed->Port == 443;
  if (Result.Github) {
    if (Parts.size() != 3 || !Parts[0].empty() || Parts[1].empty() ||
        Parts[2].empty())
      return std::unexpected(
          Diagnostic{"GitHub registry URL must identify one repository"});
    Result.Owner = Parts[1];
    Result.Repository = Parts[2];
    if (Result.Repository.ends_with(".git"))
      Result.Repository.resize(Result.Repository.size() - 4);
    for (auto *Part : {&Result.Owner, &Result.Repository}) {
      if (Part->empty() || !std::ranges::all_of(*Part, [](char Byte) {
            return (Byte >= 'A' && Byte <= 'Z') ||
                   (Byte >= 'a' && Byte <= 'z') ||
                   (Byte >= '0' && Byte <= '9') || Byte == '-' || Byte == '_' ||
                   Byte == '.';
          }))
        return std::unexpected(Diagnostic{"Invalid GitHub repository URL"});
      lowerAscii(*Part);
    }
    Result.Url = "https://github.com/" + Result.Owner + "/" + Result.Repository;
  } else {
    Result.Url = Parsed->Origin +
                 (Resource == "/" && Parsed->Scheme != "file" ? "" : Resource);
  }
  return Result;
}

Result<std::vector<PackageVersion>>
parsePackageVersions(std::string_view Text) {
  if (!validUtf8(Text))
    return std::unexpected(Diagnostic{"Registry metadata is not UTF-8"});
  std::vector<PackageVersion> Result;
  std::set<std::string> Seen;
  std::istringstream Stream{std::string(Text)};
  std::string Line;
  while (std::getline(Stream, Line)) {
    if (Line.ends_with('\r'))
      Line.pop_back();
    auto Colon = Line.rfind(':');
    if (Colon == std::string::npos)
      return std::unexpected(Diagnostic{"Invalid registry package record"});
    auto Hash = Line.substr(Colon + 1);
    auto Left = Line.substr(0, Colon);
    auto At = Left.rfind('@');
    if (Hash.size() != 64 || At == std::string::npos ||
        !std::ranges::all_of(Hash, [](char Byte) {
          return (Byte >= '0' && Byte <= '9') || (Byte >= 'a' && Byte <= 'f') ||
                 (Byte >= 'A' && Byte <= 'F');
        }))
      return std::unexpected(
          Diagnostic{"Invalid registry version/hash record"});
    auto Selected = parseVersion(Left.substr(At + 1));
    if (!Selected)
      return std::unexpected(Selected.error());
    auto Url = Left.substr(0, At);
    if (auto Valid = parseRegistryUrl(Url); !Valid)
      return std::unexpected(Valid.error());
    if (!Seen.insert(Selected->Text).second)
      return std::unexpected(
          Diagnostic{"Duplicate version in registry package metadata"});
    lowerAscii(Hash);
    Result.push_back({std::move(*Selected), std::move(Url), std::move(Hash)});
  }
  if (Result.empty())
    return std::unexpected(Diagnostic{"Registry package contains no versions"});
  std::ranges::sort(Result, [](const auto &Left, const auto &Right) {
    auto Compared = compareVersions(Left.Selected, Right.Selected);
    return Compared ? Compared > 0 : Left.Selected.Text > Right.Selected.Text;
  });
  return Result;
}

std::string packageIdentity(const RegistrySource &Source,
                            std::string_view Name) {
  return Source.Url + "|" + std::string(Name);
}

Result<DependencyReference> dependencyReference(const Manifest &Project,
                                                std::string Spelling,
                                                bool Public,
                                                SourceLocation Location) {
  DependencyReference Result;
  Result.Spelling = std::move(Spelling);
  Result.Public = Public;
  Result.Location = std::move(Location);
  const auto &Text = Result.Spelling;
  auto Fail = [&](std::string Message) -> llm::Result<DependencyReference> {
    return std::unexpected(Diagnostic{std::move(Message), Result.Location});
  };
  if (Text.starts_with("./") || Text.starts_with("../") ||
      Text.starts_with(".\\") || Text.starts_with("..\\") ||
      pathFromText(Text).is_absolute()) {
    auto File = pathFromText(Text);
    Result.Local =
        (File.is_absolute() ? File : Project.Root / File).lexically_normal();
    return Result;
  }
  if (Text.size() >= 3 && Text[1] == ':' && (Text[2] == '/' || Text[2] == '\\'))
    return Fail("Windows dependency paths require a Windows host: " + Text);
  auto At = Text.find('@');
  auto Colon = Text.find(':');
  auto Qualified = Text.substr(0, At);
  if (Colon != std::string::npos && (At == std::string::npos || Colon < At)) {
    auto Alias = Qualified.substr(0, Colon);
    auto Found = Project.Registries.find(Alias);
    if (Found == Project.Registries.end())
      return Fail("Unknown registry '" + Alias + "'");
    Result.Registry = Found->second;
    Result.Package = Qualified.substr(Colon + 1);
  } else {
    if (Project.Registries.size() != 1)
      return Fail(Project.Registries.empty()
                      ? "Registry dependency requires an explicit registry"
                      : "Ambiguous package source; qualify the dependency with "
                        "a registry alias");
    Result.Registry = Project.Registries.begin()->second;
    Result.Package = Qualified;
  }
  if (!packageName(Result.Package))
    return Fail("Invalid registry package name: " + Result.Package);
  Result.RequirementText = At == std::string::npos ? "*" : Text.substr(At + 1);
  auto Requirement = parseRequirement(Result.RequirementText);
  if (!Requirement)
    return Fail(Requirement.error().Message);
  auto Source = parseRegistrySource(Result.Registry);
  if (!Source)
    return Fail(Source.error().Message);
  Result.Requirement = std::move(*Requirement);
  Result.Registry = std::move(Source->Url);
  return Result;
}

Result<PackageResolutionStep>
resolvePackageStep(const std::vector<PackageRequirement> &Requirements,
                   const PackageCatalog &Catalog) {
  PackageResolution Selected;
  std::optional<PackageMetadataRequest> Pending;
  Diagnostic Conflict{"Package requirements cannot be resolved"};
  std::size_t Attempts = 0;
  bool LimitReached = false;
  auto Solve = [&](auto &&Self) -> bool {
    if (++Attempts > 100000) {
      LimitReached = true;
      return false;
    }
    std::map<std::string, std::vector<PackageRequirement>> Constraints;
    std::set<std::string> Expanded;
    auto Collect = [&](auto &&Visit,
                       const PackageRequirement &Requirement) -> void {
      Constraints[Requirement.Identity].push_back(Requirement);
      auto Found = Selected.Packages.find(Requirement.Identity);
      if (Found == Selected.Packages.end() ||
          !Expanded.insert(Requirement.Identity).second)
        return;
      for (const auto &Dependency : Found->second.Dependencies)
        Visit(Visit, Dependency);
    };
    for (const auto &Requirement : Requirements)
      Collect(Collect, Requirement);
    std::string Missing;
    for (const auto &[Identity, Values] : Constraints) {
      auto Found = Selected.Packages.find(Identity);
      if (Found == Selected.Packages.end()) {
        if (Missing.empty())
          Missing = Identity;
        continue;
      }
      for (const auto &Requirement : Values)
        if (!Requirement.Requirement.matches(Found->second.Package.Selected)) {
          Conflict = {"Conflicting requirements for " + Identity,
                      {},
                      {Requirement.Chain}};
          return false;
        }
    }
    if (Missing.empty()) {
      std::map<std::string, unsigned> State;
      auto Visit = [&](auto &&SelfVisit, const std::string &Identity) -> bool {
        if (State[Identity] == 1)
          return false;
        if (State[Identity] == 2)
          return true;
        State[Identity] = 1;
        for (const auto &Dependency :
             Selected.Packages.at(Identity).Dependencies)
          if (!SelfVisit(SelfVisit, Dependency.Identity))
            return false;
        State[Identity] = 2;
        return true;
      };
      for (const auto &[Identity, Value] : Constraints)
        if (!Visit(Visit, Identity)) {
          Conflict = {"Dependency cycle during package resolution: " +
                      Identity};
          return false;
        }
      std::erase_if(Selected.Packages, [&](const auto &Entry) {
        return !Constraints.contains(Entry.first);
      });
      return true;
    }
    auto Found = Catalog.Packages.find(Missing);
    if (Found == Catalog.Packages.end()) {
      Pending = PackageMetadataRequest{Missing, {}};
      return true;
    }
    auto Candidates = Found->second;
    std::ranges::sort(Candidates, [](const auto &Left, const auto &Right) {
      auto Compared =
          compareVersions(Left.Package.Selected, Right.Package.Selected);
      return Compared
                 ? Compared > 0
                 : Left.Package.Selected.Text > Right.Package.Selected.Text;
    });
    const auto Lock = Catalog.LockedVersions.find(Missing);
    for (const auto &Candidate : Candidates) {
      if (Lock != Catalog.LockedVersions.end() &&
          !Catalog.Unlocked.contains(Missing) &&
          Candidate.Package.Selected.Text != Lock->second)
        continue;
      if (!std::ranges::all_of(Constraints.at(Missing),
                               [&](const auto &Requirement) {
                                 return Requirement.Requirement.matches(
                                     Candidate.Package.Selected);
                               }))
        continue;
      if (!Candidate.MetadataAvailable) {
        Pending = PackageMetadataRequest{Missing, Candidate.Package};
        return true;
      }
      Selected.Packages[Missing] = Candidate;
      if (Self(Self))
        return true;
      Selected.Packages.erase(Missing);
      if (LimitReached)
        return false;
    }
    if (Conflict.Notes.empty())
      for (const auto &Requirement : Constraints.at(Missing))
        Conflict.Notes.push_back(Requirement.Chain);
    return false;
  };
  if (Solve(Solve)) {
    if (Pending)
      return PackageResolutionStep{std::move(*Pending)};
    return PackageResolutionStep{std::move(Selected)};
  }
  if (LimitReached)
    return std::unexpected(
        Diagnostic{"Package resolution exceeds its search limit"});
  return std::unexpected(std::move(Conflict));
}

Result<PackageResolution>
resolvePackages(const std::vector<PackageRequirement> &Requirements,
                const PackageCatalog &Catalog) {
  auto Step = resolvePackageStep(Requirements, Catalog);
  if (!Step)
    return std::unexpected(Step.error());
  if (auto Resolution = std::get_if<PackageResolution>(&*Step))
    return std::move(*Resolution);
  return std::unexpected(
      Diagnostic{"No package metadata for " +
                 std::get<PackageMetadataRequest>(*Step).Identity});
}

}
