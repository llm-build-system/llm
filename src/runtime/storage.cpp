module llm.runtime;

import :internal;
import std;

namespace llm::runtime {

std::string uniqueId() { return randomHex(16); }

Path nativePath(const Path &File) {
#ifdef _WIN32
  if (File.empty())
    return File;
  auto Value = File;
  Value.make_preferred();
  if (Value.native().starts_with(LR"(\\?\)") ||
      Value.native().starts_with(LR"(\\.\)"))
    return Value;
  Value = std::filesystem::absolute(Value).lexically_normal();
  Value.make_preferred();
  if (Value.native().starts_with(LR"(\\)"))
    return Path(std::wstring(LR"(\\?\UNC\)") + Value.native().substr(2));
  return Path(std::wstring(LR"(\\?\)") + Value.native());
#else
  return File;
#endif
}

std::string readText(const Path &File) {
  std::ifstream Input(nativePath(File), std::ios::binary);
  if (!Input)
    throw Failure("Cannot read " + pathText(File));
  std::ostringstream Text;
  Text << Input.rdbuf();
  if (Input.bad())
    throw Failure("Cannot finish reading " + pathText(File));
  return std::move(Text).str();
}

void writeText(const Path &File, std::string_view Text) {
  rejectSymlinkAncestors(File);
  std::ofstream Output(nativePath(File), std::ios::binary | std::ios::trunc);
  Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  Output.close();
  if (!Output)
    throw Failure("Cannot write " + pathText(File));
}

void atomicWrite(const Path &File, std::string_view Text) {
  rejectSymlinkAncestors(File);
  if (std::filesystem::is_regular_file(nativePath(File)) &&
      readText(File) == Text)
    return;
  std::filesystem::create_directories(nativePath(File.parent_path()));
  auto Temporary = File.parent_path() / (".llm-write-" + uniqueId());
  struct Cleanup {
    Path File;
    ~Cleanup() {
      std::error_code Error;
      std::filesystem::remove(File, Error);
    }
  } Guard{nativePath(Temporary)};
  std::ofstream Output(nativePath(Temporary),
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!Output)
    throw Failure("Cannot create atomic output beside " + pathText(File));
  Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  Output.close();
  if (!Output)
    throw Failure("Cannot finish atomic output beside " + pathText(File));
  replaceFile(Temporary, File);
}

bool ownedPath(const Path &File) {
  return isRelativeSafe(File) &&
         (*File.begin() == "obj" || *File.begin() == "bin") && File != "obj" &&
         File != "bin" && File != "obj/.owned" && File != "obj/.guard";
}

OwnedOutputs readOwned(const Path &Root) {
  auto File = Root / "obj/.owned";
  rejectSymlinkAncestors(File);
  if (!std::filesystem::exists(File))
    return {};
  std::istringstream Input(readText(File));
  std::string Text;
  OwnedOutputs Result;
  while (std::getline(Input, Text)) {
    if (Text.ends_with('\r'))
      Text.pop_back();
    auto Relative = pathFromText(Text);
    if (!ownedPath(Relative) || !Result.insert(Relative).second ||
        Result.size() > 200000)
      throw Failure("Unsafe project output inventory");
  }
  if (!Input.eof())
    throw Failure("Invalid project output inventory");
  return Result;
}

void checkOutput(const Path &Root, const Path &File) {
  if (!ownedPath(File))
    throw Failure("Unsafe generated output: " + pathText(File));
  rejectSymlinkAncestors(Root / File);
  auto Status = std::filesystem::symlink_status(Root / File);
  if (std::filesystem::exists(Status) &&
      !std::filesystem::is_regular_file(Status))
    throw Failure("Generated output has changed file type: " + pathText(File));
}

void saveOwned(const Path &Root, const OwnedOutputs &Owned) {
  std::ostringstream Text;
  for (const auto &File : Owned)
    Text << pathText(File) << '\n';
  atomicWrite(Root / "obj/.owned", Text.str());
}

void removeOutputs(const Path &Root, const OwnedOutputs &Owned) {
  for (const auto &File : Owned)
    checkOutput(Root, File);
  std::set<Path> Directories;
  for (const auto &File : Owned) {
    removeOutputFile(Root / File);
    for (auto Directory = File.parent_path();
         Directory != "obj" && Directory != "bin" && !Directory.empty();
         Directory = Directory.parent_path())
      Directories.insert(Directory);
  }
  std::vector<Path> Order(Directories.begin(), Directories.end());
  std::ranges::sort(Order, [](const auto &Left, const auto &Right) {
    return Left.native().size() > Right.native().size();
  });
  for (const auto &Directory : Order) {
    std::error_code Error;
    std::filesystem::remove(Root / Directory, Error);
  }
}

Path findProgram(const Path &Root, std::string_view Name) {
  auto Input = pathFromText(Name);
  auto Check = [](Path File) -> std::optional<Path> {
#ifdef _WIN32
    if (!File.has_extension())
      File += ".exe";
    if (File.extension() != ".exe" && File.extension() != ".com")
      return {};
#endif
    if (!std::filesystem::is_regular_file(File))
      return {};
#ifndef _WIN32
    auto Permissions = std::filesystem::status(File).permissions();
    using enum std::filesystem::perms;
    if ((Permissions & (owner_exec | group_exec | others_exec)) == none)
      return {};
#endif
    return std::filesystem::canonical(File);
  };
  if (Input.is_absolute() ||
      Name.find_first_of("/\\") != std::string_view::npos) {
    if (auto Found = Check(Input.is_absolute() ? Input : Root / Input))
      return *Found;
  } else if (auto Search = std::getenv("PATH")) {
    std::istringstream Entries(Search);
    std::string Entry;
#ifdef _WIN32
    constexpr char Separator = ';';
#else
    constexpr char Separator = ':';
#endif
    while (std::getline(Entries, Entry, Separator)) {
      auto Directory = pathFromText(Entry);
      if (Directory.is_relative())
        Directory = Root / Directory;
      if (auto Found = Check(Directory / Input))
        return *Found;
    }
  }
  throw Failure("Program not found: " + std::string(Name));
}

}
