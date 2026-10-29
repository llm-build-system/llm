module;
#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
#include <zlib.h>
module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
namespace {
void decompressGzip(Context &Call, const Path &Source, const Path &Destination,
                    std::uint64_t Limit) {
  std::ifstream Input(nativePath(Source), std::ios::binary);
  std::ofstream Output(nativePath(Destination),
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!Input || !Output)
    throw Failure("Cannot open gzip input/output");
  z_stream Stream{};
  if (inflateInit2(&Stream, 31) != Z_OK)
    throw Failure("Cannot initialize gzip decoder");
  struct End {
    z_stream &Stream;
    ~End() { inflateEnd(&Stream); }
  } Guard{Stream};
  std::array<unsigned char, 65536> Compressed{}, Plain{};
  std::uint64_t Total = 0;
  for (;;) {
    cancelled(Call);
    if (!Stream.avail_in) {
      Input.read(reinterpret_cast<char *>(Compressed.data()),
                 Compressed.size());
      auto Count = Input.gcount();
      if (!Count)
        throw Failure("Truncated gzip stream");
      Stream.next_in = Compressed.data();
      Stream.avail_in = static_cast<uInt>(Count);
    }
    Stream.next_out = Plain.data();
    Stream.avail_out = Plain.size();
    auto Status = inflate(&Stream, Z_NO_FLUSH);
    if (Status != Z_OK && Status != Z_STREAM_END)
      throw Failure("Invalid gzip stream or CRC");
    auto Count = Plain.size() - Stream.avail_out;
    if (Count > Limit - Total)
      throw Failure("Gzip expansion exceeds package limits");
    Total += Count;
    Output.write(reinterpret_cast<const char *>(Plain.data()), Count);
    if (!Output)
      throw Failure("Cannot write decompressed gzip data");
    if (Status == Z_STREAM_END) {
      if (Stream.avail_in || Input.peek() != std::char_traits<char>::eof())
        throw Failure("Unexpected data after gzip stream");
      break;
    }
  }
  Output.close();
  if (!Output)
    throw Failure("Cannot finish decompressed gzip data");
}

void compressGzip(Context &Call, const Path &Source, const Path &Destination) {
  auto Temporary = Destination.parent_path() / (".gzip-" + uniqueId());
  struct Cleanup {
    Path File;
    ~Cleanup() {
      std::error_code Error;
      fs::remove(File, Error);
    }
  } Remove{Temporary};
  std::ifstream Input(nativePath(Source), std::ios::binary);
  std::ofstream Output(nativePath(Temporary),
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!Input || !Output)
    throw Failure("Cannot open gzip input/output");
  z_stream Stream{};
  if (deflateInit2(&Stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK)
    throw Failure("Cannot initialize gzip encoder");
  struct End {
    z_stream &Stream;
    ~End() { deflateEnd(&Stream); }
  } Guard{Stream};
  gz_header Header{};
  Header.time = 0;
  Header.os = 255;
  if (deflateSetHeader(&Stream, &Header) != Z_OK)
    throw Failure("Cannot configure deterministic gzip header");
  std::array<unsigned char, 65536> Plain{}, Compressed{};
  int Status = Z_OK;
  do {
    cancelled(Call);
    Input.read(reinterpret_cast<char *>(Plain.data()), Plain.size());
    if (Input.bad())
      throw Failure("Cannot read gzip input");
    Stream.next_in = Plain.data();
    Stream.avail_in = static_cast<uInt>(Input.gcount());
    auto Flush = Input.eof() ? Z_FINISH : Z_NO_FLUSH;
    do {
      Stream.next_out = Compressed.data();
      Stream.avail_out = Compressed.size();
      Status = deflate(&Stream, Flush);
      if (Status != Z_OK && Status != Z_STREAM_END)
        throw Failure("Gzip encoding failed");
      Output.write(reinterpret_cast<const char *>(Compressed.data()),
                   Compressed.size() - Stream.avail_out);
      if (!Output)
        throw Failure("Cannot write gzip output");
    } while (Stream.avail_in || Stream.avail_out == 0 ||
             (Flush == Z_FINISH && Status != Z_STREAM_END));
  } while (Status != Z_STREAM_END);
  Output.close();
  if (!Output)
    throw Failure("Cannot finish gzip output");
  replaceFile(Temporary, Destination);
}
}
}
namespace llm::runtime {
namespace fs = std::filesystem;
bool toolchainComponent(const fs::path &Path) {
  auto Text = pathText(Path), Name = pathText(Path.filename());
  if (Name.starts_with("liblldb.") || Name.starts_with("_lldb."))
    return false;
  if (Text == "bin" || Text == "lib" || Text == "include" || Text == "share")
    return true;
  if (Text.starts_with("bin/")) {
    if (Name.ends_with(".exe"))
      Name.resize(Name.size() - 4);
    static const std::set<std::string> Tools{
        "clang",        "clang++",         "clang-23",     "clang-cl",
        "clang-cpp",    "clang-scan-deps", "clang-format", "clang-tidy",
        "lld",          "ld.lld",          "lld-link",     "ld64.lld",
        "wasm-ld",      "llvm-ar",         "llvm-ranlib",  "llvm-readobj",
        "llvm-objdump", "llvm-objcopy",    "llvm-strip",   "llvm-nm",
        "llvm-size",    "llvm-rc",         "llvm-cvtres",  "llvm-lib",
        "llvm-mt"};
    return Tools.contains(Name) || Name.ends_with(".dll");
  }
  if (Text.starts_with("lib/clang/") || Text.starts_with("lib64/clang/"))
    return true;
  if (Text.starts_with("lib/") || Text.starts_with("lib64/"))
    return Name.find(".so") != std::string::npos ||
           Name.find(".dylib") != std::string::npos;
  return Text.find("LICENSE") != std::string::npos ||
         Text.find("license") != std::string::npos;
}
namespace {
struct Reader {
  archive *Value = archive_read_new();
  explicit Reader(bool RawOnly = false) {
    if (!Value)
      throw Failure("Cannot allocate archive reader");
    if (RawOnly)
      archive_read_support_format_raw(Value);
    else {
      archive_read_support_format_tar(Value);
      archive_read_support_format_ar(Value);
      archive_read_support_format_zip(Value);
      archive_read_support_format_cpio(Value);
      if (archive_read_support_format_xar(Value) != ARCHIVE_OK)
        throw Failure("The built-in XAR reader is unavailable");
    }
    for (auto Filter :
         {archive_read_support_filter_gzip, archive_read_support_filter_bzip2,
          archive_read_support_filter_xz, archive_read_support_filter_zstd})
      if (Filter(Value) != ARCHIVE_OK)
        throw Failure("A required built-in archive filter is unavailable");
  }
  ~Reader() {
    if (Value)
      archive_read_free(Value);
  }
  void open(const fs::path &Path) {
    if (
#ifdef _WIN32
        archive_read_open_filename_w(Value, nativePath(Path).c_str(), 65536)
#else
        archive_read_open_filename(Value, Path.c_str(), 65536)
#endif
        != ARCHIVE_OK)
      throw Failure("Cannot open package archive");
  }
};
fs::path safePath(std::string Name, const std::string &Root,
                  bool PosixSdkNames = false, bool HostNames = true) {
  if (Name.empty() || Name.front() == '/' ||
      Name.find("//") != std::string::npos ||
      Name.find('\\') != std::string::npos ||
      (!PosixSdkNames && Name.find(':') != std::string::npos))
    throw Failure("Unsafe archive path: " + Name);
#ifdef _WIN32
  if (HostNames && Name.find(':') != std::string::npos)
    throw Failure("Unsafe archive path: " + Name);
#endif
  while (Name.starts_with("./"))
    Name.erase(0, 2);
  while (Name.ends_with('/'))
    Name.pop_back();
  if (Name == "." || Name.empty())
    return {};
  std::string Normalized;
  for (const auto &Part : pathFromText(Name)) {
    if (Part == ".." || Part == "." || Part.empty())
      throw Failure("Unsafe archive path: " + Name);
#ifdef _WIN32
    if (HostNames) {
      auto Component = pathText(Part);
      if (Component.back() == '.' || Component.back() == ' ' ||
          Component.find_first_of("<>\"|?*") != std::string::npos)
        throw Failure("Unsafe archive path");
      auto Stem = Component.substr(0, Component.find('.'));
      std::transform(
          Stem.begin(), Stem.end(), Stem.begin(),
          [](unsigned char C) { return static_cast<char>(std::toupper(C)); });
      if (Stem == "CON" || Stem == "PRN" || Stem == "AUX" || Stem == "NUL" ||
          (Stem.size() == 4 &&
           (Stem.starts_with("COM") || Stem.starts_with("LPT")) &&
           Stem[3] >= '0' && Stem[3] <= '9'))
        throw Failure("Reserved Windows filename in archive");
    }
#endif
    if (!Normalized.empty())
      Normalized += '/';
    Normalized += pathText(Part);
  }
  auto Result = pathFromText(Normalized);
  if (Result.has_root_name() || Result.is_absolute())
    throw Failure("Absolute archive path: " + Name);
  if (!Root.empty()) {
    auto It = Result.begin();
    if (*It != Root)
      throw Failure("Unexpected package root in archive");
    fs::path Stripped;
    while (++It != Result.end())
      Stripped /= *It;
    return Stripped;
  }
  return Result;
}
bool inside(const fs::path &Path) {
  return !Path.empty() && !Path.has_root_path() && *Path.begin() != "..";
}
fs::path mappedPath(const fs::path &Relative, const SdkPackage &Package) {
  if (Package.Mappings.empty())
    return Relative;
  auto Text = pathText(Relative);
  for (const auto &[Source, Dest] : Package.Mappings) {
    if (Text.starts_with(Source))
      return safePath(Dest + Text.substr(Source.size()), "",
                      Package.Format == "apple-pkg");
    if (Text + '/' == Source)
      return {};
  }
  return {};
}
void unpack(Context &Call, Reader &Reader, const fs::path &Destination,
            const SdkPackage &Package) {
  struct Link {
    fs::path Path, Target;
    bool Hard;
  };
  std::vector<Link> Links;
  std::set<std::string> Paths;
  std::map<fs::path, fs::path> VirtualFiles;
  std::map<fs::path, fs::path> VirtualDirectories;
  std::uint64_t Total = 0;
  size_t Count = 0;
  archive_entry *Entry = nullptr;
  int Status;
  while ((Status = archive_read_next_header(Reader.Value, &Entry)) ==
         ARCHIVE_OK) {
    cancelled(Call);
    if (++Count > 200000)
      throw Failure("Too many archive entries");
    const char *Name = archive_entry_pathname_utf8(Entry);
    if (!Name)
      throw Failure("Invalid archive filename");
    auto Relative = mappedPath(safePath(Name, Package.ArchiveRoot,
                                        Package.Format == "apple-pkg",
                                        Package.Mappings.empty()),
                               Package);
    if (Relative.empty())
      continue;
    auto Key = pathText(Relative);
    if (!Paths.insert(Key).second)
      throw Failure("Duplicate path in archive");
    if (Package.ToolchainOnly && !toolchainComponent(Relative)) {
      archive_read_data_skip(Reader.Value);
      continue;
    }
    auto Target = nativePath(Destination / Relative);
    rejectSymlinkAncestors(Target.parent_path());
    const char *Hard = archive_entry_hardlink(Entry);
    const char *Soft = archive_entry_symlink(Entry);
    if (Package.Format == "zip" && (Hard || Soft))
      throw Failure("ZIP links and special files are forbidden");
    if (Hard || Soft) {
      fs::path Link = pathFromText(Hard ? Hard : Soft);
      if (Hard)
        Link = mappedPath(
            safePath(Hard, Package.ArchiveRoot, Package.Format == "apple-pkg"),
            Package);
      else if (Link.has_root_name())
        throw Failure("Archive link contains a drive or network root");
      else if (Link.has_root_directory()) {
        if (!(Package.Format == "deb"))
          throw Failure("Absolute archive links are forbidden");
        Link = safePath(Link.relative_path().generic_string(), "");
      } else
        Link = (Relative.parent_path() / Link).lexically_normal();
      if (!inside(Link))
        throw Failure("Archive link escapes destination");
      Links.push_back({Relative, Link, Hard != nullptr});
      continue;
    }
    if (archive_entry_filetype(Entry) == AE_IFDIR) {
      rejectSymlinkAncestors(Target);
      fs::create_directories(Target);
      continue;
    }
    if (archive_entry_filetype(Entry) != AE_IFREG)
      throw Failure("Archive special files are forbidden");
    auto Size = archive_entry_size(Entry);
    if (Size < 0 ||
        static_cast<std::uint64_t>(Size) > Package.MaxExpanded - Total)
      throw Failure("Archive expansion exceeds package limits");
    if (Package.Format == "zip" && Size > 512LL * 1024 * 1024)
      throw Failure("ZIP entry exceeds package limits");
    Total += Size;
    fs::create_directories(Target.parent_path());
    if (fs::exists(Target)) {
      auto Fold = [](std::string Value) {
        std::ranges::transform(Value, Value.begin(), [](unsigned char Byte) {
          return static_cast<char>(std::tolower(Byte));
        });
        return Value;
      };
      auto Name = pathText(Target.filename());
      for (const auto &Existing :
           fs::directory_iterator(Target.parent_path())) {
        auto Actual = pathText(Existing.path().filename());
        if (Actual != Name && Fold(Actual) == Fold(Name) &&
            fs::is_regular_file(Existing.symlink_status())) {
          auto Physical = Relative.parent_path() /
                          pathFromText(".case-" + sha256Text(Key) +
                                       pathText(Relative.extension()));
          VirtualFiles.emplace(Existing.path().lexically_relative(Destination),
                               Existing.path().lexically_relative(Destination));
          VirtualFiles.emplace(Relative, Physical);
          Target = nativePath(Destination / Physical);
          break;
        }
      }
    }
    std::ofstream Output(nativePath(Target), std::ios::binary | std::ios::out |
                                                 std::ios::noreplace);
    if (!Output)
      throw Failure("Cannot create extracted file: " + pathText(Target));
    std::array<char, 65536> Buffer{};
    std::uint64_t Written = 0;
    for (;;) {
      cancelled(Call);
      auto Bytes =
          archive_read_data(Reader.Value, Buffer.data(), Buffer.size());
      if (Bytes < 0)
        throw Failure("Archive extraction or CRC verification failed");
      if (!Bytes)
        break;
      Written += Bytes;
      if (Written > static_cast<std::uint64_t>(Size))
        throw Failure("Archive entry exceeds declared size");
      Output.write(Buffer.data(), Bytes);
      if (!Output)
        throw Failure("Cannot write extracted file");
    }
    Output.close();
    if (!Output)
      throw Failure("Cannot finish extracted file");
    if (Written != static_cast<std::uint64_t>(Size))
      throw Failure("Truncated archive entry");
#ifndef _WIN32
    fs::permissions(Target, static_cast<fs::perms>(
                                (archive_entry_perm(Entry) & 0111) | 0644));
#endif
  }
  if (Status != ARCHIVE_EOF || Count == 0)
    throw Failure("Invalid package archive: " +
                  std::string(archive_error_string(Reader.Value)
                                  ? archive_error_string(Reader.Value)
                                  : "empty"));
#ifdef _WIN32
  std::vector<unsigned> State(Links.size());
  std::map<std::string, std::size_t> LinkIndices;
  for (std::size_t Index = 0; Index < Links.size(); ++Index)
    LinkIndices.emplace(pathText(Links[Index].Path), Index);
  for (std::size_t Index = 0; Index < Links.size(); ++Index) {
    const auto &Link = Links[Index];
    auto Relative = Link.Path.lexically_relative(Link.Target);
    if (!Link.Hard && Link.Path != Link.Target && !Relative.empty() &&
        inside(Relative) &&
        fs::is_directory(nativePath(Destination / Link.Target))) {
      VirtualDirectories.emplace(Link.Path, Link.Target);
      State[Index] = 2;
    }
  }
  auto Materialize = [&](auto &&Self, std::size_t Index) -> void {
    cancelled(Call);
    if (State[Index] == 2)
      return;
    if (State[Index] == 1)
      throw Failure("Archive link cycle: " + pathText(Links[Index].Path) +
                    " -> " + pathText(Links[Index].Target));
    State[Index] = 1;
    const auto &Link = Links[Index];
    for (auto Parent = Link.Target; !Parent.empty();
         Parent = Parent.parent_path())
      if (auto Found = LinkIndices.find(pathText(Parent));
          Found != LinkIndices.end())
        Self(Self, Found->second);
    auto Real = Link.Target;
    if (auto File = VirtualFiles.find(Real); File != VirtualFiles.end())
      Real = File->second;
    auto Source = nativePath(Destination / Real);
    rejectSymlinkAncestors(Source);
    if (!fs::exists(Source)) {
      VirtualFiles.emplace(Link.Path, Real);
      State[Index] = 2;
      return;
    }
    if (fs::is_directory(Source)) {
      auto Prefix = pathText(Link.Target) + '/';
      for (auto Entry = LinkIndices.lower_bound(Prefix);
           Entry != LinkIndices.end() && Entry->first.starts_with(Prefix);
           ++Entry)
        Self(Self, Entry->second);
    }
    auto Target = nativePath(Destination / Link.Path);
    rejectSymlinkAncestors(Target.parent_path());
    fs::create_directories(Target.parent_path());
    if (fs::exists(fs::symlink_status(Target)))
      throw Failure("Duplicate installed link");
    if (fs::is_directory(Source)) {
      fs::create_directory(Target);
      for (const auto &Entry : fs::recursive_directory_iterator(Source)) {
        cancelled(Call);
        auto Output =
            nativePath(Target / Entry.path().lexically_relative(Source));
        if (++Count > 200000)
          throw Failure("Archive aliases exceed file limit");
        if (Entry.is_directory())
          fs::create_directory(Output);
        else if (Entry.is_regular_file()) {
          auto Size = Entry.file_size();
          if (Size > Package.MaxExpanded - Total)
            throw Failure("Archive aliases exceed expansion limit");
          Total += Size;
          fs::copy_file(Entry.path(), Output);
        } else
          throw Failure("Unsafe archive alias type");
      }
      std::vector<std::pair<fs::path, fs::path>> Projected;
      for (const auto &[Alias, Real] : VirtualDirectories) {
        auto Relative = Alias.lexically_relative(Link.Target);
        if (!Relative.empty() && inside(Relative))
          Projected.emplace_back(Link.Path / Relative, Real);
      }
      VirtualDirectories.insert(Projected.begin(), Projected.end());
    } else {
      auto Size = fs::file_size(Source);
      if (Size > Package.MaxExpanded - Total)
        throw Failure("Archive aliases exceed expansion limit");
      Total += Size;
      fs::create_hard_link(Source, Target);
    }
    State[Index] = 2;
  };
  for (std::size_t Index = 0; Index < Links.size(); ++Index)
    Materialize(Materialize, Index);

#else
  for (const auto &Link : Links) {
    auto Target = Destination / Link.Path;
    rejectSymlinkAncestors(Target.parent_path());
    fs::create_directories(Target.parent_path());
    if (fs::exists(fs::symlink_status(Target)))
      throw Failure("Duplicate installed link");
    if (Link.Hard) {
      rejectSymlinkAncestors(Destination / Link.Target);
      fs::create_hard_link(Destination / Link.Target, Target);
    } else {
      fs::create_symlink(
          Link.Target.lexically_relative(Link.Path.parent_path()), Target);
#ifdef __APPLE__
      fs::permissions(Target, static_cast<fs::perms>(0755),
                      fs::perm_options::replace | fs::perm_options::nofollow);
#endif
    }
  }
#endif
  if (!VirtualDirectories.empty() || !VirtualFiles.empty()) {
    auto File = Destination / "paths.yaml";
    nlohmann::json Root{{"type", "directory"},
                        {"name", "."},
                        {"contents", nlohmann::json::array()}};
    if (fs::exists(File)) {
      auto Text = readText(File);
      if (Text.size() > 32 * 1024 * 1024)
        throw Failure("SDK path mapping exceeds limit");
      auto Previous = nlohmann::json::parse(Text);
      if (Previous.value("version", -1) != 0 ||
          Previous.value("root-relative", "") != "overlay-dir" ||
          !Previous.value("overlay-relative", false) ||
          !Previous.contains("roots") || Previous["roots"].size() != 1 ||
          Previous["roots"][0].value("name", "") != ".")
        throw Failure("SDK path mapping conflicts with an archive file");
      Root = Previous["roots"][0];
    }
    auto Add = [&](const fs::path &Alias, const fs::path &Real,
                   bool Directory) {
      auto *Entry = &Root;
      for (auto Part = Alias.begin(); Part != Alias.end(); ++Part) {
        if (!Entry->contains("contents"))
          throw Failure("Conflicting SDK path mappings");
        auto &Contents = (*Entry)["contents"];
        auto Name = pathText(*Part);
        auto Found = std::ranges::find_if(Contents, [&](const auto &Item) {
          return Item.at("name") == Name;
        });
        if (Found == Contents.end()) {
          Contents.push_back({{"type", "directory"},
                              {"name", Name},
                              {"contents", nlohmann::json::array()}});
          Found = std::prev(Contents.end());
        }
        Entry = &*Found;
      }
      (*Entry)["type"] = Directory ? "directory-remap" : "file";
      (*Entry)["external-contents"] = pathText(Real);
      Entry->erase("contents");
    };
    for (const auto &[Alias, Real] : VirtualDirectories)
      Add(Alias, Real, true);
    for (const auto &[Alias, Real] : VirtualFiles)
      Add(Alias, Real, false);
    nlohmann::json Overlay{{"version", 0},
                           {"case-sensitive", true},
                           {"use-external-names", true},
                           {"root-relative", "overlay-dir"},
                           {"overlay-relative", true},
                           {"roots", nlohmann::json::array({Root})}};
    writeText(File, Overlay.dump(2) + '\n');
  }
}

void unpackApple(Context &Call, const fs::path &File,
                 const fs::path &Destination, const SdkPackage &Package) {
  auto Temporary = Destination.parent_path() / (".apple-" + uniqueId());
  fs::create_directory(Temporary);
  struct Cleanup {
    fs::path P;
    ~Cleanup() {
      std::error_code E;
      fs::remove_all(P, E);
    }
  } Cleanup{Temporary};
  Reader Xar;
  Xar.open(File);
  archive_entry *Entry = nullptr;
  bool Found = false;
  auto Payload = Temporary / "Payload";
  while (archive_read_next_header(Xar.Value, &Entry) == ARCHIVE_OK) {
    if (std::string_view(archive_entry_pathname(Entry)) != "Payload")
      continue;
    if (Found || archive_entry_size(Entry) < 0 ||
        static_cast<uint64_t>(archive_entry_size(Entry)) > Package.MaxDownload)
      throw Failure("Invalid Apple SDK payload");
    Found = true;
    std::ofstream Out(nativePath(Payload), std::ios::binary);
    std::array<char, 65536> Buffer{};
    uint64_t Total = 0;
    for (;;) {
      auto N = archive_read_data(Xar.Value, Buffer.data(), Buffer.size());
      if (N < 0)
        throw Failure("Invalid Apple SDK payload");
      if (!N)
        break;
      Total += N;
      if (Total > Package.MaxDownload)
        throw Failure("Apple payload exceeds limit");
      Out.write(Buffer.data(), N);
      cancelled(Call);
    }
    if (!Out)
      throw Failure("Cannot write Apple SDK payload");
  }
  if (!Found)
    throw Failure("Apple SDK has no Payload");
  std::ifstream Input(nativePath(Payload), std::ios::binary);
  char Magic[4];
  Input.read(Magic, 4);
  auto Cpio = Temporary / "sdk.cpio";
  std::ofstream Output(nativePath(Cpio), std::ios::binary);
  uint64_t Expanded = 0;
  auto Number = [&] {
    uint64_t N = 0;
    for (int I = 0; I < 8; ++I) {
      int C = Input.get();
      if (C < 0)
        throw Failure("Truncated PBZX header");
      N = (N << 8) | static_cast<unsigned char>(C);
    }
    return N;
  };
  if (std::string_view(Magic, 4) != "pbzx")
    throw Failure("Unsupported Apple SDK Payload format");
  uint64_t Flags = Number();
  while (Flags & (1ULL << 24)) {
    Flags = Number();
    auto Length = Number();
    if (Length > Package.MaxDownload)
      throw Failure("PBZX chunk exceeds limit");
    std::vector<char> Bytes(static_cast<size_t>(Length));
    Input.read(Bytes.data(), static_cast<std::streamsize>(Length));
    if (!Input)
      throw Failure("Truncated PBZX chunk");
    if (Length >= 6 &&
        std::string_view(Bytes.data(), 6) == std::string_view("\xfd"
                                                              "7zXZ\0",
                                                              6)) {
      auto Chunk = Temporary / "chunk.xz";
      writeText(Chunk, std::string_view(Bytes.data(), Bytes.size()));
      Reader Decoder(true);
      Decoder.open(Chunk);
      archive_entry *Raw = nullptr;
      if (archive_read_next_header(Decoder.Value, &Raw) != ARCHIVE_OK)
        throw Failure("Invalid PBZX compressed chunk");
      std::array<char, 65536> Buffer{};
      for (;;) {
        auto N = archive_read_data(Decoder.Value, Buffer.data(), Buffer.size());
        if (N < 0)
          throw Failure("Invalid PBZX compressed data");
        if (!N)
          break;
        Expanded += N;
        if (Expanded > Package.MaxExpanded)
          throw Failure("Apple SDK expansion exceeds limit");
        Output.write(Buffer.data(), N);
        cancelled(Call);
      }
    } else {
      Expanded += Length;
      if (Expanded > Package.MaxExpanded)
        throw Failure("Apple SDK expansion exceeds limit");
      Output.write(Bytes.data(), Bytes.size());
    }
    if (!Output)
      throw Failure("Cannot decode Apple SDK");
  }
  Output.close();
  Reader Reader;
  Reader.open(Cpio);
  unpack(Call, Reader, Destination, Package);
}
}
void extractPackage(Context &Call, const fs::path &File,
                    const fs::path &OutputRoot, const SdkPackage &Package) {
  auto Destination = nativePath(OutputRoot);
  rejectSymlinkAncestors(Destination);
  fs::create_directories(Destination);
  if (Package.Format == "tar.gz") {
    auto Payload = Destination.parent_path() / (".tar-" + uniqueId());
    struct Remove {
      fs::path Path;
      ~Remove() {
        std::error_code Error;
        fs::remove(Path, Error);
      }
    } Remove{Payload};
    decompressGzip(Call, File, Payload, Package.MaxExpanded);
    auto Tar = Package;
    Tar.Format = "tar";
    extractPackage(Call, Payload, Destination, Tar);
    return;
  }
  if (Package.Format == "apple-pkg") {
    unpackApple(Call, File, Destination, Package);
  } else {
    Reader InputArchive;
    InputArchive.open(File);
    if ((Package.Format == "deb")) {
      archive_entry *Entry = nullptr;
      bool Found = false;
      auto Payload = Destination.parent_path() / (".deb-" + uniqueId());
      struct Cleanup {
        fs::path Path;
        ~Cleanup() {
          std::error_code Error;
          fs::remove(Path, Error);
        }
      } Cleanup{Payload};
      while (archive_read_next_header(InputArchive.Value, &Entry) ==
             ARCHIVE_OK) {
        std::string Name(archive_entry_pathname(Entry));
        if (!Name.starts_with("data.tar."))
          continue;
        if (Found || archive_entry_size(Entry) < 0 ||
            static_cast<std::uint64_t>(archive_entry_size(Entry)) >
                Package.MaxDownload)
          throw Failure("Invalid Debian package payload");
        std::ofstream Output(nativePath(Payload), std::ios::binary);
        std::array<char, 65536> Buffer{};
        std::uint64_t Total = 0;
        for (;;) {
          cancelled(Call);
          auto N = archive_read_data(InputArchive.Value, Buffer.data(),
                                     Buffer.size());
          if (N < 0)
            throw Failure("Invalid Debian payload");
          if (!N)
            break;
          Total += N;
          if (Total > Package.MaxDownload)
            throw Failure("Debian payload exceeds limit");
          Output.write(Buffer.data(), N);
          if (!Output)
            throw Failure("Cannot extract Debian payload");
        }
        Output.close();
        if (!Output)
          throw Failure("Cannot finish Debian payload");
        Reader Data;
        Data.open(Payload);
        unpack(Call, Data, Destination, Package);
        Found = true;
      }
      if (!Found)
        throw Failure("Debian package has no data archive");
    } else
      unpack(Call, InputArchive, Destination, Package);
  }
  for (const auto &Required : Package.RequiredFiles) {
    auto File = nativePath(Destination / Required);
    if (!fs::is_regular_file(File) || !fs::file_size(File))
      throw Failure("Package is missing " + pathText(Required));
  }
}
}

