module;

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

module llm.cli;

import llm.runtime;
import std;

namespace llm {
namespace {
struct ParseError : std::runtime_error {
  int Code = 1;
  explicit ParseError(std::string Message, int Code = 1)
      : std::runtime_error(std::move(Message)), Code(Code) {}
};

constexpr std::array Commands = {std::pair{"new", Operation::New},
                                 std::pair{"build", Operation::Build},
                                 std::pair{"run", Operation::Run},
                                 std::pair{"debug", Operation::Debug},
                                 std::pair{"check", Operation::Check},
                                 std::pair{"rebuild", Operation::Rebuild},
                                 std::pair{"clean", Operation::Clean},
                                 std::pair{"add", Operation::Add},
                                 std::pair{"remove", Operation::Remove},
                                 std::pair{"update", Operation::Update},
                                 std::pair{"login", Operation::Login},
                                 std::pair{"logout", Operation::Logout},
                                 std::pair{"test", Operation::Test},
                                 std::pair{"format", Operation::Format},
                                 std::pair{"analyze", Operation::Analyze},
                                 std::pair{"list", Operation::List},
                                 std::pair{"package", Operation::Package}};

ColorMode parseColor(std::string_view Text) {
  if (Text == "auto")
    return ColorMode::Automatic;
  if (Text == "always")
    return ColorMode::Always;
  if (Text == "never")
    return ColorMode::Never;
  throw ParseError("--color requires auto, always or never");
}

bool useColor(const Presentation &Output) {
  if (Output.Color == ColorMode::Never)
    return false;
  bool Forced = Output.Color == ColorMode::Always;
  if (!Forced && std::getenv("NO_COLOR"))
    return false;
  if (!Forced && std::getenv("TERM") &&
      std::string_view(std::getenv("TERM")) == "dumb")
    return false;
#ifdef _WIN32
  DWORD Mode = 0;
  auto Stream = GetStdHandle(STD_ERROR_HANDLE);
  bool Console =
      GetConsoleMode(Stream, &Mode) &&
      ((Mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
       SetConsoleMode(Stream, Mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING));
  return Forced || Console;
#else
  return Forced || isatty(STDERR_FILENO);
#endif
}

std::string plainDiagnostics(std::string_view Text) {
  std::string Result;
  Result.reserve(Text.size());
  for (std::size_t Position = 0; Position < Text.size();) {
    if (Text[Position] == '\x1b' && Position + 1 < Text.size()) {
      if (Text[Position + 1] == '[') {
        auto End = Position + 2;
        while (End < Text.size() && !(Text[End] >= '@' && Text[End] <= '~'))
          ++End;
        if (End < Text.size()) {
          Position = End + 1;
          continue;
        }
      } else if (Text[Position + 1] == ']') {
        auto End = Position + 2;
        while (End < Text.size() && Text[End] != '\a' &&
               !(Text[End] == '\x1b' && End + 1 < Text.size() &&
                 Text[End + 1] == '\\'))
          ++End;
        if (End < Text.size()) {
          Position = End + (Text[End] == '\a' ? 1 : 2);
          continue;
        }
      }
    }
    Result += Text[Position++];
  }
  return Result;
}

void diagnostic(const Presentation &Output, std::string_view Level,
                std::string_view Color, std::string_view Text,
                const SourceLocation &Location = {}) {
  if (Location.File.empty())
    std::cerr << "llm: ";
  else
    std::cerr << pathText(Location.File) << ':' << Location.Line << ':'
              << Location.Column << ": ";
  bool Colored = useColor(Output);
  if (Colored)
    std::cerr << Color;
  std::cerr << Level << ':';
  if (Colored)
    std::cerr << "\x1b[0m";
  std::cerr << ' ' << Text << '\n' << std::flush;
}

void printEvent(const Presentation &Output, const Event &Value) {
  switch (Value.Kind) {
  case EventKind::Warning:
    diagnostic(Output, "warning", "\x1b[1;33m", Value.Text);
    break;
  case EventKind::Error:
    diagnostic(Output, "error", "\x1b[1;31m", Value.Text);
    break;
  case EventKind::Note:
    diagnostic(Output, "note", "\x1b[1;36m", Value.Text);
    break;
  case EventKind::StandardOutput:
    std::cout << Value.Text << std::flush;
    break;
  case EventKind::ToolOutput: {
#ifdef _WIN32
    std::cerr << std::flush;
    auto Previous = _setmode(_fileno(stderr), _O_BINARY);
    struct Restore {
      int Mode;
      ~Restore() {
        if (Mode != -1)
          _setmode(_fileno(stderr), Mode);
      }
    } Guard{Previous};
#endif
    std::cerr << (useColor(Output) ? Value.Text : plainDiagnostics(Value.Text))
              << std::flush;
    break;
  }
  case EventKind::Progress:
    if (!Output.Quiet) {
      std::cerr << Value.Text << '\n' << std::flush;
    }
    break;
  }
}

void globalHelp() {
  std::cout
      << "LLVM Make - C, C++ and Assembly builds\n\n"
         "Usage: llm <command> [options]\n\n"
         "Commands:\n"
         "  new [path]       Create an empty project\n"
         "  build            Build the selected product\n"
         "  rebuild          Rebuild the selected profile\n"
         "  run              Build and run the application\n"
         "  debug            Build Debug and open LLDB\n"
         "  test [SUITE]     Build and run explicit test projects\n"
         "  check            Compile without linking\n"
         "  analyze          Analyze C and C++ sources\n"
         "  format           Format project sources\n"
         "  list             Show project information and dependencies\n"
         "  clean            Remove owned build outputs\n"
         "  package          Create a tar.gz distribution archive\n"
         "  add DEP          Add or change a direct dependency\n"
         "  remove DEP       Remove a direct dependency\n"
         "  update           Update permitted dependency versions\n"
         "  login REGISTRY   Validate and save registry credentials\n"
         "  logout REGISTRY  Remove saved registry credentials\n\n"
         "Global options:\n"
         "  --install             Install host tools and the native target\n"
         "  --global              Select the system-wide installation\n"
         "  --path PATH            Choose an installation directory\n"
         "  --uninstall           Remove this release and preserve other "
         "versions "
         "and caches\n"
         "  --check-installation  Check the installed host tools and target\n"
         "  -t, --target TRIPLE   Install, check or remove an optional target\n"
         "  --version             Show the llm version. Add --verbose for "
         "components\n"
         "  -v, --verbose         Show detailed commands or component "
         "information\n"
         "  -q, --quiet           Suppress progress and success messages\n"
         "  --color MODE          Diagnostic color: auto, always or never\n"
         "  -h, --help            Show this help\n\n"
         "Use llm <command> --help for command arguments and options.\n"
         "Build commands use the private toolchain and resolve declared "
         "dependencies automatically.\n"
         "Installation defaults to the current user.\n"
         "Use --install --target TRIPLE to add a target SDK explicitly.\n"
         "Foreign targets support build, rebuild, check, package and list.\n";
#ifndef _WIN32
  std::cout << "System installation requires sudo ./llm --install --global.\n"
               "Use --path PATH for a user-writable installation directory.\n";
#endif
}

bool builds(Operation Action) {
  return Action == Operation::Build || Action == Operation::Rebuild ||
         Action == Operation::Run || Action == Operation::Debug ||
         Action == Operation::Check || Action == Operation::Test ||
         Action == Operation::Analyze || Action == Operation::Package;
}

void commandHelp(std::string_view Name, Operation Action) {
  std::cout << "Usage: llm " << Name;
  if (Action == Operation::New)
    std::cout << " [path] [--type TYPE]";
  else if (Action == Operation::Test)
    std::cout << " [SUITE] [options] [-- arguments...]";
  else if (Action == Operation::Run || Action == Operation::Debug)
    std::cout << " [options] [-- arguments...]";
  else if (Action == Operation::Format)
    std::cout << " [PATH...] [options]";
  else if (Action == Operation::Add || Action == Operation::Remove)
    std::cout << " DEP [options]";
  else if (Action == Operation::Login || Action == Operation::Logout)
    std::cout << " REGISTRY [options]";
  else
    std::cout << " [options]";
  std::cout << '\n';
  if (Action == Operation::New) {
    std::cout
        << "Create .llm, src/ and a new .gitignore while preserving "
           "existing files.\n"
           "Types: executable (default), static-library, shared-library,\n"
           "       header-only-library, test.\n";
    return;
  }
  std::cout
      << "\n  -p, --project PATH    Select the directory containing .llm\n"
         "  -v, --verbose         Show commands and details\n"
         "  -q, --quiet           Suppress progress and success messages\n"
         "      --color MODE      auto, always or never\n"
         "  -h, --help            Show command help\n";
  if (builds(Action) || Action == Operation::List)
    std::cout << "  -t, --target TRIPLE   LLVM target (native by default)\n";
  if (Action != Operation::Debug &&
      (builds(Action) || Action == Operation::List))
    std::cout << "  "
                 "-r, --release  "
                 "       Select Release instead of Debug\n";
  if (builds(Action))
    std::cout << "  -j, --jobs N          Maximum parallel jobs\n";
  if (Action == Operation::Run || Action == Operation::Test)
    std::cout << "      --no-build        Use existing outputs\n";
  if (Action == Operation::Test)
    std::cout << "      --all             Select all associated suites\n      "
                 "--list            List available suites without building\n"
                 "      --filter GLOB     Filter selected suite paths\n      "
                 "--timeout SECONDS Override per-suite timeout\n";
  if (Action == Operation::List)
    std::cout << "      --json            Emit JSON\n";
  if (Action == Operation::Format)
    std::cout << "      --check           Check formatting without editing\n";
  if (Action == Operation::Analyze)
    std::cout << "      --tests           Include associated suites\n      "
                 "--checks GLOBS     clang-tidy check selection\n"
                 "      --warnings-as-errors GLOBS\n";
  if (Action == Operation::Add)
    std::cout << "      --public          Add a public library dependency\n";
}

int runCommand(Context &Call, const Strings &Arguments) {
  auto Found = std::ranges::find_if(Commands, [&](const auto &Entry) {
    return Entry.first == std::string_view(Arguments[1]);
  });
  if (Found == Commands.end())
    throw ParseError("Unknown command '" + Arguments[1] + "'", 2);
  Command Request;
  Request.Action = Found->second;
  bool HasPath = false;
  for (std::size_t Index = 2; Index < Arguments.size(); ++Index) {
    const auto &Argument = Arguments[Index];
    if (Argument == "--")
      break;
    if (Argument == "--help" || Argument == "-h") {
      commandHelp(Found->first, Request.Action);
      return 0;
    }
  }
  for (std::size_t Index = 2; Index < Arguments.size(); ++Index) {
    const auto &Argument = Arguments[Index];
    if (Argument == "--") {
      if (Request.Action == Operation::Run ||
          Request.Action == Operation::Debug ||
          Request.Action == Operation::Test)
        Request.Arguments.assign(Arguments.begin() + Index + 1,
                                 Arguments.end());
      else if (Request.Action == Operation::Format)
        for (++Index; Index < Arguments.size(); ++Index)
          Request.FormatPaths.push_back(pathFromText(Arguments[Index]));
      else
        throw ParseError("Unexpected argument separator");
      break;
    }
    auto Equal = Argument.find('=');
    auto Key = Argument.substr(0, Equal);
    auto Value = [&]() -> std::string {
      std::string Result;
      if (Equal != std::string::npos)
        Result = Argument.substr(Equal + 1);
      else {
        if (++Index == Arguments.size())
          throw ParseError("Missing value for " + Key);
        Result = Arguments[Index];
      }
      if (Result.empty())
        throw ParseError("Missing value for " + Key);
      return Result;
    };
    if (Request.Action == Operation::New) {
      if (Key == "--type") {
        auto Type = parseProductType(Value());
        if (!Type)
          throw ParseError(Type.error().Message);
        Request.NewType = *Type;
      } else if (!Argument.empty() && Argument.front() != '-' && !HasPath) {
        Request.Project = pathFromText(Argument);
        HasPath = true;
      } else
        throw ParseError("Usage: llm new [path] [--type TYPE]");
      continue;
    }
    if (Key == "--project" || Key == "-p")
      Request.Project = pathFromText(Value());
    else if (Argument == "--verbose" || Argument == "-v")
      Call.Output.Verbose = true;
    else if (Argument == "--quiet" || Argument == "-q")
      Call.Output.Quiet = true;
    else if (Key == "--color")
      Call.Output.Color = parseColor(Value());
    else if ((builds(Request.Action) || Request.Action == Operation::List) &&
             (Key == "--target" || Key == "-t")) {
      auto Target = parseTarget(Value());
      if (!Target)
        throw ParseError(Target.error().Message);
      Request.BuildTarget = *Target;
      Request.TargetExplicit = true;
    } else if (Request.Action == Operation::Debug &&
               (Argument == "--release" || Argument == "-r"))
      throw ParseError("debug requires the Debug profile");
    else if ((builds(Request.Action) || Request.Action == Operation::List) &&
             (Argument == "--release" || Argument == "-r"))
      Request.Mode = Profile::Release;
    else if (builds(Request.Action) && (Key == "--jobs" || Key == "-j")) {
      auto Text = Value();
      auto [End, Error] =
          std::from_chars(Text.data(), Text.data() + Text.size(), Request.Jobs);
      if (Error != std::errc{} || End != Text.data() + Text.size() ||
          Request.Jobs < 1)
        throw ParseError("--jobs requires a positive integer");
      Request.JobsExplicit = true;
    } else if ((Request.Action == Operation::Run ||
                Request.Action == Operation::Test) &&
               Argument == "--no-build")
      Request.NoBuild = true;
    else if (Request.Action == Operation::Test && Argument == "--all")
      Request.All = true;
    else if (Request.Action == Operation::Test && Argument == "--list")
      Request.ListOnly = true;
    else if (Request.Action == Operation::Test && Key == "--filter")
      Request.Filter = Value();
    else if (Request.Action == Operation::Test && Key == "--timeout") {
      auto Text = Value();
      double Seconds = 0;
      auto [End, Error] =
          std::from_chars(Text.data(), Text.data() + Text.size(), Seconds);
      if (Error != std::errc{} || End != Text.data() + Text.size() ||
          !std::isfinite(Seconds) || Seconds < 0.001 ||
          Seconds > 9223372036854775.0)
        throw ParseError(
            "--timeout requires positive finite seconds (at least 0.001)");
      Request.Timeout = Seconds;
    } else if (Request.Action == Operation::List && Argument == "--json")
      Request.Json = true;
    else if (Request.Action == Operation::Format && Argument == "--check")
      Request.FormatCheck = true;
    else if (Request.Action == Operation::Analyze && Argument == "--tests")
      Request.IncludeTests = true;
    else if (Request.Action == Operation::Analyze && Key == "--checks")
      Request.Checks = Value();
    else if (Request.Action == Operation::Analyze &&
             Key == "--warnings-as-errors")
      Request.WarningsAsErrors = Value();
    else if (Request.Action == Operation::Add && Argument == "--public")
      Request.Public = true;
    else if (!Argument.empty() && Argument.front() != '-' &&
             Request.Action == Operation::Format)
      Request.FormatPaths.push_back(pathFromText(Argument));
    else if (!Argument.empty() && Argument.front() != '-' &&
             Request.Selection.empty() &&
             (Request.Action == Operation::Test ||
              Request.Action == Operation::Add ||
              Request.Action == Operation::Remove ||
              Request.Action == Operation::Login ||
              Request.Action == Operation::Logout))
      Request.Selection = Argument;
    else
      throw ParseError("Unknown option or argument for " +
                       std::string(Found->first) + ": " + Argument);
  }
  if (Call.Output.Quiet && Call.Output.Verbose)
    throw ParseError("--quiet and --verbose cannot be used together");
  auto Outcome = execute(Call, Request);
  if (!Outcome) {
    diagnostic(Call.Output, "error", "\x1b[1;31m", Outcome.error().Message,
               Outcome.error().Location);
    for (const auto &Note : Outcome.error().Notes)
      diagnostic(Call.Output, "note", "\x1b[1;36m", Note);
    return 1;
  }
  return *Outcome;
}
}

int runCli(std::vector<std::string> Arguments, std::string_view Version) {
  Context Call;
  Call.Version = Version;
  Call.InvocationDirectory = std::filesystem::current_path();
  Call.Emit = [&](const Event &Value) { printEvent(Call.Output, Value); };
  if (Arguments.size() >= 2 && Arguments[1].starts_with("--internal-")) {
    auto Outcome = runInternal(Call, Arguments);
    if (Outcome)
      return *Outcome;
    diagnostic(Call.Output, "error", "\x1b[1;31m", Outcome.error().Message,
               Outcome.error().Location);
    return 1;
  }
  try {
    if (Arguments.size() >= 2 && !Arguments[1].starts_with('-'))
      return runCommand(Call, Arguments);
    std::string Global;
    Command Request;
    bool Help = false;
    for (std::size_t Index = 1; Index < Arguments.size(); ++Index) {
      const auto &Argument = Arguments[Index];
      if (Argument == "-v" || Argument == "--verbose")
        Call.Output.Verbose = true;
      else if (Argument == "-q" || Argument == "--quiet")
        Call.Output.Quiet = true;
      else if (Argument.starts_with("--color="))
        Call.Output.Color = parseColor(Argument.substr(8));
      else if (Argument == "--color") {
        if (++Index == Arguments.size())
          throw ParseError("Missing value for --color");
        Call.Output.Color = parseColor(Arguments[Index]);
      } else if (Argument == "--target" || Argument == "-t" ||
                 Argument.starts_with("--target=") ||
                 Argument.starts_with("-t=")) {
        auto Equal = Argument.find('=');
        std::string Text;
        if (Equal != std::string::npos)
          Text = Argument.substr(Equal + 1);
        else {
          if (++Index == Arguments.size())
            throw ParseError("Missing value for --target");
          Text = Arguments[Index];
        }
        if (Text.empty())
          throw ParseError("Missing value for --target");
        auto Target = parseTarget(Text);
        if (!Target)
          throw ParseError(Target.error().Message);
        Request.BuildTarget = *Target;
        Request.TargetExplicit = true;
      } else if (Argument == "--global") {
        if (Request.Global)
          throw ParseError("--global may only be specified once");
        Request.Global = true;
      } else if (Argument == "--path" || Argument.starts_with("--path=")) {
        if (Request.InstallPath)
          throw ParseError("--path may only be specified once");
        std::string Text;
        if (Argument.starts_with("--path="))
          Text = Argument.substr(7);
        else {
          if (++Index == Arguments.size())
            throw ParseError("Missing value for --path");
          Text = Arguments[Index];
        }
        if (Text.empty())
          throw ParseError("Missing value for --path");
        Request.InstallPath = pathFromText(Text);
      } else if (Argument == "--help" || Argument == "-h") {
        Help = true;
      } else if (Argument == "--version" || Argument == "--install" ||
                 Argument == "--uninstall" ||
                 Argument == "--check-installation") {
        if (!Global.empty())
          throw ParseError("Choose only one global operation");
        Global = Argument;
      } else
        throw ParseError("Unknown command or option '" + Argument + "'", 2);
    }
    if (Call.Output.Quiet && Call.Output.Verbose)
      throw ParseError("--quiet and --verbose cannot be used together");
    if (Help) {
      globalHelp();
      return 0;
    }
    if (Request.Global && Request.InstallPath)
      throw ParseError("--global and --path cannot be used together");
    if (Request.Global && Global != "--install" && Global != "--uninstall" &&
        Global != "--check-installation")
      throw ParseError("--global requires --install, --uninstall or "
                       "--check-installation");
    if (Request.InstallPath && Global != "--install" &&
        Global != "--uninstall" && Global != "--check-installation")
      throw ParseError("--path requires --install, --uninstall or "
                       "--check-installation");
    if (Request.TargetExplicit && Global != "--install" &&
        Global != "--uninstall" && Global != "--check-installation")
      throw ParseError("--target requires --install, --uninstall or "
                       "--check-installation outside a command");
    if (Global.empty() && Arguments.size() != 1)
      throw ParseError("Choose a command or global operation. Use llm --help.");
    if (Global.empty()) {
      globalHelp();
      return 0;
    }
    if (Global == "--version") {
      std::cout << "llm " << Version << '\n';
      if (Call.Output.Verbose)
        std::cout
            << "Target: " << targetName(nativeTarget())
            << "\nLLVM 23.1.3, LLDB 23.1.3, Ninja 1.13.2, curl 8.23.0, "
               "OpenSSL 3.5.9, libarchive 3.8.9, Expat 2.9.0\nzlib 1.3.2, "
               "xz 5.8.4, zstd 1.5.7, bzip2 1.0.8, toml++ 3.4.0, "
               "nlohmann/json 3.12.0\n";
      return 0;
    }
    Request.Action = Global == "--install"     ? Operation::Install
                     : Global == "--uninstall" ? Operation::Uninstall
                                               : Operation::CheckInstallation;
    auto Outcome = execute(Call, Request);
    if (Outcome)
      return *Outcome;
    diagnostic(Call.Output, "error", "\x1b[1;31m", Outcome.error().Message,
               Outcome.error().Location);
    return 1;
  } catch (const ParseError &Error) {
    diagnostic(Call.Output, "error", "\x1b[1;31m", Error.what());
    diagnostic(Call.Output, "note", "\x1b[1;36m",
               "Use llm --help for available commands and options.");
    return Error.Code;
  }
}

}
