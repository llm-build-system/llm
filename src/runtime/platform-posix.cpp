module;
#include <array>
#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <pwd.h>
#include <sstream>
#include <sys/file.h>
#include <termios.h>
#ifdef __APPLE__
#include <cstdlib>
#include <mach-o/dyld.h>
#endif
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" char **environ;

module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
bool redirectsPath(const Path &File) {
  return fs::is_symlink(fs::symlink_status(File));
}
std::string collisionKey(const Path &File) { return pathText(File); }
using ProcessEnvironment = std::map<std::string, std::string>;

void validateEnvironment(const ProcessEnvironment &Environment) {
  for (const auto &[Name, Value] : Environment)
    if (Name.empty() || Name.find_first_of("=\0", 0, 2) != std::string::npos ||
        Value.find('\0') != std::string::npos)
      throw Failure("Invalid process environment override");
}

namespace {
volatile sig_atomic_t Interrupted = 0;
void onSignal(int) { Interrupted = 1; }
[[noreturn]] void fail(std::string_view Action) {
  throw Failure(std::string(Action) + ": " + std::strerror(errno));
}
struct ProcessStartPipe {
  int Descriptors[2]{-1, -1};
  ProcessStartPipe() {
    if (pipe(Descriptors))
      fail("Create process start pipe");
    for (int &Descriptor : Descriptors) {
      auto ProtectedDescriptor = fcntl(Descriptor, F_DUPFD_CLOEXEC, 3);
      if (ProtectedDescriptor < 0) {
        auto Error = errno;
        close(Descriptors[0]);
        close(Descriptors[1]);
        errno = Error;
        fail("Protect process start pipe");
      }
      close(Descriptor);
      Descriptor = ProtectedDescriptor;
    }
  }
  ~ProcessStartPipe() {
    for (int Descriptor : Descriptors)
      if (Descriptor >= 0)
        close(Descriptor);
  }
  void closeEnd(size_t Index) {
    close(Descriptors[Index]);
    Descriptors[Index] = -1;
  }
};
class TerminalSession {
  pid_t Foreground;
  termios Attributes;
  struct sigaction Interrupt, Background;
  bool Active = true;

public:
  TerminalSession() {
    Foreground = tcgetpgrp(STDIN_FILENO);
    if (Foreground < 0 || tcgetattr(STDIN_FILENO, &Attributes))
      fail("Read foreground terminal");
    if (Foreground != getpgrp())
      throw Failure("debug requires a foreground terminal");
    struct sigaction Ignore{};
    Ignore.sa_handler = SIG_IGN;
    sigemptyset(&Ignore.sa_mask);
    if (sigaction(SIGTTOU, &Ignore, &Background))
      fail("Protect terminal ownership");
    if (sigaction(SIGINT, &Ignore, &Interrupt)) {
      auto Error = errno;
      sigaction(SIGTTOU, &Background, nullptr);
      errno = Error;
      fail("Protect debugger interrupt");
    }
  }
  void restore() {
    int Error = 0;
    if (tcsetpgrp(STDIN_FILENO, Foreground))
      Error = errno;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &Attributes) && !Error)
      Error = errno;
    if (sigaction(SIGINT, &Interrupt, nullptr) && !Error)
      Error = errno;
    if (sigaction(SIGTTOU, &Background, nullptr) && !Error)
      Error = errno;
    Active = false;
    if (Error) {
      errno = Error;
      fail("Restore debugger terminal");
    }
  }
  ~TerminalSession() {
    if (Active) {
      tcsetpgrp(STDIN_FILENO, Foreground);
      tcsetattr(STDIN_FILENO, TCSADRAIN, &Attributes);
      sigaction(SIGINT, &Interrupt, nullptr);
      sigaction(SIGTTOU, &Background, nullptr);
    }
  }
};
enum class ProcessStartStage {
  Group,
  Directory,
  StdoutRedirect,
  InputOpen,
  InputRedirect,
  LogRedirect,
  StdoutCapture,
  Execute
};
struct ProcessStartError {
  ProcessStartStage Stage;
  int Error;
};
int protectProcessDescriptor(int Descriptor) {
  if (Descriptor < 0 || Descriptor >= 3)
    return Descriptor;
  auto ProtectedDescriptor = fcntl(Descriptor, F_DUPFD_CLOEXEC, 3);
  auto Error = errno;
  close(Descriptor);
  errno = Error;
  return ProtectedDescriptor;
}
[[noreturn]] void childStartError(int Pipe, ProcessStartStage Stage, int Error,
                                  int Code = 126) {
  const ProcessStartError Record{Stage, Error};
  const auto *Bytes = reinterpret_cast<const char *>(&Record);
  size_t Written = 0;
  while (Written < sizeof(Record)) {
    auto Count = write(Pipe, Bytes + Written, sizeof(Record) - Written);
    if (Count < 0 && errno == EINTR)
      continue;
    if (Count <= 0)
      break;
    Written += static_cast<size_t>(Count);
  }
  _exit(Code);
}
std::string_view processStartStage(ProcessStartStage Stage) {
  switch (Stage) {
  case ProcessStartStage::Group:
    return "create process group";
  case ProcessStartStage::Directory:
    return "set working directory";
  case ProcessStartStage::StdoutRedirect:
    return "redirect standard output";
  case ProcessStartStage::InputOpen:
    return "open standard input";
  case ProcessStartStage::InputRedirect:
    return "redirect standard input";
  case ProcessStartStage::LogRedirect:
    return "redirect diagnostic output";
  case ProcessStartStage::StdoutCapture:
    return "capture standard output";
  case ProcessStartStage::Execute:
    return "execute program";
  }
  return "prepare process";
}
}