namespace llm::runtime {
void createTarball(Context &Call, const fs::path &Root,
                   const fs::path &Destination,
                   const std::map<fs::path, fs::path> &Symlinks) {
  rejectSymlinkAncestors(Root);
  rejectSymlinkAncestors(Destination);
  auto Temporary = Destination.parent_path() / (".tar-" + uniqueId());
  struct Cleanup {
    fs::path Path;
    ~Cleanup() {
      std::error_code Error;
      fs::remove(Path, Error);
    }
  } Cleanup{Temporary};
  struct Writer {
    archive *Value = archive_write_new();
    ~Writer() {
      if (Value)
        archive_write_free(Value);
    }
  } Writer;
  if (!Writer.Value ||
      archive_write_set_format_pax_restricted(Writer.Value) != ARCHIVE_OK)
    throw Failure("Cannot initialize tar writer");
  if (
#ifdef _WIN32
      archive_write_open_filename_w(Writer.Value, nativePath(Temporary).c_str())
#else
      archive_write_open_filename(Writer.Value, Temporary.c_str())
#endif
      != ARCHIVE_OK)
    throw Failure("Cannot create tar archive");
  std::vector<fs::path> Paths;
  for (const auto &Entry : fs::recursive_directory_iterator(Root))
    Paths.push_back(Entry.path());
  for (const auto &[Path, Target] : Symlinks) {
    if (!isRelativeSafe(Path) || Target.is_absolute())
      throw Failure("Unsafe virtual package symlink");
    if (fs::exists(fs::symlink_status(Root / Path)))
      throw Failure("Package symlink collides with another entry");
    Paths.push_back(Root / Path);
  }
  std::ranges::sort(Paths, [&](const auto &Left, const auto &Right) {
    return pathText(Left.lexically_relative(Root)) <
           pathText(Right.lexically_relative(Root));
  });
  std::array<char, 65536> Buffer{};
  std::set<std::string> Names;
  for (const auto &Path : Paths) {
    cancelled(Call);
    auto Relative = Path.lexically_relative(Root);
    auto Status = fs::symlink_status(Path);
    auto Key = collisionKey(Relative);
    if (!Names.insert(Key).second)
      throw Failure("Package archive paths collide: " + pathText(Relative));
    auto VirtualLink = Symlinks.find(Relative);
    struct Entry {
      archive_entry *Value = archive_entry_new();
      ~Entry() { archive_entry_free(Value); }
    } Entry;
    if (!Entry.Value)
      throw Failure("Cannot allocate tar entry");
    auto Name = pathText(Relative);
    if (!isRelativeSafe(Relative) ||
        Name.find_first_of("\\:\r\n\0", 0, 5) != std::string::npos)
      throw Failure("Unsupported package archive path: " + Name);
    archive_entry_set_pathname_utf8(Entry.Value, Name.c_str());
    archive_entry_set_mtime(Entry.Value, 0, 0);
    archive_entry_set_uid(Entry.Value, 0);
    archive_entry_set_gid(Entry.Value, 0);
    if (VirtualLink != Symlinks.end() || fs::is_symlink(Status)) {
      auto Target = VirtualLink != Symlinks.end() ? VirtualLink->second
                                                  : fs::read_symlink(Path);
      auto Resolved = (Relative.parent_path() / Target).lexically_normal();
      if (Target.is_absolute() ||
          (Resolved != "." && !isRelativeSafe(Resolved)))
        throw Failure("Package symlink escapes its root: " + Name);
      auto Text = pathText(Target);
      archive_entry_set_filetype(Entry.Value, AE_IFLNK);
      archive_entry_set_perm(Entry.Value, 0777);
      archive_entry_set_symlink_utf8(Entry.Value, Text.c_str());
      archive_entry_set_size(Entry.Value, 0);
    } else if (fs::is_directory(Status)) {
      archive_entry_set_filetype(Entry.Value, AE_IFDIR);
      archive_entry_set_perm(Entry.Value, 0755);
      archive_entry_set_size(Entry.Value, 0);
    } else if (fs::is_regular_file(Status)) {
      archive_entry_set_filetype(Entry.Value, AE_IFREG);
      auto Executable = (Status.permissions() &
                         (fs::perms::owner_exec | fs::perms::group_exec |
                          fs::perms::others_exec)) != fs::perms::none;
      archive_entry_set_perm(Entry.Value, Executable ? 0755 : 0644);
      archive_entry_set_size(Entry.Value, fs::file_size(Path));
    } else
      throw Failure("Unsupported package file: " + Name);
    if (archive_write_header(Writer.Value, Entry.Value) != ARCHIVE_OK)
      throw Failure("Cannot write tar header: " + Name);
    if (VirtualLink == Symlinks.end() && fs::is_regular_file(Status)) {
      std::ifstream Input(nativePath(Path), std::ios::binary);
      if (!Input)
        throw Failure("Cannot read package file: " + Name);
      while (Input) {
        cancelled(Call);
        Input.read(Buffer.data(), Buffer.size());
        auto Count = Input.gcount();
        if (Count &&
            archive_write_data(Writer.Value, Buffer.data(),
                               static_cast<std::size_t>(Count)) != Count)
          throw Failure("Cannot write package file: " + Name);
      }
      if (!Input.eof())
        throw Failure("Cannot finish reading package file: " + Name);
    }
  }
  if (archive_write_close(Writer.Value) != ARCHIVE_OK)
    throw Failure("Cannot finish tar archive");
  compressGzip(Call, Temporary, Destination);
}
}

