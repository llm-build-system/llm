import std;
import llm.runtime;

namespace {
using namespace llm;
namespace fs = std::filesystem;

void require(bool Condition, std::string_view Message) {
  if (!Condition)
    throw std::runtime_error(std::string(Message));
}
void write(const Path &File, std::string_view Text) {
  fs::create_directories(File.parent_path());
  std::ofstream Output(File, std::ios::binary);
  Output << Text;
  require(bool(Output), "Cannot write integration fixture");
}
std::string read(const Path &File) {
  std::ifstream Input(File, std::ios::binary);
  require(bool(Input), "Missing integration output");
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

struct Fixture {
  Path Root;
  Context Call;
  std::string Output;
  std::size_t Work = 0;
  Path GeneratorProgram;
  Target BuildTarget = nativeTarget();
  bool GlobalInstallation = false;

  Fixture(const Path &Toolchain, const Path &Executable) {
    auto Base = fs::canonical(fs::temp_directory_path());
    for (unsigned Attempt = 0; Attempt < 100; ++Attempt) {
      Root =
          Base /
          pathFromText(
              "llm contract-äΩ-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              '-' + std::to_string(Attempt));
      if (fs::create_directory(Root))
        break;
      Root.clear();
    }
    require(!Root.empty(), "Cannot create integration workspace");
    Call.Version = "1.0.0";
    Call.Toolchain = fs::canonical(Toolchain);
    Call.Executable = fs::canonical(Executable);
    Call.InvocationDirectory = Root;
    Call.UserDirectory = Root / "profile";
    Call.Output.Quiet = true;
    Call.Emit = [&](const Event &Event) { Output += Event.Text + '\n'; };
  }
  ~Fixture() {
    if (std::uncaught_exceptions()) {
      std::cerr << "Integration workspace retained: " << pathText(Root) << '\n';
    } else {
      std::error_code Error;
      fs::remove_all(Root, Error);
    }
  }
  int run(Operation Action, const Path &Project, Strings Arguments = {},
          std::string Selection = {}, Profile Mode = Profile::Release) {
    Command Request;
    Request.Action = Action;
    Request.Project = Project;
    Request.Mode = Mode;
    Request.Arguments = std::move(Arguments);
    Request.Selection = std::move(Selection);
    Request.BuildTarget = BuildTarget;
    Request.Global = GlobalInstallation;
    auto Snapshot = [&] {
      std::map<Path, fs::file_time_type> Files;
      for (const auto &Entry : fs::recursive_directory_iterator(Root))
        if (Entry.is_regular_file() && (Entry.path().extension() == ".o" ||
                                        Entry.path().extension() == ".pcm"))
          Files.emplace(Entry.path(), Entry.last_write_time());
      return Files;
    };
    auto Before = Snapshot();
    Output.clear();
    Work = 0;
    auto Result = execute(Call, Request);
    if (!Result)
      throw std::runtime_error(Result.error().Message + '\n' + Output);
    auto After = Snapshot();
    Work = Before == After ? 0 : 1;
    return *Result;
  }
};

void buildAndPackageContract(Fixture &Fixture) {
  const auto &Root = Fixture.Root;
  write(Root / "library/.llm", "type = 'static-library'\nname = 'answer'\n");
  write(Root / "library/include/answer.h", "int answer(void);\n");
  write(Root / "library/code/answer.c",
        "#include <answer.h>\nint answer(void){return 42;}\n");
  write(Root / "app/.llm",
        "type = 'executable'\ndependencies.private = ['../library']\n");
  write(Root / "app/main.c",
        "#include <answer.h>\nint main(void){return answer()!=42;}\n");
  require(Fixture.run(Operation::Build, Root / "app") == 0 && Fixture.Work > 0,
          "C dependency graph builds");
  auto Objects = Root / "app/obj";
  require(fs::is_regular_file(Objects / ".owned") &&
              fs::is_regular_file(Objects / ".guard"),
          "Build metadata uses the agreed internal dotfiles");
  for (const auto &Entry : fs::recursive_directory_iterator(Root / "app/bin"))
    if (Entry.is_regular_file())
      require(Entry.path().filename() == artifactName("app",
                                                      ProductType::Executable,
                                                      Fixture.BuildTarget),
              "Bin contains only the requested product");
  for (const auto &Entry : fs::recursive_directory_iterator(Objects))
    require(
        Entry.path().filename() != "compile_commands.json" &&
            Entry.path().extension() != ".result" &&
            !pathText(Entry.path().filename()).starts_with(".ninja-result-"),
        "Build leaves no editor database or executor result files");
  require(!read(Objects / ".owned").starts_with('"'),
          "Output inventory stores one unquoted path per line");
  require(Fixture.run(Operation::Run, Root / "app") == 0 && Fixture.Work == 0,
          "A current build starts no compiler/linker/scanner");
  auto ForeignDatabase = Root / "app/compile_commands.json";
  write(ForeignDatabase, "foreign editor configuration\n");
  require(Fixture.run(Operation::Check, Root / "app") == 0,
          "Check compiles the graph");
  require(read(ForeignDatabase) == "foreign editor configuration\n",
          "Foreign root compilation database is preserved");
  write(Root / "library/src/removed.c", "int removed(void){return 1;}\n");
  require(Fixture.run(Operation::Build, Root / "app") == 0 && Fixture.Work > 0,
          "Added sources rebuild their archive");
  fs::remove(Root / "library/src/removed.c");
  require(Fixture.run(Operation::Build, Root / "app") == 0 && Fixture.Work > 0,
          "Removed sources recreate the archive");
  require(Fixture.run(Operation::Package, Root / "library") == 0,
          "Development library packages");
  auto Archive =
      Root / "library/bin" / pathFromText(targetName(Fixture.BuildTarget)) /
      "release" /
      pathFromText("answer-" + std::string(targetName(Fixture.BuildTarget)) +
                   ".tar.gz");
  auto First = read(Archive);
  require(!First.empty() &&
              Fixture.run(Operation::Package, Root / "library") == 0 &&
              read(Archive) == First,
          "Unchanged package archives are byte reproducible");
  write(
      Root / "extractor/.llm",
      "type='header-only-library'\n[[generate]]\ninputs=['package.tar.gz']"
      "\noutputs=['obj/unpacked/"
      ".llm']\ncommand=['tar','-xzf','package.tar.gz','-C','obj/unpacked']\n");
  fs::create_directories(Root / "extractor/obj/unpacked");
  fs::copy_file(Archive, Root / "extractor/package.tar.gz");
  require(Fixture.run(Operation::Build, Root / "extractor") == 0,
          "A generator unpacks the development package");
  fs::rename(Root / "library", Root / "library-unavailable");
  write(Root / "consumer/.llm", "type='executable'\ndependencies.private=['../"
                                "extractor/obj/unpacked']\n");
  write(Root / "consumer/src/main.c",
        "#include <answer.h>\nint main(void){return answer()!=42;}\n");
  require(
      Fixture.run(Operation::Run, Root / "consumer") == 0,
      "A relocated development package links without its original source tree");
  require(Fixture.run(Operation::Clean, Root / "app") == 0,
          "Clean removes owned outputs");
  require(read(ForeignDatabase) == "foreign editor configuration\n",
          "Clean preserves the foreign root database");
  write(Root / "shared/.llm",
        "type = 'shared-library'\nname = 'shared-answer'\n");
  write(Root / "shared/include/shared.h", "int shared_answer(void);\n");
  write(
      Root / "shared/src/shared.c",
      "int global_value=42;\n#ifdef _WIN32\n__declspec(dllexport)\n#endif\nint "
      "shared_answer(void){return global_value;}\n");
  write(Root / "shared-app/.llm",
        "type = 'executable'\ndependencies.private = ['../shared']\n");
  write(Root / "shared-app/src/main.c",
        "#include <shared.h>\nint main(void){return shared_answer()!=42;}\n");
  require(Fixture.run(Operation::Run, Root / "shared-app") == 0,
          "Shared libraries use relocatable code and stage their runtime "
          "beside the application");
  write(Root / "assembly/.llm",
        "type='executable'\n[inputs]\n'src/value.s'=['src/value.inc']\n");
  write(Root / "assembly/src/main.c",
        "extern int value;\nint main(void){return value!=42;}\n");
  auto Symbol = Fixture.BuildTarget == Target::MacosArm64 ? "_value" : "value";
  write(Root / "assembly/src/value.s",
        std::string(".data\n.p2align 2\n.globl ") + Symbol + '\n' + Symbol +
            ":\n.include \"value.inc\"\n");
  write(Root / "assembly/src/value.inc", ".long 42\n");
  require(Fixture.run(Operation::Run, Root / "assembly") == 0,
          "Raw assembly is compiled and linked with C");
  write(Root / "assembly/src/value.inc", ".long 43\n");
  require(Fixture.run(Operation::Run, Root / "assembly") == 1 &&
              Fixture.Work > 0,
          "Changing an assembler include rebuilds the executable");
}

void moduleAndTestContract(Fixture &Fixture) {
  const auto &Root = Fixture.Root;
  write(Root / "standard-context/.llm",
        "type='executable'\nflags.cxx=['-fno-exceptions']\n");
  write(
      Root / "standard-context/main.cpp",
      "import std.compat;\nint main(){return std::vector<int>{42}[0]!=42;}\n");
  require(Fixture.run(Operation::Run, Root / "standard-context") == 0,
          "Standard modules use the consumer's compiler options");
  require(Fixture.run(Operation::Build, Root / "standard-context") == 0 &&
              Fixture.Work == 0,
          "Standard module context is stable on no-op builds");
  write(Root / "modules/.llm",
        "type = 'static-library'\nname = 'module-answer'\n");
  write(Root / "modules/units/answer.cppm",
        "export module answer;\nimport std;\n#ifdef SECOND\nimport "
        "extra;\n#endif\nexport int answer(){return "
        "std::vector<int>{42}.at(0);}\n");
  write(Root / "modules/units/extra.cppm", "export module extra;\n");
  write(Root / "modules/tests/.llm",
        "type='test'\ndependencies.private=['../']\n");
  write(Root / "modules/tests/main.cpp",
        "import answer;\nint main(){return answer()!=42;}\n");
  require(Fixture.run(Operation::Test, Root / "modules") == 0,
          "Explicit test projects consume their declared library's module");
  write(Root / "modules/checks/.llm", "type='test'\n");
  write(Root / "modules/checks/main.c", "int main(void){return 0;}\n");
  require(Fixture.run(Operation::Test, Root / "modules") == 0 &&
              Fixture.Output.find("2 passed") != std::string::npos,
          "Test discovery follows explicit manifests rather than folder names");
  require(Fixture.run(Operation::Test, Root / "modules") == 0 &&
              Fixture.Work == 0,
          "Module and test no-op builds reuse scans and BMIs");
  write(Root / "modules/.llm", "type='static-library'\nname='module-answer'\n"
                               "defines.private=['SECOND']\n");
  require(Fixture.run(Operation::Test, Root / "modules") == 0 &&
              Fixture.Work > 0,
          "Changed compile flags invalidate scanner dependencies");
  require(Fixture.run(Operation::Test, Root / "modules") == 0 &&
              Fixture.Work == 0,
          "Updated module scans remain incremental");
  for (const auto &Entry : fs::recursive_directory_iterator(Root / "modules"))
    if (Entry.is_regular_file() && Entry.path().extension() == ".json") {
      auto Name = pathText(Entry.path().filename());
      require(Name.ends_with(".scan.json") &&
                  !Name.ends_with(".catalog.scan.json"),
              "Only the authoritative P1689 dependency results remain");
      if (Name == "answer.cppm.o.scan.json")
        require(read(Entry.path()).find("\"extra\"") != std::string::npos,
                "The scanner sees an import enabled by changed defines");
    }
  write(Root / "modules/units/answer.cppm",
        "export module updated;\nimport std;\nexport int answer(){return "
        "std::vector<int>{42}.at(0);}\n");
  write(Root / "modules/tests/main.cpp",
        "import updated;\nint main(){return answer()!=42;}\n");
  require(Fixture.run(Operation::Test, Root / "modules") == 0 &&
              Fixture.Work > 0,
          "Changed providers replace the cached Clang scan");
  write(Root / "headers/.llm", "type = 'executable'\n");
  write(Root / "headers/src/value.cppm",
        "export module value;\nexport int value(){return 42;}\n");
  write(Root / "headers/src/value.hpp",
        "#pragma once\n#define HEADER_READY 1\nimport value;\ninline int "
        "headerValue(){return value();}\n");
  write(Root / "headers/src/main.cpp",
        "import \"value.hpp\";\n#if NEXT_HEADER\nimport \"extra.hpp\";\n"
        "#if !EXTRA_READY\n#error missing next header macro\n#endif\n#endif\n"
        "#if !HEADER_READY\n#error missing imported "
        "macro\n#endif\nint main(){return headerValue()!=42;}\n");
  require(Fixture.run(Operation::Run, Root / "headers") != 0,
          "Unsupported automatic header units fail instead of being emulated");
}

void generatorContract(Fixture &Fixture) {
  const auto &Root = Fixture.Root;
  std::ostringstream Manifest;
  Manifest << "type='executable'\nincludes.private=['obj/"
              "generated']\n[[generate]]\ninputs=['value.txt']\noutputs=['obj/"
              "generated/value.h']\ncommand=["
           << std::quoted(pathText(Fixture.GeneratorProgram))
           << ",'--generate','value.txt','obj/generated/value.h']\n";
  write(Root / "generated/.llm", Manifest.str());
  write(Root / "generated/value.txt", "42\n");
  write(Root / "generated/src/main.c",
        "#include <value.h>\nint main(void){return VALUE!=42;}\n");
  require(Fixture.run(Operation::Run, Root / "generated") == 0,
          "Declared generated headers exist before compilation");
  require(Fixture.run(Operation::Run, Root / "generated") == 0 &&
              Fixture.Work == 0,
          "A current generator is not started again");
  write(Root / "generated/value.txt", "43\n");
  require(Fixture.run(Operation::Run, Root / "generated") == 1 &&
              Fixture.Work > 0,
          "Changed generator input rebuilds its consumers");
  write(Root / "generated/obj/generated/foreign.txt", "preserve\n");
  write(Root / "generated/.llm", "type='executable'\n");
  write(Root / "generated/src/main.c", "int main(void){return 0;}\n");
  require(Fixture.run(Operation::Run, Root / "generated") == 0 &&
              !fs::exists(Root / "generated/obj/generated/value.h") &&
              read(Root / "generated/obj/generated/foreign.txt") ==
                  "preserve\n",
          "Removing a generator cleans only its declared outputs");
}

void registryContract(Fixture &Fixture, std::string_view Source,
                      const Path &Control) {
  const auto &Root = Fixture.Root;
  auto Project = Root / "registry";
  std::ostringstream Manifest;
  Manifest << "type='executable'\n# preserve this comment\nregistries.fixture="
           << std::quoted(std::string(Source))
           << "\ndependencies.private=['fixture:value','fixture:other']\n";
  write(Project / ".llm", Manifest.str());
  write(Project / "src/main.c",
        "#include <value.hpp>\nint main(void){return VALUE!=42;}\n");
  constexpr std::string_view Token = "REGISTRY_FIXTURE_TOKEN";
  Fixture.Call.ReadSecret = [Token](std::string_view) {
    return std::string(Token);
  };
  require(Fixture.run(Operation::Login, Project, {}, "fixture") == 0 &&
              Fixture.Output.find(Token) == std::string::npos,
          "Private registry credentials validate and persist without appearing "
          "in diagnostics");
  require(Fixture.run(Operation::Run, Project) == 0,
          "Private index and redirected archives resolve, verify, extract and "
          "build");
  require(Fixture.run(Operation::Add, Project, {}, "fixture:third@1.0.0") ==
                  0 &&
              read(Project / ".llm").find("# preserve this comment") !=
                  std::string::npos,
          "Dependency edits preserve unrelated manifest text and commit a "
          "consistent lock");
  require(Fixture.run(Operation::Run, Project) == 0,
          "The updated package closure builds");
  auto Lock = read(Project / ".lock");
  write(Control / "offline", "offline\n");
  require(Fixture.run(Operation::Run, Project) == 0 && Fixture.Work == 0,
          "A locked build works offline without restarting tools");
  std::vector<Path> Headers;
  for (const auto &Entry :
       fs::recursive_directory_iterator(Fixture.Call.UserDirectory))
    if (Entry.is_regular_file() && Entry.path().filename() == "value.hpp")
      Headers.push_back(Entry.path());
  require(!Headers.empty(),
          "The private test profile contains its verified package copies");
  std::map<Path, std::string> OriginalHeaders;
  for (const auto &Header : Headers) {
    OriginalHeaders.emplace(Header, read(Header));
    write(Header, "#define VALUE 99\n");
  }
  bool CacheRejected = false;
  try {
    Fixture.run(Operation::Run, Project);
  } catch (const std::runtime_error &) {
    CacheRejected = true;
  }
  require(CacheRejected && read(Project / ".lock") == Lock,
          "Corrupt cached sources fail without repair or lock changes");
  for (const auto &[Header, Text] : OriginalHeaders)
    write(Header, Text);
  fs::remove(Control / "offline");
  write(Control / "bad-hash", "reject\n");
  auto Original = read(Project / ".llm");
  Command Request;
  Request.Action = Operation::Add;
  Request.Project = Project;
  Request.Mode = Profile::Release;
  Request.Selection = "fixture:tampered@1.0.0";
  Fixture.Output.clear();
  auto Rejected = execute(Fixture.Call, Request);
  require(
      !Rejected && Rejected.error().Message.find(Token) == std::string::npos &&
          Fixture.Output.find(Token) == std::string::npos &&
          read(Project / ".llm") == Original && read(Project / ".lock") == Lock,
      "A false archive hash leaks no token and changes neither manifest nor "
      "lock");
  fs::remove(Control / "bad-hash");
  Request.Selection = "fixture:malformed-etag@1.0.0";
  auto InvalidRevision = execute(Fixture.Call, Request);
  require(
      !InvalidRevision &&
          InvalidRevision.error().Message.find("strong global revision ETag") !=
              std::string::npos &&
          read(Project / ".llm") == Original && read(Project / ".lock") == Lock,
      "A list of ETags is rejected rather than treated as one immutable "
      "registry snapshot");
  require(Fixture.run(Operation::Logout, Project, {}, "fixture") == 0,
          "Logout removes the isolated source-bound credential");
}
}

int main(int Argc, char **Argv) try {
  if (Argc == 6 && std::string_view(Argv[1]) == "--registry") {
    Fixture Workspace(pathFromText(Argv[2]), pathFromText(Argv[3]));
    registryContract(Workspace, Argv[4], pathFromText(Argv[5]));
    std::cout << "Registry transport and integrity contracts passed.\n";
    return 0;
  }
#ifdef __APPLE__
  if (Argc == 4 && std::string_view(Argv[1]) == "--install-contract") {
    Fixture Workspace(pathFromText(Argv[2]), pathFromText(Argv[3]));
    Workspace.Call.Version = "1.0.0-alpha-contract";
    Workspace.GlobalInstallation = true;
    const Path Prefix =
        Path("/usr/local/lib/llm/versions") / Workspace.Call.Version;
    const Path CommandPath = "/usr/local/bin/llm";
    require(!fs::exists(Prefix), "Installer contract release already exists");
    auto Status = fs::symlink_status(CommandPath);
    require(!fs::exists(Status) || fs::is_symlink(Status),
            "Installer contract requires an absent or linked existing command");
    std::optional<Path> Original;
    if (fs::is_symlink(Status))
      Original = fs::read_symlink(CommandPath);
    struct Restore {
      Fixture &Workspace;
      Path CommandPath;
      std::optional<Path> Original;
      bool Active = true;
      ~Restore() {
        if (!Active)
          return;
        try {
          Workspace.run(Operation::Uninstall, {});
          if (Original && !fs::exists(fs::symlink_status(CommandPath)))
            fs::create_symlink(*Original, CommandPath);
        } catch (const std::exception &Error) {
          std::cerr << "Installer contract cleanup failed: " << Error.what()
                    << '\n';
        }
      }
    } Guard{Workspace, CommandPath, Original};
    auto ReleaseExecutable = Workspace.Call.Executable;
    auto Incomplete = Workspace.Root / "incomplete-release/llm";
    fs::create_directories(Incomplete.parent_path());
    fs::copy_file(ReleaseExecutable, Incomplete);
    Workspace.Call.Executable = Incomplete;
    Command MissingLicenses;
    MissingLicenses.Action = Operation::Install;
    MissingLicenses.Global = true;
    auto Missing = execute(Workspace.Call, MissingLicenses);
    require(!Missing &&
                Missing.error().Message.find(
                    "Release license file is missing") != std::string::npos &&
                !fs::exists(Prefix),
            "An incomplete release fails without installing a partial version");
    Workspace.Call.Executable = ReleaseExecutable;
    std::cout << "Checking the native Mac SDK installation contract.\n"
              << std::flush;
    require(Workspace.run(Operation::Install, {}) == 0,
            "Installer extracts and validates the actual pinned LLVM and Apple "
            "SDK archives");
    require(
        fs::is_regular_file(
            Prefix /
            "toolchain/targets/aarch64-apple-darwin/sdk/usr/include/stdio.h") &&
            fs::is_regular_file(Prefix /
                                "toolchain/targets/aarch64-apple-darwin/"
                                "sdk/usr/lib/libSystem.tbd"),
        "The Apple XAR/PBZX/CPIO SDK payload is present");
    require(read(Prefix / "licenses/expat/LICENSE").find("Expat maintainers") !=
                std::string::npos,
            "Expat's original license is installed");
    auto Release = ReleaseExecutable.parent_path();
    require(read(Prefix / "LICENSE") == read(Release / "LICENSE"),
            "The original llm license is preserved");
    for (const auto &Entry :
         fs::recursive_directory_iterator(Release / "licenses"))
      if (Entry.is_regular_file()) {
        auto Relative = Entry.path().lexically_relative(Release);
        require(read(Prefix / Relative) == read(Entry.path()),
                "Release license and notice files are preserved byte for byte");
      }
    require(!fs::exists(Prefix / "licenses/windows-runtime-LICENSE.txt") &&
                fs::is_regular_file(Prefix / "toolchain/sdk-sources.txt") &&
                fs::is_regular_file(Prefix /
                                    "toolchain/targets/"
                                    "aarch64-apple-darwin/sdk-sources.txt"),
            "SDK catalogs are separate from the original release licenses");
    require(Workspace.run(Operation::CheckInstallation, {}) == 0,
            "The staged installation passes its complete installation audit");
    require(Workspace.run(Operation::Uninstall, {}) == 0 && !fs::exists(Prefix),
            "Uninstall removes only the dedicated contract release");
    if (Original) {
      fs::create_symlink(*Original, CommandPath);
      require(fs::read_symlink(CommandPath) == *Original,
              "The previous installed command is restored");
    }
    Guard.Active = false;
    std::cout << "Mac SDK installation contract passed.\n";
    return 0;
  }
#endif
  if ((Argc == 4 || Argc == 5) &&
      std::string_view(Argv[1]) == "--debug-contract") {
    Fixture Workspace(pathFromText(Argv[2]), pathFromText(Argv[3]));
    if (Argc == 5) {
      auto Target = parseTarget(Argv[4]);
      require(bool(Target), "A supported target is required");
      Workspace.BuildTarget = *Target;
    }
    std::string Environment;
    for (const char *Name :
         {"PATH", "PYTHONHOME", "LD_LIBRARY_PATH", "PYTHONDONTWRITEBYTECODE"}) {
      auto Value = std::getenv(Name);
      Environment += Value ? '1' : '0';
      if (Value)
        Environment += Value;
      Environment += '\0';
    }
    for (bool Cxx : {false, true}) {
      fs::remove(Workspace.Root / "result");
      auto Project = Workspace.Root / (Cxx ? "cpp" : "c");
      write(Project / ".llm", "type = 'executable'\nname = 'debug-probe'\n");
      auto Source = Project / (Cxx ? "main.cpp" : "main.c");
      const std::string Function =
          "__attribute__((noinline)) int inspect_value(int input) {\n"
          "  int answer = input + 2;\n  return answer;\n}\n";
      if (Cxx)
        write(Project / "inspect.cppm",
              "export module probe;\nexport " + Function);
      write(Source,
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "volatile int keep_running = 1;\n" +
                (Cxx ? "import probe;\n" : Function) +
                "int main(int argc, char **argv) {\n"
                "  if(argc != 2 || strcmp(argv[1], \"two words\")) return 7;\n"
                "  volatile int result = inspect_value(argc + 40);\n"
                "  while(keep_running) {}\n"
                "  FILE *f = fopen(\"result\", \"wb\");\n"
                "  if(!f) return 8;\n"
                "  const char *names[] = "
                "{\"PATH\",\"PYTHONHOME\",\"LD_LIBRARY_PATH\","
                "\"PYTHONDONTWRITEBYTECODE\"};\n"
                "  for(unsigned i=0;i<4;++i){const char *v=getenv(names[i]);\n"
                "    fputc(v?'1':'0',f); if(v)fputs(v,f); fputc(0,f);}\n"
                "  fclose(f); return result == 44 ? 0 : 9;\n}\n");
      require(Workspace.run(Operation::Debug, Project, {"two words"}, {},
                            Profile::Debug) == 0 &&
                  read(Workspace.Root / "result") == Environment,
              "Debug preserves program arguments, environment and invocation "
              "directory");
      write(Source, "#error intentional debug build failure\n");
      require(
          Workspace.run(Operation::Debug, Project, {}, {}, Profile::Debug) != 0,
          "Debug does not start an old executable after a failed build");
    }
    Command Request;
    Request.Action = Operation::Debug;
    Request.Project = Workspace.Root / "c";
    Request.Mode = Profile::Release;
    auto Release = execute(Workspace.Call, Request);
    require(!Release && Release.error().Message.contains("Debug"),
            "Debug rejects the Release profile");
    Request.Mode = Profile::Debug;
    Request.BuildTarget = nativeTarget() == Target::WindowsX64
                              ? Target::WindowsArm64
                              : Target::WindowsX64;
    auto Foreign = execute(Workspace.Call, Request);
    require(!Foreign && Foreign.error().Message.contains("native target"),
            "Debug rejects a foreign target before building");
    std::cout << "Debug contract passed.\n";
    return 0;
  }
  if (Argc == 4 && std::string_view(Argv[1]) == "--generate") {
    write(pathFromText(Argv[3]),
          "#define VALUE " + read(pathFromText(Argv[2])));
    return 0;
  }
  if (Argc == 6 && std::string_view(Argv[1]) == "--self-host") {
    Fixture Workspace(pathFromText(Argv[2]), pathFromText(Argv[3]));
    Workspace.Call.UserDirectory.clear();
    auto Target = parseTarget(Argv[5]);
    require(bool(Target), "A known target is required");
    Command Request;
    Request.Action = Operation::Build;
    Request.Project = fs::canonical(pathFromText(Argv[4]));
    Request.BuildTarget = *Target;
    Request.Mode = Profile::Release;
    auto Result = execute(Workspace.Call, Request);
    if (!Result)
      throw std::runtime_error(Result.error().Message + '\n' +
                               Workspace.Output);
    require(*Result == 0, "The target build succeeds");
    std::cout << "Target build passed.\n";
    return 0;
  }
  if (Argc == 5 && std::string_view(Argv[1]) == "--self-host") {
    Fixture Workspace(pathFromText(Argv[2]), pathFromText(Argv[3]));
    require(Workspace.run(Operation::Rebuild, pathFromText(Argv[4])) == 0 &&
                Workspace.Work > 0,
            "Alpha self-host build succeeds");
    require(Workspace.run(Operation::Build, pathFromText(Argv[4])) == 0 &&
                Workspace.Work == 0,
            "The self-hosted graph is current without restarting tools");
    std::cout << "Alpha self-host build passed.\n";
    return 0;
  }
  if (Argc != 3 && Argc != 4)
    throw std::runtime_error(
        "Integration test arguments: TOOLCHAIN ALPHA_EXECUTABLE");
  Fixture Workspace(pathFromText(Argv[1]), pathFromText(Argv[2]));
  if (Argc == 4) {
    auto Target = parseTarget(Argv[3]);
    require(bool(Target), "A supported target is required");
    Workspace.BuildTarget = *Target;
  }
  Workspace.GeneratorProgram = fs::canonical(pathFromText(Argv[0]));
  buildAndPackageContract(Workspace);
  moduleAndTestContract(Workspace);
  generatorContract(Workspace);
  std::cout << "Runtime contract checks passed.\n";
  return 0;
} catch (const std::exception &Error) {
  std::cerr << Error.what() << '\n';
  return 1;
}