void replaceFile(const fs::path &Source, const fs::path &Destination) {
  fs::rename(Source, Destination);
}
void removeOutputFile(const Path &File) { fs::remove(File); }

fs::path executablePath() {
#ifdef __APPLE__
  std::uint32_t Size = 0;
  _NSGetExecutablePath(nullptr, &Size);
  std::vector<char> Buffer(Size);
  if (_NSGetExecutablePath(Buffer.data(), &Size))
    throw Failure("Find llm executable");
  return fs::canonical(Buffer.data());
#else
  std::array<char, 65536> Buffer{};
  auto N = readlink("/proc/self/exe", Buffer.data(), Buffer.size());
  if (N <= 0 || static_cast<size_t>(N) == Buffer.size())
    fail("Find llm executable");
  return std::string(Buffer.data(), N);
#endif
}
InstallLayout systemLayout() {
#ifdef __APPLE__
  return {"/usr/local/lib/llm", "/Library/Application Support/llm",
          "/usr/local/bin/llm", true, "/Library/Caches/llm"};
#else
  return {"/opt/llm", "/var/opt/llm", "/usr/local/bin/llm", true};
#endif
}
bool elevated() { return geteuid() == 0; }
InstallLayout userLayout() {
#ifdef __APPLE__
  auto Root = userHome() / "Library/Application Support/llm";
#else
  auto Root = userHome() / ".local/share/llm";
#endif
  return {Root, Root, Root / "bin/llm", false};
}
int elevateOperation(Context &Call, std::string_view Operation,
                     std::optional<Target> BuildTarget) {
  throw Failure("System operation requires root. Run: sudo " +
                quoteArgument(pathText(Call.Executable)) + " " +
                std::string(Operation) + " --global" +
                (BuildTarget
                     ? " --target " + std::string(targetName(*BuildTarget))
                     : ""));
}
void enableCancellation() {
#ifdef __APPLE__
  if (!std::setlocale(LC_CTYPE, "en_US.UTF-8"))
    throw Failure("The system needs a UTF-8 locale for package filenames");
#else
  if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
    throw Failure("The system needs the C.UTF-8 locale for package filenames");
#endif
  struct sigaction Action{};
  Action.sa_handler = onSignal;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGINT, &Action, nullptr) ||
      sigaction(SIGTERM, &Action, nullptr))
    fail("Install cancellation handler");
}
bool interrupted() { return Interrupted; }
void checkInterrupted() {
  if (interrupted())
    throw Failure("Operation cancelled");
}
std::string quoteArgument(std::string_view Argument) {
  std::string Result = "'";
  for (char Ch : Argument)
    Result += Ch == '\'' ? "'\\''" : std::string(1, Ch);
  return Result + "'";
}
struct FileLock::Impl {
  int File = -1;
  explicit Impl(Context &Call, const fs::path &Path, bool Wait) {
    File = open(Path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (File < 0)
      fail("Open installation lock");
    try {
      while (flock(File, LOCK_EX | LOCK_NB)) {
        if (!Wait || (errno != EWOULDBLOCK && errno != EAGAIN))
          fail("Acquire installation lock");
        cancelled(Call);
        usleep(50000);
      }
    } catch (...) {
      close(File);
      throw;
    }
  }
  ~Impl() { close(File); }
};
FileLock::FileLock(Context &Call, const fs::path &Path, bool Wait)
    : State(std::make_unique<Impl>(Call, Path, Wait)) {}
FileLock::~FileLock() = default;
void rejectSymlinkAncestors(const fs::path &Path) {
  if (!Path.is_absolute())
    throw Failure("Installation path must be absolute");
  fs::path Current;
  for (const auto &Part : Path.lexically_normal()) {
    Current /= Part;
    if (fs::is_symlink(fs::symlink_status(Current)))
      throw Failure(
          "Symbolic links are not allowed in installation ancestors: " +
          pathText(Current));
  }
}
void secureDataDirectory(const fs::path &Directory, bool System) {
  if (!Directory.is_absolute())
    throw Failure("Installation path must be absolute");
  std::vector<fs::path> Missing;
  for (auto Path = Directory; !fs::exists(Path); Path = Path.parent_path())
    Missing.push_back(Path);
  for (auto It = Missing.rbegin(); It != Missing.rend(); ++It) {
    fs::create_directory(*It);
    if (System)
      fs::permissions(*It, static_cast<fs::perms>(0755));
  }
  if (System) {
    struct stat Status{};
    if (lstat(Directory.c_str(), &Status))
      fail("Inspect installation directory");
    if (Status.st_uid != 0 || !S_ISDIR(Status.st_mode) ||
        (Status.st_mode & 0022))
      throw Failure("System installation directory must be root-owned and not "
                    "writable by other users: " +
                    pathText(Directory));
    if (chmod(Directory.c_str(), 0755))
      fail("Set installation directory permissions");
  }
}

fs::path userHome() {
  auto User = getpwuid(getuid());
  if (!User || !User->pw_dir || !*User->pw_dir)
    throw Failure("Cannot determine user home");
  return fs::canonical(pathFromText(User->pw_dir));
}
void protectPrivatePath(const fs::path &Path, bool Directory) {
  rejectSymlinkAncestors(Path);
  struct stat Status{};
  if (lstat(Path.c_str(), &Status) || Status.st_uid != getuid() ||
      (Directory ? !S_ISDIR(Status.st_mode) : !S_ISREG(Status.st_mode)))
    throw Failure("Private path is not owned by the current user");
  if (chmod(Path.c_str(), Directory ? 0700 : 0600))
    fail("Set private permissions");
}
void checkPrivateFile(const fs::path &Path) {
  rejectSymlinkAncestors(Path);
  struct stat Status{};
  if (lstat(Path.c_str(), &Status) || Status.st_uid != getuid() ||
      !S_ISREG(Status.st_mode) || (Status.st_mode & 0077))
    throw Failure(
        "Credential store must be a private current-user file (0600)");
}
std::string readSecret(std::string_view Prompt) {
  termios Original{};
  if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &Original))
    throw Failure("login requires a terminal. Provision a private credential "
                  "store for CI");
  auto Hidden = Original;
  Hidden.c_lflag &= ~ECHO;
  std::cerr << Prompt << std::flush;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Hidden))
    fail("Disable terminal echo");
  struct Restore {
    termios Mode;
    ~Restore() {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Mode);
      std::cerr << '\n';
    }
  } Restore{Original};
  std::string Result;
  for (;;) {
    int Byte = std::cin.get();
    if (Byte < 0 || Byte == '\n')
      break;
    if (Result.size() >= 16384)
      throw Failure("Registry token exceeds size limit");
    Result += static_cast<char>(Byte);
  }
  if (!Result.empty() && Result.back() == '\r')
    Result.pop_back();
  return Result;
}
namespace {
std::string pathStatement(const Path &Directory, bool Fish) {
  auto Text = pathText(Directory);
  if (Text.find_first_of(":\r\n") != std::string::npos)
    throw Failure("PATH directories cannot contain colons or line breaks");
  auto Quoted = quoteArgument(Text);
  if (Fish)
    return "if not contains -- " + Quoted + " $PATH; set -gx PATH " + Quoted +
           " $PATH; end";
  return "case \":${PATH-}:\" in *:" + Quoted +
         ":*) ;; *) export PATH=" + Quoted + "${PATH:+\":$PATH\"} ;; esac";
}
std::vector<PathEntry> shellEntries(const InstallLayout &Layout) {
  if (Layout.System)
    return {};
  auto User = getpwuid(getuid());
  if (!User || !User->pw_shell || !*User->pw_shell)
    throw Failure("Cannot determine the account's login shell");
  auto Shell = Path(User->pw_shell).filename();
  auto Home = userHome();
  std::vector<Path> Files;
  bool Fish = false;
  if (Shell == "bash") {
    Files.push_back(Home / ".bashrc");
    Path Login = Home / ".profile";
    for (const auto *Name : {".bash_profile", ".bash_login", ".profile"}) {
      auto File = Home / Name;
      if (fs::exists(fs::symlink_status(File))) {
        Login = File;
        break;
      }
    }
    Files.push_back(Login);
  } else if (Shell == "zsh") {
    auto Directory = std::getenv("ZDOTDIR");
    auto Root = Directory ? pathFromText(Directory) : Home;
    if (!Root.is_absolute())
      throw Failure("ZDOTDIR must be an absolute directory");
    Files.push_back(Root / ".zshrc");
  } else if (Shell == "fish") {
    auto Directory = std::getenv("XDG_CONFIG_HOME");
    auto Root =
        Directory && *Directory ? pathFromText(Directory) : Home / ".config";
    if (!Root.is_absolute())
      throw Failure("XDG_CONFIG_HOME must be an absolute directory");
    Files.push_back(Root / "fish/config.fish");
    Fish = true;
  } else
    throw Failure("Automatic PATH setup does not support login shell: " +
                  pathText(Shell));
  std::vector<PathEntry> Result;
  auto Text = pathStatement(Layout.CommandPath.parent_path(), Fish);
  for (auto File : Files) {
    if (fs::is_symlink(fs::symlink_status(File)))
      File = fs::canonical(File);
    else
      File = fs::weakly_canonical(File);
    if (pathText(File).find_first_of("\r\n") != std::string::npos)
      throw Failure("Shell configuration paths cannot contain line breaks");
    if (std::ranges::none_of(
            Result, [&](const auto &Entry) { return Entry.File == File; }))
      Result.push_back({File, Text});
  }
  return Result;
}
std::size_t statementPosition(std::string_view Text,
                              std::string_view Statement) {
  for (auto Position = Text.find(Statement); Position != Text.npos;
       Position = Text.find(Statement, Position + 1)) {
    auto End = Position + Statement.size();
    if ((Position == 0 || Text[Position - 1] == '\n') &&
        (End == Text.size() || Text[End] == '\n' ||
         (Text[End] == '\r' && End + 1 < Text.size() && Text[End + 1] == '\n')))
      return Position;
  }
  return Text.npos;
}
void editShellFile(const Path &File, bool Add,
                   const std::function<bool(std::string &, bool)> &Edit) {
  auto Status = fs::symlink_status(File);
  bool Exists = fs::exists(Status);
  if (!Exists && !Add)
    return;
  if (Exists && !fs::is_regular_file(Status))
    throw Failure("Shell configuration must be a regular file: " +
                  pathText(File));
  if (!Exists)
    fs::create_directories(File.parent_path());
  int Flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;
  if (!Exists)
    Flags |= O_CREAT | O_EXCL;
  int Descriptor = open(File.c_str(), Flags, 0600);
  if (Descriptor < 0)
    fail("Open shell configuration");
  struct Close {
    int File;
    ~Close() { close(File); }
  } Guard{Descriptor};
  if (flock(Descriptor, LOCK_EX | LOCK_NB))
    fail("Lock shell configuration");
  struct stat Details{};
  if (fstat(Descriptor, &Details))
    fail("Inspect shell configuration");
  if (Details.st_uid != getuid() || !S_ISREG(Details.st_mode) ||
      Details.st_size < 0 || Details.st_size > 16777216)
    throw Failure("Shell configuration must be owned by the current user and "
                  "smaller than 16 MiB");
  std::string Text(static_cast<std::size_t>(Details.st_size), '\0');
  for (std::size_t Position = 0; Position < Text.size();) {
    auto Count =
        read(Descriptor, Text.data() + Position, Text.size() - Position);
    if (Count <= 0)
      fail("Read shell configuration");
    Position += Count;
  }
  if (!Edit(Text, Exists))
    return;
  for (std::size_t Position = 0; Position < Text.size();) {
    auto Count = pwrite(Descriptor, Text.data() + Position,
                        Text.size() - Position, Position);
    if (Count <= 0)
      fail("Write shell configuration");
    Position += Count;
  }
  if (ftruncate(Descriptor, Text.size()) || fsync(Descriptor))
    fail("Save shell configuration");
}
}
void validateCommandRegistration(const InstallLayout &Layout) {
  for (const auto &Entry : shellEntries(Layout)) {
    struct stat Status{};
    if (lstat(Entry.File.c_str(), &Status) == 0) {
      if (!S_ISREG(Status.st_mode) || Status.st_uid != getuid())
        throw Failure("Shell configuration is not a current-user file: " +
                      pathText(Entry.File));
      if (access(Entry.File.c_str(), R_OK | W_OK))
        fail("Shell configuration requires read and write access");
    } else if (errno != ENOENT)
      fail("Inspect shell configuration");
  }
}
std::vector<PathEntry> registerCommand(const InstallLayout &Layout,
                                       std::span<const PathEntry> Previous) {
  std::vector<PathEntry> Result;
  auto Posix = pathStatement(Layout.CommandPath.parent_path(), false);
  auto Fish = pathStatement(Layout.CommandPath.parent_path(), true);
  for (const auto &Entry : Previous) {
    if (!Entry.File.is_absolute() ||
        (Entry.Text != Posix && Entry.Text != Fish))
      throw Failure("Invalid installed shell PATH entry");
    if (std::ranges::none_of(Result, [&](const auto &Old) {
          return Old.File == Entry.File && Old.Text == Entry.Text;
        }))
      Result.push_back(Entry);
  }
  std::vector<PathEntry> Added;
  try {
    for (auto Entry : shellEntries(Layout)) {
      editShellFile(Entry.File, true, [&](std::string &Text, bool Exists) {
        if (statementPosition(Text, Entry.Text) != Text.npos)
          return false;
        std::erase_if(Result, [&](const auto &Old) {
          return Old.File == Entry.File && Old.Text == Entry.Text;
        });
        Entry.Created = !Exists;
        Entry.Separator = !Text.empty() && Text.back() != '\n';
        if (Entry.Separator)
          Text += '\n';
        Text += Entry.Text + '\n';
        Result.push_back(Entry);
        Added.push_back(Entry);
        return true;
      });
    }
  } catch (...) {
    auto Error = std::current_exception();
    unregisterCommand(Layout, Added);
    std::rethrow_exception(Error);
  }
  return Result;
}
void unregisterCommand(const InstallLayout &Layout,
                       std::span<const PathEntry> Entries) {
  if (Layout.System)
    return;
  auto Posix = pathStatement(Layout.CommandPath.parent_path(), false);
  auto Fish = pathStatement(Layout.CommandPath.parent_path(), true);
  for (const auto &Entry : Entries) {
    if (!Entry.File.is_absolute() ||
        (Entry.Text != Posix && Entry.Text != Fish))
      throw Failure("Invalid installed shell PATH entry");
    bool RemoveFile = false;
    editShellFile(Entry.File, false, [&](std::string &Text, bool) {
      auto Start = statementPosition(Text, Entry.Text);
      if (Start == Text.npos)
        return false;
      auto End = Start + Entry.Text.size();
      if (End < Text.size() && Text[End] == '\r')
        ++End;
      if (End < Text.size() && Text[End] == '\n')
        ++End;
      if (Entry.Separator && End == Text.size() && Start &&
          Text[Start - 1] == '\n') {
        --Start;
        if (Start && Text[Start - 1] == '\r')
          --Start;
      }
      Text.erase(Start, End - Start);
      RemoveFile = Entry.Created && Text.empty();
      return true;
    });
    if (RemoveFile)
      fs::remove(Entry.File);
  }
}
void removeInstalledFile(const fs::path &File) { fs::remove(File); }
void finishUninstall(const InstallLayout &, const fs::path &) {}
void activateCommand(const fs::path &Source, const fs::path &CommandPath) {
  auto Temporary = CommandPath.parent_path() / (".llm-activate-" + uniqueId());
  fs::create_symlink(Source, Temporary);
  try {
#ifdef __APPLE__
    fs::permissions(Temporary, static_cast<fs::perms>(0755),
                    fs::perm_options::replace | fs::perm_options::nofollow);
#endif
    fs::rename(Temporary, CommandPath);
  } catch (...) {
    fs::remove(Temporary);
    throw;
  }
}
ProcessResult runProcess(Context &Call, const Path &Exe, const Strings &Args,
                         const ProcessOptions &Options) {
  const auto &Cwd = Options.Directory;
  const auto &Output = Options.Output;
  const auto &ToolBin = Options.ToolBin;
  const auto StdoutToStderr = Options.OutputToError;
  const auto &StdinFile = Options.StandardInput;
  const auto Timeout = Options.Timeout;
  const auto &StdoutFile = Options.StandardOutput;
  const auto &Overrides = Options.Environment;

  cancelled(Call);
  std::optional<TerminalSession> Terminal;
  if (Options.Interactive) {
    if (Output || StdoutFile || StdinFile || Options.CaptureOutput ||
        Options.CaptureError || StdoutToStderr || Timeout.count())
      throw Failure("Interactive processes require inherited terminal streams");
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
      throw Failure("debug requires an interactive terminal");
    Terminal.emplace();
  }
  std::vector<std::string> Arguments{pathText(Exe)};
  Arguments.insert(Arguments.end(), Args.begin(), Args.end());
  std::vector<char *> Argv;
  for (auto &Value : Arguments)
    Argv.push_back(Value.data());
  Argv.push_back(nullptr);
  std::vector<std::string> Environment;
  const std::set<std::string_view> Excluded{"PATH",
                                            "CPATH",
                                            "C_INCLUDE_PATH",
                                            "CPLUS_INCLUDE_PATH",
                                            "OBJC_INCLUDE_PATH",
                                            "LIBRARY_PATH",
                                            "COMPILER_PATH",
                                            "GCC_EXEC_PREFIX",
                                            "LD_PRELOAD",
                                            "LD_LIBRARY_PATH",
                                            "DYLD_LIBRARY_PATH",
                                            "DYLD_INSERT_LIBRARIES",
                                            "CCC_OVERRIDE_OPTIONS",
                                            "CLANG_CONFIG_FILE_SYSTEM_DIR",
                                            "CLANG_CONFIG_FILE_USER_DIR",
                                            "MAKEFLAGS",
                                            "SDKROOT",
                                            "MACOSX_DEPLOYMENT_TARGET",
                                            "INCLUDE",
                                            "LIB"};
  if (ToolBin) {
    for (char **P = environ; *P; ++P) {
      std::string_view Value(*P);
      if (!Excluded.contains(Value.substr(0, Value.find('='))))
        Environment.emplace_back(Value);
    }
    Environment.push_back("PATH=" + pathText(*ToolBin) + ":/usr/bin:/bin");
#ifdef __linux__
    auto Runtime = ToolBin->parent_path() / "host-runtime";
    if (fs::is_directory(Runtime))
      Environment.push_back("LD_LIBRARY_PATH=" + pathText(Runtime));
#endif
  }
  if (!Overrides.empty()) {
    validateEnvironment(Overrides);
    if (!ToolBin)
      for (char **Entry = environ; *Entry; ++Entry)
        Environment.emplace_back(*Entry);
    for (const auto &[Name, Value] : Overrides) {
      std::erase_if(Environment, [&](const auto &Entry) {
        return Entry.substr(0, Entry.find('=')) == Name;
      });
      Environment.push_back(Name + "=" + Value);
    }
  }
  std::vector<char *> Env;
  for (auto &Value : Environment)
    Env.push_back(Value.data());
  Env.push_back(nullptr);
  ProcessStartPipe StartPipe;
  auto MakeCapture = [](std::string *Text) {
    std::unique_ptr<ProcessStartPipe> Result;
    if (Text) {
      Text->clear();
      Result = std::make_unique<ProcessStartPipe>();
      auto Reader = Result->Descriptors[0];
      if (fcntl(Reader, F_SETFL, fcntl(Reader, F_GETFL) | O_NONBLOCK) < 0)
        fail("Configure output pipe");
    }
    return Result;
  };
  auto CapturedOutput = MakeCapture(Options.CaptureOutput);
  auto CapturedError = MakeCapture(Options.CaptureError);
  int Log = -1;
  if (Output) {
    Log = protectProcessDescriptor(
        open(Output->c_str(),
             O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0644));
    if (Log < 0)
      fail("Open process log");
  }
  int SeparateStdout = -1;
  if (StdoutFile) {
    SeparateStdout = protectProcessDescriptor(
        open(StdoutFile->c_str(),
             O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0644));
    if (SeparateStdout < 0) {
      if (Log >= 0)
        close(Log);
      fail("Open process stdout");
    }
  }
  pid_t Child = fork();
  if (Child < 0) {
    if (Log >= 0)
      close(Log);
    if (SeparateStdout >= 0)
      close(SeparateStdout);
    fail("Start process");
  }
  if (Child == 0) {
    close(StartPipe.Descriptors[0]);
    if (setpgid(0, 0))
      childStartError(StartPipe.Descriptors[1], ProcessStartStage::Group,
                      errno);
    if (Terminal) {
      if (tcsetpgrp(STDIN_FILENO, getpid()))
        childStartError(StartPipe.Descriptors[1], ProcessStartStage::Group,
                        errno);
      struct sigaction Default{};
      Default.sa_handler = SIG_DFL;
      sigemptyset(&Default.sa_mask);
      if (sigaction(SIGINT, &Default, nullptr) ||
          sigaction(SIGTTOU, &Default, nullptr))
        childStartError(StartPipe.Descriptors[1], ProcessStartStage::Group,
                        errno);
    }
    if (chdir(Cwd.c_str()))
      childStartError(StartPipe.Descriptors[1], ProcessStartStage::Directory,
                      errno);
    if (StdoutToStderr && Log < 0 && dup2(2, 1) < 0)
      childStartError(StartPipe.Descriptors[1],
                      ProcessStartStage::StdoutRedirect, errno);
    if (Log >= 0) {
      int Input = protectProcessDescriptor(open(
          StdinFile ? StdinFile->c_str() : "/dev/null", O_RDONLY | O_CLOEXEC));
      if (Input < 0)
        childStartError(StartPipe.Descriptors[1], ProcessStartStage::InputOpen,
                        errno);
      if (dup2(Input, 0) < 0)
        childStartError(StartPipe.Descriptors[1],
                        ProcessStartStage::InputRedirect, errno);
      if (dup2(Log, 1) < 0 || dup2(Log, 2) < 0)
        childStartError(StartPipe.Descriptors[1],
                        ProcessStartStage::LogRedirect, errno);
      close(Input);
      close(Log);
    } else if (StdinFile) {
      int Input = protectProcessDescriptor(
          open(StdinFile->c_str(), O_RDONLY | O_CLOEXEC));
      if (Input < 0)
        childStartError(StartPipe.Descriptors[1], ProcessStartStage::InputOpen,
                        errno);
      if (dup2(Input, 0) < 0)
        childStartError(StartPipe.Descriptors[1],
                        ProcessStartStage::InputRedirect, errno);
      close(Input);
    }
    if (SeparateStdout >= 0) {
      if (dup2(SeparateStdout, 1) < 0)
        childStartError(StartPipe.Descriptors[1],
                        ProcessStartStage::StdoutCapture, errno);
      close(SeparateStdout);
    }
    if (CapturedOutput) {
      if (dup2(CapturedOutput->Descriptors[1], 1) < 0 ||
          (!CapturedError && !Options.OutputToError &&
           dup2(CapturedOutput->Descriptors[1], 2) < 0))
        childStartError(StartPipe.Descriptors[1],
                        ProcessStartStage::StdoutCapture, errno);
    }
    if (CapturedError && dup2(CapturedError->Descriptors[1], 2) < 0)
      childStartError(StartPipe.Descriptors[1],
                      ProcessStartStage::StdoutCapture, errno);
    execve(Exe.c_str(), Argv.data(),
           (ToolBin || !Overrides.empty()) ? Env.data() : environ);
    const int Error = errno;
    childStartError(StartPipe.Descriptors[1], ProcessStartStage::Execute, Error,
                    Error == ENOENT ? 127 : 126);
  }
  StartPipe.closeEnd(1);
  if (CapturedOutput)
    CapturedOutput->closeEnd(1);
  if (CapturedError)
    CapturedError->closeEnd(1);
  auto Drain = [](const auto &Pipe, std::string *Text) {
    if (!Pipe)
      return;
    std::array<char, 65536> Buffer;
    for (;;) {
      auto Count = read(Pipe->Descriptors[0], Buffer.data(), Buffer.size());
      if (Count < 0 && errno == EINTR)
        continue;
      if (Count <= 0)
        return;
      if (Text->size() + Count > 256ULL * 1024 * 1024)
        throw Failure("Captured process output exceeds 256 MiB");
      Text->append(Buffer.data(), static_cast<std::size_t>(Count));
    }
  };
  if (Log >= 0)
    close(Log);
  if (SeparateStdout >= 0)
    close(SeparateStdout);
  setpgid(Child, Child);
  int Status = 0;
  bool Reaped = false;
  struct CleanupProcess {
    pid_t Child;
    bool &Reaped;
    ~CleanupProcess() {
      if (!Reaped) {
        kill(-Child, SIGKILL);
        while (waitpid(Child, nullptr, 0) < 0 && errno == EINTR) {
        }
      }
    }
  } Cleanup{Child, Reaped};
  const auto Started = std::chrono::steady_clock::now();
  for (;;) {
    Drain(CapturedOutput, Options.CaptureOutput);
    Drain(CapturedError, Options.CaptureError);
    auto Result = waitpid(Child, &Status, WNOHANG);
    if (Result == Child) {
      Reaped = true;
      break;
    }
    if (Result < 0 && errno != EINTR) {
      kill(-Child, SIGKILL);
      waitpid(Child, nullptr, 0);
      Reaped = true;
      fail("Wait for process");
    }
    if (Interrupted || Call.Cancellation.stop_requested()) {
      kill(-Child, SIGKILL);
      waitpid(Child, nullptr, 0);
      Reaped = true;
      cancelled(Call);
    }
    if (Timeout.count() > 0 &&
        std::chrono::steady_clock::now() - Started >= Timeout) {
      kill(-Child, SIGKILL);
      while (waitpid(Child, nullptr, 0) < 0 && errno == EINTR) {
      }
      Reaped = true;
      Drain(CapturedOutput, Options.CaptureOutput);
      Drain(CapturedError, Options.CaptureError);
      return {124, 0, true};
    }
    if (Result == 0)
      std::this_thread::sleep_for(
          std::chrono::milliseconds(CapturedOutput || CapturedError ? 1 : 10));
  }
  Drain(CapturedOutput, Options.CaptureOutput);
  Drain(CapturedError, Options.CaptureError);
  ProcessStartError StartError{};
  size_t Received = 0;
  while (Received < sizeof(StartError)) {
    auto Count = read(StartPipe.Descriptors[0],
                      reinterpret_cast<char *>(&StartError) + Received,
                      sizeof(StartError) - Received);
    if (Count < 0 && errno == EINTR)
      continue;
    if (Count < 0)
      fail("Read process start result");
    if (!Count)
      break;
    Received += static_cast<size_t>(Count);
  }
  if (Received) {
    if (Received != sizeof(StartError))
      throw Failure("Incomplete process start result for " + pathText(Exe));
    throw Failure("Cannot start '" + pathText(Exe) +
                  "': " + std::string(processStartStage(StartError.Stage)) +
                  ": " + std::strerror(StartError.Error) + " (errno " +
                  std::to_string(StartError.Error) + ")");
  }
  if (Terminal)
    Terminal->restore();
  if (WIFSIGNALED(Status))
    return {128 + WTERMSIG(Status), WTERMSIG(Status)};
  return {WEXITSTATUS(Status), 0};
}

}