namespace llm::runtime {
namespace {
bool below(const fs::path &Root, const fs::path &Path) {
  auto Relative = Path.lexically_relative(Root);
  return Relative == "." || isRelativeSafe(Relative);
}
toml::array array(const Strings &Values) {
  toml::array Result;
  for (const auto &Value : Values)
    Result.push_back(Value);
  return Result;
}
void document(const fs::path &File, const toml::table &Table) {
  std::ostringstream Text;
  Text << Table;
  writeText(File, Text.str() + "\n");
}
using Links = std::map<fs::path, fs::path>;
void copyEntry(const fs::path &Source, const fs::path &Destination,
               const fs::path &Staging, Links &Links) {
  if (!below(Staging, Destination))
    throw Failure("Package output escapes staging directory");
  rejectSymlinkAncestors(Source.parent_path());
  rejectSymlinkAncestors(Destination);
  auto Relative = Destination.lexically_relative(Staging / "payload");
  if (!isRelativeSafe(Relative))
    throw Failure("Package entry escapes payload");
  auto SourceStatus = fs::symlink_status(Source);
  if (auto Existing = Links.find(Relative); Existing != Links.end()) {
    if (fs::is_symlink(SourceStatus) &&
        fs::read_symlink(Source) == Existing->second)
      return;
    throw Failure("Package entries collide: " + pathText(Relative));
  }
  for (auto Ancestor = Relative; !Ancestor.empty();
       Ancestor = Ancestor.parent_path())
    if (Links.contains(Ancestor))
      throw Failure("Package entry conflicts with a declared symlink: " +
                    pathText(Relative));
  if (fs::exists(fs::symlink_status(Destination))) {
    auto Actual = fs::symlink_status(Destination);
    constexpr auto Execute =
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    if (fs::is_regular_file(SourceStatus) && fs::is_regular_file(Actual) &&
        ((SourceStatus.permissions() & Execute) != fs::perms::none) ==
            ((Actual.permissions() & Execute) != fs::perms::none) &&
        sha256File(Source) == sha256File(Destination))
      return;
    throw Failure("Package entries collide: " +
                  pathText(Destination.lexically_relative(Staging)));
  }
  fs::create_directories(Destination.parent_path());
  if (fs::is_symlink(fs::symlink_status(Source))) {
    auto Target = fs::read_symlink(Source);
    if (Target.is_absolute() ||
        !below(Staging / "payload",
               (Destination.parent_path() / Target).lexically_normal()))
      throw Failure("Package symlink escapes its payload: " +
                    pathText(Relative));
    Links.emplace(Relative, Target);
    return;
  }
  if (!fs::is_regular_file(Source))
    throw Failure("Package input is missing: " + pathText(Source));
  fs::copy_file(Source, Destination);
  fs::permissions(Destination, fs::status(Source).permissions());
}
void resources(const Manifest &Manifest, const EffectiveSettings &Settings,
               const fs::path &Destination, const fs::path &Staging,
               Links &Links) {
  std::set<fs::path> Files;
  for (const auto &Pattern : Settings.Resources) {
    auto Relative = pathFromText(Pattern);
    if (!isRelativeSafe(Relative))
      throw Failure("Resource must stay inside its project: " + Pattern);
    auto Exact = Manifest.Root / Relative;
    if (fs::is_symlink(fs::symlink_status(Exact))) {
      Files.insert(Relative);
      continue;
    }
    if (fs::is_regular_file(Exact)) {
      Files.insert(Relative);
      continue;
    }
    bool Matched = false;
    if (fs::is_directory(Exact)) {
      rejectSymlinkAncestors(Exact);
      for (const auto &Entry : fs::recursive_directory_iterator(Exact))
        if (Entry.is_regular_file() || fs::is_symlink(Entry.symlink_status())) {
          Files.insert(Entry.path().lexically_relative(Manifest.Root));
          Matched = true;
        }
      if (!Matched)
        fs::create_directories(Destination / Relative);
      continue;
    }
    for (auto It = fs::recursive_directory_iterator(Manifest.Root);
         It != fs::recursive_directory_iterator(); ++It) {
      auto Path = It->path().lexically_relative(Manifest.Root);
      auto First = *Path.begin();
      if (First == "obj" || First == "bin" || First == ".git") {
        It.disable_recursion_pending();
        continue;
      }
      if (fs::is_symlink(It->symlink_status())) {
        It.disable_recursion_pending();
        if (matchesGlob(Pattern, pathText(Path))) {
          Files.insert(Path);
          Matched = true;
        }
        continue;
      }
      if (It->is_regular_file() && matchesGlob(Pattern, pathText(Path))) {
        Files.insert(Path);
        Matched = true;
      }
    }
    if (!Matched)
      throw Failure("Resource pattern matches no file: " + Pattern);
  }
  for (const auto &Relative : Files)
    copyEntry(Manifest.Root / Relative, Destination / Relative, Staging, Links);
}
void validateLinks(const fs::path &Root, const Links &Links) {
  std::function<fs::path(fs::path, unsigned)> Resolve = [&](fs::path Relative,
                                                            unsigned Depth) {
    if (Depth > 64)
      throw Failure("Package symlink cycle");
    Relative = Relative.lexically_normal();
    if (Relative != "." && !isRelativeSafe(Relative))
      throw Failure("Package symlink escapes payload");
    fs::path Prefix;
    for (auto Component = Relative.begin(); Component != Relative.end();
         ++Component) {
      Prefix /= *Component;
      if (auto Found = Links.find(Prefix); Found != Links.end()) {
        auto Next = (Prefix.parent_path() / Found->second);
        for (auto Rest = std::next(Component); Rest != Relative.end(); ++Rest)
          Next /= *Rest;
        return Resolve(Next, Depth + 1);
      }
    }
    return Root / Relative;
  };
  for (const auto &[Path, Target] : Links)
    if (!fs::exists(Resolve(Path, 0)))
      throw Failure("Package symlink target was not included: " +
                    pathText(Path));
}
}
void exportPackage(Context &Call, const ProjectGraph &Graph,
                   const Command &Options, const fs::path &Toolchain,
                   const BuildResult &Built) {
  const auto &RootPath = Graph.Projects.at(Graph.Root).Input.Project.Root;
  auto BuildPlan =
      take(planBuild(Graph, {Options.BuildTarget, Options.Mode, false}));
  const auto &CompileCommands = Built.CompileCommands;
  std::map<std::pair<std::string, Path>, Path> ObjectFiles;
  for (const auto &Action : BuildPlan.Actions)
    if (Action.Kind == ActionKind::Compile)
      ObjectFiles.emplace(
          std::pair{Action.Project,
                    std::get<CompileUnit>(Action.Details).Source},
          Action.Outputs.front());
  auto Sdk = Toolchain.empty() ? fs::path{} : fs::weakly_canonical(Toolchain);
  auto SdkPath = [&](const fs::path &Path) {
    return !Sdk.empty() && below(Sdk, fs::weakly_canonical(Path));
  };
  const auto &Root = Graph.Projects.at(Graph.Root);
  auto Relative = Path(std::string(targetName(Options.BuildTarget))) /
                  (Options.Mode == Profile::Release ? "release" : "debug");
  auto Directory = RootPath / "bin" / Relative;
  rejectSymlinkAncestors(Directory);
  fs::create_directories(Directory);
  auto Work = RootPath / "obj" / Relative;
  rejectSymlinkAncestors(Work);
  fs::create_directories(Work);
  auto Staging = Work / (".package-" + uniqueId());
  fs::create_directory(Staging);
  struct Cleanup {
    fs::path Path;
    ~Cleanup() {
      std::error_code Error;
      fs::remove_all(Path, Error);
    }
  } Cleanup{Staging};
  auto Payload = Staging / "payload";
  fs::create_directory(Payload);
  Links Links;
  auto Copy = [&](const fs::path &Source, const fs::path &Destination,
                  const fs::path &Stage) {
    copyEntry(Source, Destination, Stage, Links);
  };
  auto Binaries = RootPath / "bin" /
                  std::string(targetName(Options.BuildTarget)) /
                  ((Options.Mode == Profile::Release) ? "release" : "debug");
  auto Output =
      Directory /
      pathFromText(Root.Input.Project.Name + "-" +
                   std::string(targetName(Options.BuildTarget)) + ".tar.gz");
  auto RelativeOutput = Output.lexically_relative(RootPath);
  checkOutput(RootPath, RelativeOutput);
  auto Owned = readOwned(RootPath);
  if (fs::exists(Output) && !Owned.contains(RelativeOutput))
    throw Failure("Refusing to overwrite a foreign package archive");
  Owned.insert(RelativeOutput);
  saveOwned(RootPath, Owned);
  if (Root.Input.Project.Type == ProductType::Executable ||
      Root.Input.Project.Type == ProductType::Test) {
    auto Program = RootPath / BuildPlan.Artifacts.at(Graph.Root);
    Copy(Program, Payload / Program.filename(), Staging);
    std::map<std::string, fs::path> Runtime;
    for (const auto &Id : Root.Runtimes) {
      auto Path = RootPath / BuildPlan.Artifacts.at(Id);
      auto Name = pathText(Path.filename());
      auto [Previous, Inserted] = Runtime.emplace(Name, Path);
      if (!Inserted && sha256File(Previous->second) != sha256File(Path))
        throw Failure("Package runtime filename collision: " + Name);
    }
    for (const auto &[Name, Path] : Runtime)
      Copy(Path, Payload / pathFromText(Name), Staging);
    resources(Root.Input.Project, Root.Settings, Payload, Staging, Links);
  } else {
    std::map<std::string, fs::path> Destinations;
    for (const auto &[Id, Node] : Graph.Projects)
      Destinations[Id] = Id == Graph.Root ? Payload
                                          : Payload / "dependencies" /
                                                sha256Text(Id).substr(0, 20);
    for (const auto &[Id, Node] : Graph.Projects) {
      if (!isLibrary(Node.Input.Project.Type))
        throw Failure(
            "A library development package contains a non-library dependency");
      auto Destination = Destinations.at(Id);
      fs::create_directories(Destination);
      std::map<fs::path, fs::path> Mapped;
      std::map<fs::path, fs::path> SearchRoots;
      auto MapPath = [&](const fs::path &Source) {
        auto Normalized =
            (fs::weakly_canonical(Source.parent_path()) / Source.filename())
                .lexically_normal();
        if (auto Found = Mapped.find(Normalized); Found != Mapped.end())
          return Found->second;
        std::optional<std::pair<fs::path, fs::path>> Matched;
        for (const auto &[Original, Exported] : SearchRoots)
          if (below(Original, Normalized))
            if (!Matched ||
                Original.native().size() > Matched->first.native().size())
              Matched = std::make_pair(Original, Exported);
        if (Matched) {
          auto Relative =
              Matched->second / Normalized.lexically_relative(Matched->first);
          Mapped[Normalized] = Relative;
          return Relative;
        }
        auto Relative = Normalized.lexically_relative(Node.Input.Project.Root);
        if (!isRelativeSafe(Relative) || *Relative.begin() == "obj" ||
            *Relative.begin() == "bin")
          Relative =
              fs::path("interface") /
              sha256Text(pathText(Normalized.parent_path())).substr(0, 16) /
              Normalized.filename();
        Mapped[Normalized] = Relative;
        return Relative;
      };
      std::set<fs::path> Copied;
      auto CopyInterface = [&](const fs::path &Source) {
        auto Relative = MapPath(Source);
        if (Copied.insert(Relative).second)
          Copy(Source, Destination / Relative, Staging);
        return Relative;
      };
      Strings PrivateIncludes, PublicIncludes;
      auto Includes = [&](const Strings &Values, Strings &Result) {
        for (const auto &Value : Values) {
          auto Source = pathFromText(Value);
          if (!Source.is_absolute())
            Source = Node.Input.Project.Root / Source;
          Source = Source.lexically_normal();
          if (fs::exists(Source) && !fs::is_directory(Source))
            throw Failure("Package include path is not a directory: " +
                          pathText(Source));
          if (fs::is_directory(Source))
            Source = fs::canonical(Source);
          if (SdkPath(Source))
            continue;
          auto Relative = Source.lexically_relative(Node.Input.Project.Root);
          if (Relative != "." &&
              (!isRelativeSafe(Relative) || *Relative.begin() == "obj" ||
               *Relative.begin() == "bin"))
            Relative = fs::path("interface") /
                       sha256Text(pathText(Source)).substr(0, 16);
          SearchRoots[Source] = Relative;
          fs::create_directories(Destination / Relative);
          Result.push_back(pathText(Relative));
          if (!fs::is_directory(Source))
            continue;
          for (auto It = fs::recursive_directory_iterator(Source);
               It != fs::recursive_directory_iterator(); ++It) {
            const auto &Entry = *It;
            if (Source == Node.Input.Project.Root) {
              auto First = *Entry.path().lexically_relative(Source).begin();
              auto Name = pathText(First);
              if (First == "obj" || First == "bin" || First == "tests" ||
                  First == "build" || First == "out" || Name.starts_with('.')) {
                It.disable_recursion_pending();
                continue;
              }
            }
            if (SdkPath(Entry.path()) ||
                (Entry.is_directory() &&
                 fs::is_regular_file(Entry.path() / ".llm"))) {
              It.disable_recursion_pending();
              continue;
            }
            if (!Entry.is_regular_file() || !isHeader(Entry.path()))
              continue;
            auto File = Relative / Entry.path().lexically_relative(Source);
            Mapped[Entry.path().lexically_normal()] = File;
            if (Copied.insert(File).second)
              Copy(Entry.path(), Destination / File, Staging);
          }
        }
      };
      Strings ImplicitPrivate, ImplicitPublic;
      if (fs::is_directory(Node.Input.Project.Root / "src"))
        ImplicitPrivate.push_back("src");
      if (fs::is_directory(Node.Input.Project.Root / "include"))
        ImplicitPublic.push_back("include");
      ImplicitPrivate.insert(ImplicitPrivate.end(),
                             Node.Settings.Includes.Private.begin(),
                             Node.Settings.Includes.Private.end());
      ImplicitPublic.insert(ImplicitPublic.end(),
                            Node.Settings.Includes.Public.begin(),
                            Node.Settings.Includes.Public.end());
      for (const auto &Generator : Node.Settings.Generators)
        for (const auto &Value : Generator.Outputs)
          if (isHeader(pathFromText(Value)))
            ImplicitPrivate.push_back(
                pathText(pathFromText(Value).parent_path()));
      Includes(ImplicitPrivate, PrivateIncludes);
      Includes(ImplicitPublic, PublicIncludes);
      for (auto It = fs::recursive_directory_iterator(Node.Input.Project.Root);
           It != fs::recursive_directory_iterator(); ++It) {
        auto Relative = It->path().lexically_relative(Node.Input.Project.Root);
        auto First = *Relative.begin();
        auto FirstName = pathText(First);
        if (First == "obj" || First == "bin" || First == "tests" ||
            First == "build" || First == "out" || FirstName.starts_with('.') ||
            SdkPath(It->path())) {
          It.disable_recursion_pending();
          continue;
        }
        if (It->is_directory() && fs::is_regular_file(It->path() / ".llm")) {
          It.disable_recursion_pending();
          continue;
        }
        if (fs::is_symlink(It->symlink_status()))
          It.disable_recursion_pending();
        if ((It->is_regular_file() || fs::is_symlink(It->symlink_status())) &&
            isHeader(It->path()))
          CopyInterface(It->path());
      }
      Strings ModuleSources;
      std::map<std::string, fs::path> StandardObjects;
      if (Node.Input.Imported)
        for (const auto &[Name, File] : Node.Input.Imported->StandardObjects)
          StandardObjects.emplace(Name, Node.Input.Project.Root / File);
      for (const auto &Relative : Node.Sources) {
        if (sourceLanguage(Relative) != Language::Cxx)
          continue;
        auto Source = Node.Input.Project.Root / Relative;
        auto Found = ObjectFiles.find({Id, Source});
        if (Found == ObjectFiles.end())
          throw Failure("Missing compilation action for packaging: " +
                        pathText(Source));
        auto Object = Found->second;
        if (!CompileCommands.contains(Object))
          throw Failure("Missing compiler arguments for packaging: " +
                        pathText(Source));
        if ((Node.Settings.CxxStandard == Standard::Cxx20 ||
             Node.Settings.CxxStandard == Standard::Cxx23) &&
            Built.Modules.at(Object).Provider)
          ModuleSources.push_back(pathText(CopyInterface(Source)));
        if (auto Command = CompileCommands.find(Object);
            Command != CompileCommands.end())
          for (const auto &Text : Command->second)
            for (const auto *Logical : {"std", "std.compat"}) {
              auto Prefix = "-fmodule-file=" + std::string(Logical) + "=";
              if (Text.starts_with(Prefix)) {
                auto File = pathFromText(Text.substr(Prefix.size()));
                if (!File.is_absolute())
                  File = RootPath / File;
                File.replace_extension(".o");
                auto [Entry, Added] =
                    StandardObjects.try_emplace(Logical, File);
                if (!Added && Entry->second != File)
                  throw Failure(
                      "Incompatible standard module objects in package: " +
                      std::string(Logical));
              }
            }
      }
      toml::table Exported{
          {"type", std::string(productTypeName(Node.Input.Project.Type))},
          {"name", Node.Input.Project.Name},
          {"sources", array(ModuleSources)},
          {"standards",
           toml::array{std::string(standardName(Node.Settings.CStandard)),
                       std::string(standardName(Node.Settings.CxxStandard))}},
          {"includes", toml::table{{"private", array(PrivateIncludes)},
                                   {"public", array(PublicIncludes)}}},
          {"defines",
           toml::table{{"private", array(Node.Settings.Defines.Private)},
                       {"public", array(Node.Settings.Defines.Public)}}},
          {"flags",
           toml::table{{"c", array(Node.Settings.Arguments.C)},
                       {"cxx", array(Node.Settings.Arguments.Cxx)},
                       {"asm", array(Node.Settings.Arguments.Assembly)},
                       {"link", array(Node.Settings.Arguments.Link)}}}};
      Strings PrivateDependencies, PublicDependencies;
      for (const auto &[Provider, IsPublic] : Node.Input.Dependencies) {
        auto Path =
            pathText(Destinations.at(Provider).lexically_relative(Destination));
        if (!Path.starts_with('.'))
          Path = "./" + Path;
        (IsPublic ? PublicDependencies : PrivateDependencies).push_back(Path);
      }
      Exported.insert("dependencies",
                      toml::table{{"private", array(PrivateDependencies)},
                                  {"public", array(PublicDependencies)}});
      Strings SdkLibraries;
      for (const auto &Library : Node.Settings.Libraries) {
        auto Path = pathFromText(Library);
        if (Path.extension() == ".framework" &&
            (Path.has_parent_path() || Path.is_absolute())) {
          auto Source =
              Path.is_absolute() ? Path : Node.Input.Project.Root / Path;
          rejectSymlinkAncestors(Source);
          if (!fs::is_directory(Source))
            throw Failure("Declared framework is missing: " + pathText(Source));
          auto Relative = fs::path("lib") / Source.filename();
          fs::create_directories(Destination / Relative);
          for (const auto &Entry : fs::recursive_directory_iterator(Source)) {
            auto Output = Destination / Relative /
                          Entry.path().lexically_relative(Source);
            if (fs::is_directory(Entry.symlink_status()))
              fs::create_directories(Output);
            else
              Copy(Entry.path(), Output, Staging);
          }
          SdkLibraries.push_back(pathText(Relative));
          continue;
        }
        if (Path.has_parent_path() || Path.is_absolute() ||
            Path.extension() == ".a" || Path.extension() == ".lib" ||
            Path.extension() == ".so" || Path.extension() == ".dylib") {
          auto Source =
              Path.is_absolute() ? Path : Node.Input.Project.Root / Path;
          auto Relative = fs::path("lib") / Source.filename();
          Copy(Source, Destination / Relative, Staging);
          SdkLibraries.push_back(pathText(Relative));
        } else
          SdkLibraries.push_back(Library);
      }
      Exported.insert("libraries", array(SdkLibraries));
      if (Node.Input.Project.Type != ProductType::HeaderOnlyLibrary) {
        auto Artifact = RootPath / BuildPlan.Artifacts.at(Id);
        auto Relative = fs::path("lib") / Artifact.filename();
        Copy(Artifact, Destination / Relative, Staging);
        bool Cpp = (Node.Input.Imported && Node.Input.Imported->UsesCxx) ||
                   std::ranges::any_of(Node.Sources, [](const auto &File) {
                     return sourceLanguage(File) == Language::Cxx;
                   });
        toml::table Metadata{
            {"artifact", pathText(Relative)},
            {"target", std::string(targetName(Options.BuildTarget))},
            {"profile",
             (Options.Mode == Profile::Release) ? "release" : "debug"},
            {"toolchain",
             toolchainIdentity(Call.Toolchain ? *Call.Toolchain
                                              : installedToolchain(Call),
                               Options.BuildTarget)},
            {"cpp", Cpp}};
        toml::table Support;
        if (Node.Input.Project.Type == ProductType::StaticLibrary)
          for (const auto &[Logical, Object] : StandardObjects) {
            auto ExportedObject =
                fs::path("lib") / "std" / pathFromText(Logical + ".o");
            Copy(Object, Destination / ExportedObject, Staging);
            Support.insert(Logical, pathText(ExportedObject));
          }
        if (!Support.empty())
          Metadata.insert("standard-objects", std::move(Support));
        if (isWindowsTarget(Options.BuildTarget) &&
            Node.Input.Project.Type == ProductType::SharedLibrary) {
          auto Import =
              Node.Input.Imported
                  ? Node.Input.Project.Root / Node.Input.Imported->LinkArtifact
                  : Artifact.parent_path() /
                        linkArtifactName(Node.Input.Project.Name,
                                         Node.Input.Project.Type,
                                         Options.BuildTarget);
          auto ImportRelative = fs::path("lib") / Import.filename();
          Copy(Import, Destination / ImportRelative, Staging);
          Metadata.insert("import-library", pathText(ImportRelative));
        }
        document(Destination / ".llm-export", Metadata);
      }
      document(Destination / ".llm", Exported);
      resources(Node.Input.Project, Node.Settings, Destination, Staging, Links);
    }
  }
  validateLinks(Payload, Links);
  createTarball(Call, Payload, Staging / "result.tar.gz", Links);
  rejectSymlinkAncestors(Output);
  replaceFile(Staging / "result.tar.gz", Output);
  emit(Call, EventKind::Progress, "Packaged " + pathText(Output));
}

}

namespace llm::runtime {
int packageProject(Context &Call, const Command &Request,
                   const Manifest &Project) {
  if (Project.Type == ProductType::Test)
    throw Failure("Test products cannot be packaged for distribution");
  auto Packages = resolveDependencies(Call, Request, Project.Root, {Project});
  auto Graph = loadGraph(Call, Project, Request.BuildTarget, Packages);
  auto Build = Request;
  Build.Action = Operation::Build;
  auto Built = buildProject(Call, Build, Graph, BuildDetails::Modules);
  std::set<Path> Roots;
  for (const auto &[Identity, Node] : Graph.Projects)
    Roots.insert(Node.Input.Project.Root);
  std::vector<std::unique_ptr<FileLock>> Locks;
  for (const auto &Root : Roots) {
    std::filesystem::create_directories(Root / "obj");
    Locks.push_back(
        std::make_unique<FileLock>(Call, Root / "obj/.guard", true));
  }
  exportPackage(Call, Graph, Request,
                Call.Toolchain ? *Call.Toolchain : installedToolchain(Call),
                Built);
  return 0;
}
}
