module llm.runtime;
import :internal;
import std;

namespace llm {
Result<int> runInternal(Context &Call, const Strings &Arguments) {
  using namespace runtime;
  namespace fs = std::filesystem;
  try {
    if (Arguments.size() < 2)
      throw Failure("Missing internal operation");
    if (Call.Executable.empty())
      Call.Executable = executablePath();
    const auto &Operation = Arguments[1];

    auto Root = fs::canonical(Call.InvocationDirectory);
    auto Owned = readOwned(Root);
    auto Validate = [&](const Path &File) {
      checkOutput(Root, File);
      if (!Owned.contains(File))
        throw Failure("Internal tool output is not owned by this project: " +
                      pathText(File));
    };
    if (Operation == "--internal-collate") {
      if (Arguments.size() != 3)
        throw Failure("Invalid module collation invocation");
      auto Descriptor = pathFromText(Arguments[2]);
      Validate(Descriptor);
      return collateModules(Call, Root / Descriptor);
    }
    if (Operation == "--internal-generate") {
      if (Arguments.size() != 3)
        throw Failure("Invalid internal generator invocation");
      auto Response = pathFromText(Arguments[2]);
      Validate(Response);
      auto Values = responseArguments(readText(Root / Response));
      if (Values.empty())
        throw Failure("Missing generator directory");
      auto Owner = fs::canonical(pathFromText(Values.front()));
      auto OwnerFiles = readOwned(Owner);
      std::vector<Path> Outputs;
      std::size_t Index = 1;
      while (Index + 1 < Values.size() && Values[Index] == "--output") {
        auto File = pathFromText(Values[Index + 1]);
        auto Relative = File.lexically_relative(Owner);
        checkOutput(Owner, Relative);
        if (!OwnerFiles.contains(Relative))
          throw Failure("Generator output is not owned by its project");
        Outputs.push_back(File);
        Index += 2;
      }
      if (Outputs.empty() || Index + 1 >= Values.size() ||
          Values[Index++] != "--")
        throw Failure("Invalid generator command");
      auto Program = pathFromText(Values[Index++]);
      Strings Flags(Values.begin() + Index, Values.end());
      try {
        auto Result = runProcess(Call, Program, Flags,
                                 {.Directory = Owner, .OutputToError = true});
        if (!Result.success())
          throw BuildFailure(Result.ExitCode, "Generator failed");
        for (const auto &File : Outputs) {
          checkOutput(Owner, File.lexically_relative(Owner));
          if (!fs::is_regular_file(nativePath(File)))
            throw Failure("Generator did not produce " + pathText(File));
        }
      } catch (...) {
        for (const auto &File : Outputs) {
          checkOutput(Owner, File.lexically_relative(Owner));
          removeOutputFile(File);
        }
        throw;
      }
      return 0;
    }
    if (Operation == "--internal-copy") {
      if (Arguments.size() != 3)
        throw Failure("Invalid internal copy invocation");
      auto Response = pathFromText(Arguments[2]);
      Validate(Response);
      auto Values = responseArguments(readText(Root / Response));
      if (Values.size() < 3)
        throw Failure("Incomplete copy command");
      auto Destination = pathFromText(Values[0]),
           Temporary = pathFromText(Values[1]);
      Validate(Destination);
      Validate(Temporary);
      auto Source = pathFromText(Values[2]);
      rejectSymlinkAncestors(Root / Source);
      auto Hash = sha256File(Root / Source);
      for (std::size_t Index = 3; Index < Values.size(); ++Index) {
        auto Other = pathFromText(Values[Index]);
        rejectSymlinkAncestors(Root / Other);
        if (sha256File(Root / Other) != Hash)
          throw Failure("Runtime library filename collision: " +
                        pathText(Destination.filename()));
      }
      if (fs::is_regular_file(nativePath(Root / Destination)) &&
          sha256File(Root / Destination) == Hash)
        return 0;
      removeOutputFile(Root / Temporary);
      fs::copy_file(nativePath(Root / Source), nativePath(Root / Temporary));
      fs::permissions(nativePath(Root / Temporary),
                      fs::status(nativePath(Root / Source)).permissions());
      replaceFile(Root / Temporary, Root / Destination);
      return 0;
    }
    if (Operation != "--internal-archive" || Arguments.size() != 6)
      throw Failure("Invalid internal tool invocation");
    auto Output = pathFromText(Arguments[2]),
         Response = pathFromText(Arguments[3]);
    auto Temporary = Response;
    Temporary.replace_extension(".tmp");
    for (const auto &File : {Output, Response, Temporary})
      Validate(File);
    auto Expected = Output;
    if (*Output.begin() == "bin") {
      auto Relative = Output.lexically_relative("bin");
      Expected = Path("obj") / Relative.parent_path() / "projects/root" /
                 Output.filename();
    }
    Expected += ".rsp";
    if (Response != Expected ||
        (Output.extension() != ".a" && Output.extension() != ".lib"))
      throw Failure("Invalid internal archive paths");
    auto Target = take(parseTarget(Arguments[5]));
    auto Toolchain = fs::canonical(pathFromText(Arguments[4]));
    auto Program = Toolchain / "bin/llvm-ar";
#ifdef _WIN32
    Program += ".exe";
#endif
    removeOutputFile(Root / Temporary);
    struct Cleanup {
      Path File;
      ~Cleanup() {
        std::error_code Error;
        fs::remove(nativePath(File), Error);
      }
    } Guard{Root / Temporary};
    auto Format =
        Target == Target::MacosArm64 ? "--format=darwin" : "--format=gnu";
    auto Result = runProcess(
        Call, Program,
        {Format, "qcsD", pathText(Temporary), "@" + pathText(Response)},
        {.Directory = Root, .ToolBin = Toolchain / "bin"});
    if (!Result.success())
      return Result.ExitCode;
    replaceFile(Root / Temporary, Root / Output);
    return 0;
  } catch (const Failure &Error) {
    return std::unexpected(Error.Error);
  } catch (const std::exception &Error) {
    return std::unexpected(Diagnostic{Error.what()});
  }
}
}
