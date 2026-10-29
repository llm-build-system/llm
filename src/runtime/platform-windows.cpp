module;
#include <windows.h>

#include <aclapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>

module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;
using ProcessEnvironment = std::map<std::string, std::string>;

void validateEnvironment(const ProcessEnvironment &Environment) {
  for (const auto &[Name, Value] : Environment)
    if (Name.empty() || Name.find_first_of("=\0", 0, 2) != std::string::npos ||
        Value.find('\0') != std::string::npos)
      throw Failure("Invalid process environment override");
}

class Handle {
  HANDLE Value = nullptr;

public:
  explicit Handle(HANDLE Value = nullptr) : Value(Value) {}
  ~Handle() {
    if (Value && Value != INVALID_HANDLE_VALUE)
      CloseHandle(Value);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  HANDLE get() const { return Value; }
  void reset(HANDLE New = nullptr) {
    if (Value && Value != INVALID_HANDLE_VALUE)
      CloseHandle(Value);
    Value = New;
  }
};
std::string utf8(std::wstring_view Value);
std::wstring wide(std::string_view Value);
[[noreturn]] void failWindows(std::string_view Action,
                              DWORD Error = GetLastError());
std::wstring quoteArg(std::wstring_view Argument);
std::wstring appendPathEntry(std::wstring_view Current,
                             std::wstring_view Directory);
bool addPath(const Path &Directory, bool System);

bool redirectsPath(const Path &File) {
  auto Attributes = GetFileAttributesW(nativePath(File).c_str());
  if (Attributes == INVALID_FILE_ATTRIBUTES)
    failWindows("Inspect " + pathText(File));
  return (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::string collisionKey(const Path &File) {
  auto Name = File.generic_wstring();
  CharLowerBuffW(Name.data(), static_cast<DWORD>(Name.size()));
  return utf8(Name);
}

namespace {
std::atomic_bool Interrupted = false;
std::atomic_bool InteractiveProcess = false;
template <class Action> bool retrySharingViolation(Action &&Operation) {
  const auto Deadline = GetTickCount64() + 2000;
  for (;;) {
    if (Operation())
      return true;
    const auto Error = GetLastError();
    if ((Error != ERROR_SHARING_VIOLATION && Error != ERROR_LOCK_VIOLATION) ||
        GetTickCount64() >= Deadline) {
      SetLastError(Error);
      return false;
    }
    if (Interrupted)
      throw Failure("Operation cancelled");
    Sleep(25);
  }
}
std::vector<fs::path> PendingRemovals;
BOOL WINAPI onControl(DWORD Event) {
  if (Event == CTRL_C_EVENT || Event == CTRL_BREAK_EVENT) {
    if (!InteractiveProcess)
      Interrupted = true;
    return TRUE;
  }
  return FALSE;
}
bool equalCi(std::wstring_view A, std::wstring_view B) {
  return CompareStringOrdinal(A.data(), static_cast<int>(A.size()), B.data(),
                              static_cast<int>(B.size()), TRUE) == CSTR_EQUAL;
}
std::vector<wchar_t> cleanEnvironment(const fs::path &ToolBin) {
  const wchar_t *Raw = GetEnvironmentStringsW();
  if (!Raw)
    failWindows("Read environment");
  std::vector<wchar_t> Result;
  const std::array Excluded{L"PATH",
                            L"CPATH",
                            L"C_INCLUDE_PATH",
                            L"CPLUS_INCLUDE_PATH",
                            L"LIBRARY_PATH",
                            L"COMPILER_PATH",
                            L"GCC_EXEC_PREFIX",
                            L"CL",
                            L"_CL_",
                            L"LINK",
                            L"CCACHE",
                            L"MAKEFLAGS",
                            L"CCC_OVERRIDE_OPTIONS",
                            L"CLANG_CONFIG_FILE_SYSTEM_DIR",
                            L"CLANG_CONFIG_FILE_USER_DIR",
                            L"LLVM_SYMBOLIZER_PATH",
                            L"LLVM_PROFILE_FILE",
                            L"INCLUDE",
                            L"LIB",
                            L"LIBPATH",
                            L"SDKROOT",
                            L"VCToolsInstallDir",
                            L"WindowsSdkDir",
                            L"WindowsSDKVersion"};
  for (const wchar_t *P = Raw; *P; P += wcslen(P) + 1) {
    std::wstring_view Entry(P);
    auto Split = Entry.find(L'=', Entry.starts_with(L'=') ? 1 : 0);
    auto Name = Entry.substr(0, Split);
    if (std::ranges::any_of(Excluded,
                            [&](auto Item) { return equalCi(Name, Item); }))
      continue;
    Result.insert(Result.end(), Entry.begin(), Entry.end());
    Result.push_back(0);
  }
  FreeEnvironmentStringsW(const_cast<wchar_t *>(Raw));
  std::array<wchar_t, MAX_PATH> System{};
  if (!GetSystemDirectoryW(System.data(), static_cast<UINT>(System.size())))
    failWindows("Find Windows directory");
  const std::wstring Path = L"PATH=" + ToolBin.wstring() + L";" + System.data();
  Result.insert(Result.end(), Path.begin(), Path.end());
  Result.push_back(0);
  constexpr std::wstring_view Libraries = L"LIB=";
  Result.insert(Result.end(), Libraries.begin(), Libraries.end());
  Result.push_back(0);
  Result.push_back(0);
  return Result;
}
std::vector<wchar_t> environmentOverrides(std::vector<wchar_t> Base,
                                          const ProcessEnvironment &Overrides) {
  if (Overrides.empty())
    return Base;
  validateEnvironment(Overrides);
  for (auto First = Overrides.begin(); First != Overrides.end(); ++First)
    for (auto Second = std::next(First); Second != Overrides.end(); ++Second)
      if (equalCi(wide(First->first), wide(Second->first)))
        throw Failure("Duplicate Windows environment override: " +
                      First->first);
  if (Base.empty()) {
    auto Raw = GetEnvironmentStringsW();
    if (!Raw)
      failWindows("Read process environment");
    for (auto P = Raw; *P; P += wcslen(P) + 1)
      Base.insert(Base.end(), P, P + wcslen(P) + 1);
    Base.push_back(0);
    FreeEnvironmentStringsW(Raw);
  }
  std::vector<std::wstring> Entries;
  for (auto P = Base.data(); *P; P += wcslen(P) + 1) {
    std::wstring Entry(P);
    auto Split = Entry.find(L'=', Entry.starts_with(L'=') ? 1 : 0);
    bool Replaced = std::ranges::any_of(Overrides, [&](const auto &Item) {
      return equalCi(Entry.substr(0, Split), wide(Item.first));
    });
    if (!Replaced)
      Entries.push_back(std::move(Entry));
  }
  for (const auto &[Name, Value] : Overrides)
    Entries.push_back(wide(Name) + L"=" + wide(Value));
  std::ranges::sort(Entries, [](const auto &Left, const auto &Right) {
    return CompareStringOrdinal(Left.data(), static_cast<int>(Left.size()),
                                Right.data(), static_cast<int>(Right.size()),
                                TRUE) == CSTR_LESS_THAN;
  });
  std::vector<wchar_t> Result;
  for (const auto &Entry : Entries) {
    Result.insert(Result.end(), Entry.begin(), Entry.end());
    Result.push_back(0);
  }
  Result.push_back(0);
  return Result;
}
}
std::string utf8(std::wstring_view Value) {
  if (Value.empty())
    return {};
  int Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
                                 static_cast<int>(Value.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (!Size)
    failWindows("Encode UTF-8");
  std::string Result(Size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
                      static_cast<int>(Value.size()), Result.data(), Size,
                      nullptr, nullptr);
  return Result;
}
std::wstring wide(std::string_view Value) {
  if (Value.empty())
    return {};
  int Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
                                 static_cast<int>(Value.size()), nullptr, 0);
  if (!Size)
    failWindows("Decode UTF-8");
  std::wstring Result(Size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
                      static_cast<int>(Value.size()), Result.data(), Size);
  return Result;
}

void replaceFile(const fs::path &Source, const fs::path &Destination) {
  if (!MoveFileExW(nativePath(Source).c_str(), nativePath(Destination).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    failWindows("Replace file");
}
void removeOutputFile(const Path &File) {
  if (!retrySharingViolation([&] {
        if (DeleteFileW(nativePath(File).c_str()))
          return true;
        const auto Error = GetLastError();
        return Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND;
      })) {
    const auto Error = GetLastError();
    failWindows("Remove generated output " + pathText(File), Error);
  }
}

std::string quoteArgument(std::string_view Argument) {
  return utf8(quoteArg(wide(Argument)));
}
[[noreturn]] void failWindows(std::string_view Action, DWORD Error) {
  wchar_t *Buffer = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, Error, 0, reinterpret_cast<wchar_t *>(&Buffer), 0,
                 nullptr);
  std::string Detail = Buffer ? utf8(Buffer) : "Unknown Windows error";
  if (Buffer)
    LocalFree(Buffer);
  throw Failure(std::string(Action) + ": " + Detail + " (" +
                std::to_string(Error) + ")");
}
fs::path executablePath() {
  std::wstring Path(32768, L'\0');
  DWORD Count =
      GetModuleFileNameW(nullptr, Path.data(), static_cast<DWORD>(Path.size()));
  if (!Count || Count == Path.size())
    failWindows("Find llm executable");
  Path.resize(Count);
  return Path;
}
InstallLayout systemLayout() {
  PWSTR Directory = nullptr;
  HRESULT Hr = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT,
                                    nullptr, &Directory);
  if (FAILED(Hr))
    throw Failure("Cannot locate Program Files");
  fs::path Program(Directory);
  CoTaskMemFree(Directory);
  Directory = nullptr;
  Hr = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr,
                            &Directory);
  if (FAILED(Hr))
    throw Failure("Cannot locate ProgramData");
  fs::path Data(Directory);
  CoTaskMemFree(Directory);
  Program /= L"llm";
  Data /= L"llm";
  return {Program, Data, Program / L"bin/llm.exe", true};
}
bool elevated() {
  HANDLE Raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw))
    failWindows("Read process token");
  Handle Token(Raw);
  TOKEN_ELEVATION Elevation{};
  DWORD Size = sizeof(Elevation);
  if (!GetTokenInformation(Token.get(), TokenElevation, &Elevation, Size,
                           &Size))
    failWindows("Read elevation");
  return Elevation.TokenIsElevated != 0;
}
InstallLayout userLayout() {
  PWSTR Directory = nullptr;
  auto Result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                     nullptr, &Directory);
  if (FAILED(Result))
    throw Failure("Cannot locate LocalAppData");
  Path Root(Directory);
  CoTaskMemFree(Directory);
  Root /= L"llm";
  return {Root, Root, Root / L"bin/llm.exe", false};
}
int elevateOperation(Context &Call, std::string_view Operation,
                     std::optional<Target> BuildTarget) {
  Strings Arguments;
  Arguments.emplace_back(
      "--color=" + std::string(Call.Output.Color == ColorMode::Always ? "always"
                               : Call.Output.Color == ColorMode::Never
                                   ? "never"
                                   : "auto"));
  if (Call.Output.Quiet)
    Arguments.emplace_back("--quiet");
  if (Call.Output.Verbose)
    Arguments.emplace_back("--verbose");
  Arguments.emplace_back("--global");
  if (BuildTarget) {
    Arguments.emplace_back("--target");
    Arguments.emplace_back(targetName(*BuildTarget));
  }

  SHELLEXECUTEINFOW Info{};
  Info.cbSize = sizeof(Info);
  auto Exe = executablePath().wstring();
  Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  auto Argument = quoteArg(wide(Operation));
  for (const auto &Value : Arguments)
    Argument += L" " + quoteArg(wide(Value));
  Info.lpVerb = L"runas";
  Info.lpFile = Exe.c_str();
  Info.lpParameters = Argument.c_str();
  Info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&Info))
    failWindows("Administrator authorization failed");
  Handle Process(Info.hProcess);
  WaitForSingleObject(Process.get(), INFINITE);
  DWORD Code = 1;
  if (!GetExitCodeProcess(Process.get(), &Code))
    failWindows("Read installer result");
  return static_cast<int>(Code);
}
void enableCancellation() {
  if (!SetConsoleCtrlHandler(onControl, TRUE))
    failWindows("Install cancellation handler");
}
bool interrupted() { return Interrupted; }
void checkInterrupted() {
  if (interrupted())
    throw Failure("Operation cancelled");
}
std::wstring quoteArg(std::wstring_view Arg) {
  std::wstring Result = L"\"";
  size_t Slashes = 0;
  for (wchar_t Ch : Arg) {
    if (Ch == L'\\') {
      ++Slashes;
      continue;
    }
    Result.append(Slashes * (Ch == L'"' ? 2 : 1), L'\\');
    Slashes = 0;
    if (Ch == L'"')
      Result.push_back(L'\\');
    Result.push_back(Ch);
  }
  Result.append(Slashes * 2, L'\\');
  Result.push_back(L'"');
  return Result;
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
  struct RestoreInteractive {
    UINT Input = 0, Output = 0;
    ~RestoreInteractive() {
      InteractiveProcess = false;
      if (Input)
        SetConsoleCP(Input);
      if (Output)
        SetConsoleOutputCP(Output);
    }
  } InteractiveGuard;
  if (Options.Interactive) {
    DWORD Mode;
    if (Output || StdoutFile || StdinFile || Options.CaptureOutput ||
        Options.CaptureError || StdoutToStderr || Timeout.count())
      throw Failure("Interactive processes require inherited terminal streams");
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &Mode) ||
        !GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &Mode))
      throw Failure("debug requires an interactive terminal");
    InteractiveGuard.Input = GetConsoleCP();
    InteractiveGuard.Output = GetConsoleOutputCP();
    if (!InteractiveGuard.Input || !InteractiveGuard.Output ||
        !SetConsoleCP(CP_UTF8) || !SetConsoleOutputCP(CP_UTF8))
      failWindows("Configure debugger terminal encoding");
    InteractiveProcess = true;
  }
  std::wstring CommandPath = quoteArg(Exe.wstring());
  for (auto &Arg : Args)
    CommandPath += L" " + quoteArg(wide(Arg));
  STARTUPINFOW Startup{};
  Startup.cb = sizeof(Startup);
  SECURITY_ATTRIBUTES Security{sizeof(Security), nullptr, TRUE};
  auto OpenOutput = [&](const Path &File) {
    HANDLE Result = INVALID_HANDLE_VALUE;
    if (!retrySharingViolation([&] {
          cancelled(Call);
          Result = CreateFileW(File.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               &Security, CREATE_ALWAYS, 0, nullptr);
          return Result != INVALID_HANDLE_VALUE;
        })) {
      const auto Error = GetLastError();
      failWindows("Open process output " + pathText(File), Error);
    }
    return Result;
  };
  Handle Log(Output ? OpenOutput(*Output) : nullptr);
  Handle SeparateStdout(StdoutFile ? OpenOutput(*StdoutFile) : nullptr);
  Handle Input((Output || StdinFile)
                   ? CreateFileW(StdinFile ? StdinFile->c_str() : L"NUL",
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &Security,
                                 OPEN_EXISTING, 0, nullptr)
                   : nullptr);
  if (Output) {
    if (Log.get() == INVALID_HANDLE_VALUE ||
        Input.get() == INVALID_HANDLE_VALUE)
      failWindows("Open process log");
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdInput = Input.get();
    Startup.hStdOutput = Log.get();
    Startup.hStdError = Log.get();
  } else {
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdInput =
        StdinFile ? Input.get() : GetStdHandle(STD_INPUT_HANDLE);
    Startup.hStdOutput =
        GetStdHandle(StdoutToStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    Startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }
  if (StdoutFile)
    Startup.hStdOutput = SeparateStdout.get();
  struct CapturePipe {
    Handle Read, Write;
    std::string *Text;
    CapturePipe(std::string *Text, SECURITY_ATTRIBUTES &Security) : Text(Text) {
      if (!Text)
        return;
      Text->clear();
      HANDLE Reader, Writer;
      if (!CreatePipe(&Reader, &Writer, &Security, 1024 * 1024))
        failWindows("Create output pipe");
      Read.reset(Reader);
      Write.reset(Writer);
      if (!SetHandleInformation(Read.get(), HANDLE_FLAG_INHERIT, 0))
        failWindows("Protect output pipe");
    }
    void drain() {
      if (!Text)
        return;
      DWORD Available;
      std::array<char, 65536> Buffer;
      while (
          PeekNamedPipe(Read.get(), nullptr, 0, nullptr, &Available, nullptr) &&
          Available) {
        DWORD Count;
        if (!ReadFile(Read.get(), Buffer.data(),
                      std::min<DWORD>(Available, Buffer.size()), &Count,
                      nullptr))
          failWindows("Read process output");
        if (Text->size() + Count > 256ULL * 1024 * 1024)
          throw Failure("Captured process output exceeds 256 MiB");
        Text->append(Buffer.data(), Count);
      }
    }
  } CapturedOutput(Options.CaptureOutput, Security),
      CapturedError(Options.CaptureError, Security);
  if (Options.CaptureOutput) {
    Startup.hStdOutput = CapturedOutput.Write.get();
    if (!Options.CaptureError && !Options.OutputToError)
      Startup.hStdError = CapturedOutput.Write.get();
  }
  if (Options.CaptureError)
    Startup.hStdError = CapturedError.Write.get();
  Handle Job(CreateJobObjectW(nullptr, nullptr));
  if (!Job.get())
    failWindows("Create process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
  Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(Job.get(), JobObjectExtendedLimitInformation,
                               &Limits, sizeof(Limits)))
    failWindows("Configure process job");
  auto Environment = environmentOverrides(
      ToolBin ? cleanEnvironment(*ToolBin) : std::vector<wchar_t>{}, Overrides);
  PROCESS_INFORMATION Process{};
  if (!CreateProcessW(Exe.c_str(), CommandPath.data(), nullptr, nullptr, TRUE,
                      CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                      Environment.empty() ? nullptr : Environment.data(),
                      Cwd.c_str(), &Startup, &Process))
    failWindows("Start " + pathText(Exe));
  Handle Child(Process.hProcess);
  Handle Thread(Process.hThread);
  CapturedOutput.Write.reset();
  CapturedError.Write.reset();
  if (!AssignProcessToJobObject(Job.get(), Child.get())) {
    auto Error = GetLastError();
    TerminateProcess(Child.get(), 1);
    failWindows("Assign process job", Error);
  }
  if (ResumeThread(Thread.get()) == static_cast<DWORD>(-1))
    failWindows("Resume process");
  const auto Started = std::chrono::steady_clock::now();
  while (true) {
    CapturedOutput.drain();
    CapturedError.drain();
    DWORD Status = WaitForSingleObject(
        Child.get(), Options.CaptureOutput || Options.CaptureError ? 1 : 100);
    if (Status == WAIT_OBJECT_0)
      break;
    if (Status == WAIT_FAILED)
      failWindows("Wait for process");
    cancelled(Call);
    if (Timeout.count() > 0 &&
        std::chrono::steady_clock::now() - Started >= Timeout) {
      if (!TerminateJobObject(Job.get(), 124))
        failWindows("Terminate timed-out process");
      WaitForSingleObject(Child.get(), INFINITE);
      CapturedOutput.drain();
      CapturedError.drain();
      return {124, 0, true};
    }
  }
  CapturedOutput.drain();
  CapturedError.drain();
  DWORD Code = 1;
  if (!GetExitCodeProcess(Child.get(), &Code))
    failWindows("Read process exit code");
  return {static_cast<int>(Code)};
}

std::wstring appendPathEntry(std::wstring_view Current,
                             std::wstring_view Directory) {
  auto Normalize = [](std::wstring_view Value) {
    while (!Value.empty() && (Value.front() == L' ' || Value.front() == L'"'))
      Value.remove_prefix(1);
    while (!Value.empty() && (Value.back() == L' ' || Value.back() == L'"' ||
                              Value.back() == L'\\' || Value.back() == L'/'))
      Value.remove_suffix(1);
    return Value;
  };
  for (size_t Pos = 0; Pos <= Current.size();) {
    auto End = Current.find(L';', Pos);
    if (End == std::wstring_view::npos)
      End = Current.size();
    if (equalCi(Normalize(Current.substr(Pos, End - Pos)),
                Normalize(Directory)))
      return std::wstring(Current);
    if (End == Current.size())
      break;
    Pos = End + 1;
  }
  std::wstring Result(Current);
  if (!Result.empty())
    Result += L';';
  Result += Directory;
  return Result;
}
bool addPath(const fs::path &Directory, bool System) {
  HKEY Key = nullptr;
  LSTATUS Status = RegOpenKeyExW(
      System ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
      System
          ? L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
          : L"Environment",
      0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key);
  if (Status != ERROR_SUCCESS)
    failWindows("Open PATH registry", Status);
  struct Close {
    HKEY Key;
    ~Close() { RegCloseKey(Key); }
  } Close{Key};
  DWORD Type = REG_EXPAND_SZ, Bytes = 0;
  Status = RegQueryValueExW(Key, L"Path", nullptr, &Type, nullptr, &Bytes);
  if (Status != ERROR_SUCCESS && Status != ERROR_FILE_NOT_FOUND)
    failWindows("Read PATH", Status);
  bool Missing = Status == ERROR_FILE_NOT_FOUND;
  if (!Missing && Type != REG_SZ && Type != REG_EXPAND_SZ)
    throw Failure("PATH has an unsupported registry type");
  std::wstring Current(Bytes / sizeof(wchar_t) + 1, L'\0');
  if (!Missing) {
    Status = RegQueryValueExW(Key, L"Path", nullptr, &Type,
                              reinterpret_cast<BYTE *>(Current.data()), &Bytes);
    if (Status != ERROR_SUCCESS)
      failWindows("Read PATH", Status);
  }
  Current.resize(wcslen(Current.c_str()));
  auto Updated = appendPathEntry(Current, Directory.wstring());
  if (Updated != Current) {
    Status = RegSetValueExW(
        Key, L"Path", 0, Missing ? REG_EXPAND_SZ : Type,
        reinterpret_cast<const BYTE *>(Updated.c_str()),
        static_cast<DWORD>((Updated.size() + 1) * sizeof(wchar_t)));
    if (Status != ERROR_SUCCESS)
      failWindows("Update PATH", Status);
    DWORD_PTR Ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 3000, &Ignored);
  }
  return Updated != Current;
}
void rejectSymlinkAncestors(const fs::path &Path) {
  auto Logical = Path;
  auto Text = Path.native();
  if (Text.starts_with(LR"(\\?\UNC\)"))
    Logical = fs::path(std::wstring(LR"(\\)") + Text.substr(8));
  else if (Text.starts_with(LR"(\\?\)"))
    Logical = fs::path(Text.substr(4));
  if (!Logical.is_absolute())
    throw Failure("Installation path must be absolute");
  Logical = Logical.lexically_normal();
  auto Current = Logical.root_path();
  auto Inspect = [&] {
    DWORD Attr = GetFileAttributesW(nativePath(Current).c_str());
    if (Attr == INVALID_FILE_ATTRIBUTES) {
      auto Error = GetLastError();
      if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
        return;
      failWindows("Inspect " + pathText(Current), Error);
    }
    if (Attr & FILE_ATTRIBUTE_REPARSE_POINT)
      throw Failure("Reparse points are not allowed: " + pathText(Current));
  };
  if (!Logical.root_name().native().starts_with(LR"(\\)"))
    Inspect();
  for (const auto &Component : Logical.relative_path()) {
    Current /= Component;
    Inspect();
  }
}

struct FileLock::Impl {
  Handle File;
  static HANDLE acquire(Context &Call, const fs::path &Path, bool Wait) {
    for (;;) {
      cancelled(Call);
      auto Handle =
          CreateFileW(nativePath(Path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (Handle != INVALID_HANDLE_VALUE)
        return Handle;
      auto Error = GetLastError();
      if (!Wait ||
          (Error != ERROR_SHARING_VIOLATION && Error != ERROR_LOCK_VIOLATION))
        failWindows("Acquire installation lock", Error);
      Sleep(50);
    }
  }
  explicit Impl(Context &Call, const fs::path &Path, bool Wait)
      : File(acquire(Call, Path, Wait)) {}
};
FileLock::FileLock(Context &Call, const fs::path &Path, bool Wait)
    : State(std::make_unique<Impl>(Call, Path, Wait)) {}
FileLock::~FileLock() = default;
void secureDataDirectory(const fs::path &Directory, bool System) {
  fs::create_directories(Directory);
  if (!System)
    return;
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",
          SDDL_REVISION_1, &Descriptor, nullptr))
    failWindows("Prepare ProgramData permissions");
  PACL Dacl = nullptr;
  BOOL Present = FALSE, Defaulted = FALSE;
  GetSecurityDescriptorDacl(Descriptor, &Present, &Dacl, &Defaulted);
  auto Error = SetNamedSecurityInfoW(
      const_cast<wchar_t *>(Directory.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
      nullptr, Dacl, nullptr);
  LocalFree(Descriptor);
  if (Error != ERROR_SUCCESS)
    failWindows("Protect installation data", Error);
}
fs::path userHome() {
  PWSTR Path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &Path)))
    throw Failure("Cannot determine user home");
  fs::path Result(Path);
  CoTaskMemFree(Path);
  return fs::canonical(Result);
}
namespace {
void privateOwner(const fs::path &Path) {
  PSID Owner = nullptr;
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  auto Status =
      GetNamedSecurityInfoW(const_cast<wchar_t *>(Path.c_str()), SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION, &Owner, nullptr,
                            nullptr, nullptr, &Descriptor);
  if (Status != ERROR_SUCCESS)
    failWindows("Inspect private file owner", Status);
  struct Release {
    void *Value;
    ~Release() { LocalFree(Value); }
  } Release{Descriptor};
  HANDLE Raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw))
    failWindows("Read current user token");
  Handle Token(Raw);
  DWORD Size = 0;
  GetTokenInformation(Token.get(), TokenUser, nullptr, 0, &Size);
  std::vector<unsigned char> User(Size);
  if (!GetTokenInformation(Token.get(), TokenUser, User.data(), Size, &Size))
    failWindows("Read current user");
  GetTokenInformation(Token.get(), TokenOwner, nullptr, 0, &Size);
  std::vector<unsigned char> DefaultOwner(Size);
  if (!GetTokenInformation(Token.get(), TokenOwner, DefaultOwner.data(), Size,
                           &Size))
    failWindows("Read current owner");
  if (!EqualSid(Owner, reinterpret_cast<TOKEN_USER *>(User.data())->User.Sid) &&
      !EqualSid(Owner,
                reinterpret_cast<TOKEN_OWNER *>(DefaultOwner.data())->Owner))
    throw Failure("Private path belongs to another user");
}
std::wstring privateDescriptor() {
  HANDLE Raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw))
    failWindows("Read current user token");
  Handle Token(Raw);
  DWORD Size = 0;
  GetTokenInformation(Token.get(), TokenUser, nullptr, 0, &Size);
  std::vector<unsigned char> Data(Size);
  if (!GetTokenInformation(Token.get(), TokenUser, Data.data(), Size, &Size))
    failWindows("Read current user identity");
  LPWSTR Sid = nullptr;
  if (!ConvertSidToStringSidW(
          reinterpret_cast<TOKEN_USER *>(Data.data())->User.Sid, &Sid))
    failWindows("Read current user SID");
  std::wstring Result = L"D:P(A;;FA;;;" + std::wstring(Sid) + L")(A;;FA;;;SY)";
  LocalFree(Sid);
  return Result;
}
}
void protectPrivatePath(const fs::path &Path, bool Directory) {
  rejectSymlinkAncestors(Path);
  if (Directory && !fs::is_directory(Path))
    throw Failure("Private directory is missing");
  if (!Directory && !fs::is_regular_file(Path))
    throw Failure("Private file is missing");
  privateOwner(Path);
  auto Text = privateDescriptor();
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          Text.c_str(), SDDL_REVISION_1, &Descriptor, nullptr))
    failWindows("Prepare private permissions");
  struct Release {
    void *Value;
    ~Release() { LocalFree(Value); }
  } Release{Descriptor};
  if (!SetFileSecurityW(Path.c_str(),
                        DACL_SECURITY_INFORMATION |
                            PROTECTED_DACL_SECURITY_INFORMATION,
                        Descriptor))
    failWindows("Set private file permissions");
}
void checkPrivateFile(const fs::path &Path) {
  rejectSymlinkAncestors(Path);
  if (!fs::is_regular_file(Path))
    throw Failure("Credential store is not a regular file");
  privateOwner(Path);
  PACL Acl = nullptr;
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  auto Status = GetNamedSecurityInfoW(
      const_cast<wchar_t *>(Path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION, nullptr, nullptr, &Acl, nullptr, &Descriptor);
  if (Status != ERROR_SUCCESS)
    failWindows("Inspect private file permissions", Status);
  struct Release {
    void *Value;
    ~Release() { LocalFree(Value); }
  } Release{Descriptor};
  if (!Acl)
    throw Failure("Credential store permissions are not private");
  HANDLE Raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw))
    failWindows("Read current user token");
  Handle Token(Raw);
  DWORD Size = 0;
  GetTokenInformation(Token.get(), TokenUser, nullptr, 0, &Size);
  std::vector<unsigned char> User(Size);
  if (!GetTokenInformation(Token.get(), TokenUser, User.data(), Size, &Size))
    failWindows("Read current user");
  std::array<unsigned char, SECURITY_MAX_SID_SIZE> System{};
  DWORD SystemSize = System.size();
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, System.data(),
                          &SystemSize))
    failWindows("Read system identity");
  for (DWORD Index = 0; Index < Acl->AceCount; ++Index) {
    void *Value = nullptr;
    if (!GetAce(Acl, Index, &Value))
      failWindows("Inspect private access rule");
    auto Header = static_cast<ACE_HEADER *>(Value);
    if (Header->AceType == ACCESS_DENIED_ACE_TYPE)
      continue;
    if (Header->AceType != ACCESS_ALLOWED_ACE_TYPE)
      throw Failure("Unsupported private file access rule");
    auto Ace = static_cast<ACCESS_ALLOWED_ACE *>(Value);
    auto Sid = reinterpret_cast<PSID>(&Ace->SidStart);
    if ((Ace->Mask & (FILE_READ_DATA | FILE_WRITE_DATA | GENERIC_ALL |
                      GENERIC_READ | GENERIC_WRITE)) &&
        !EqualSid(Sid, reinterpret_cast<TOKEN_USER *>(User.data())->User.Sid) &&
        !EqualSid(Sid, System.data()))
      throw Failure("Credential store permissions must restrict access to the "
                    "current user");
  }
}
std::string readSecret(std::string_view Prompt) {
  auto Input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD Mode = 0;
  if (!GetConsoleMode(Input, &Mode))
    throw Failure("login requires a terminal. Provision a private credential "
                  "store for CI");
  std::cerr << Prompt << std::flush;
  if (!SetConsoleMode(Input, Mode & ~ENABLE_ECHO_INPUT))
    failWindows("Disable terminal echo");
  struct Restore {
    HANDLE Input;
    DWORD Mode;
    ~Restore() {
      SetConsoleMode(Input, Mode);
      std::cerr << '\n';
    }
  } Restore{Input, Mode};
  std::wstring Value;
  std::array<wchar_t, 512> Buffer{};
  for (;;) {
    DWORD Count = 0;
    if (!ReadConsoleW(Input, Buffer.data(), Buffer.size(), &Count, nullptr))
      failWindows("Read registry token");
    Value.append(Buffer.data(), Count);
    if (Value.size() > 16384)
      throw Failure("Registry token exceeds size limit");
    if (Value.find(L'\n') != std::wstring::npos || !Count)
      break;
  }
  while (!Value.empty() && (Value.back() == L'\r' || Value.back() == L'\n'))
    Value.pop_back();
  return utf8(Value);
}
void validateCommandRegistration(const InstallLayout &Layout) {
  if (pathText(Layout.CommandPath.parent_path()).find_first_of(";\r\n") !=
      std::string::npos)
    throw Failure("PATH directories cannot contain semicolons or line breaks");
}
std::vector<PathEntry> registerCommand(const InstallLayout &Layout,
                                       std::span<const PathEntry> Previous) {
  auto Directory = Layout.CommandPath.parent_path();
  bool Added = addPath(Directory, Layout.System);
  if (Added)
    return {{Path{}, pathText(Directory)}};
  for (const auto &Entry : Previous)
    if (Entry.File.empty() && Entry.Text == pathText(Directory))
      return {Entry};
  return {};
}
void unregisterCommand(const InstallLayout &Layout,
                       std::span<const PathEntry> Entries) {
  if (Entries.empty())
    return;
  for (const auto &Entry : Entries)
    if (!Entry.File.empty() ||
        Entry.Text != pathText(Layout.CommandPath.parent_path()))
      throw Failure("Invalid installed registry PATH entry");
  HKEY Raw = nullptr;
  auto Status = RegOpenKeyExW(
      Layout.System ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
      Layout.System
          ? L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
          : L"Environment",
      0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Raw);
  if (Status != ERROR_SUCCESS)
    failWindows("Open PATH registry", Status);
  struct Close {
    HKEY Key;
    ~Close() { RegCloseKey(Key); }
  } Close{Raw};
  DWORD Type = 0, Bytes = 0;
  Status = RegQueryValueExW(Raw, L"Path", nullptr, &Type, nullptr, &Bytes);
  if (Status == ERROR_FILE_NOT_FOUND)
    return;
  if (Status != ERROR_SUCCESS)
    failWindows("Read PATH", Status);
  if (Type != REG_SZ && Type != REG_EXPAND_SZ)
    throw Failure("PATH has an unsupported registry type");
  std::wstring Current(Bytes / sizeof(wchar_t) + 1, L'\0');
  Status = RegQueryValueExW(Raw, L"Path", nullptr, &Type,
                            reinterpret_cast<BYTE *>(Current.data()), &Bytes);
  if (Status != ERROR_SUCCESS)
    failWindows("Read PATH", Status);
  Current.resize(wcslen(Current.c_str()));
  auto Directory = Layout.CommandPath.parent_path().wstring();
  std::wstring Updated;
  bool First = true;
  for (size_t Pos = 0; Pos <= Current.size();) {
    auto End = Current.find(L';', Pos);
    if (End == std::wstring::npos)
      End = Current.size();
    auto Entry = Current.substr(Pos, End - Pos);
    if (appendPathEntry(Entry, Directory) != Entry) {
      if (!First)
        Updated += L';';
      Updated += Entry;
      First = false;
    }
    if (End == Current.size())
      break;
    Pos = End + 1;
  }
  if (Updated == Current)
    return;
  Status = RegSetValueExW(
      Raw, L"Path", 0, Type, reinterpret_cast<const BYTE *>(Updated.c_str()),
      static_cast<DWORD>((Updated.size() + 1) * sizeof(wchar_t)));
  if (Status != ERROR_SUCCESS)
    failWindows("Remove llm from PATH", Status);
  DWORD_PTR Ignored = 0;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                      reinterpret_cast<LPARAM>(L"Environment"),
                      SMTO_ABORTIFHUNG, 3000, &Ignored);
}
void removeInstalledFile(const fs::path &File) {
  if (DeleteFileW(nativePath(File).c_str()))
    return;
  auto Error = GetLastError();
  if ((Error != ERROR_ACCESS_DENIED && Error != ERROR_SHARING_VIOLATION) ||
      File.filename() != L"llm.exe")
    failWindows("Remove installed file", Error);
  auto Retired =
      File.parent_path() / wide(".llm-delete-" + uniqueId() + ".exe");
  if (!MoveFileExW(nativePath(File).c_str(), nativePath(Retired).c_str(),
                   MOVEFILE_WRITE_THROUGH))
    failWindows("Retire running llm executable");
  PendingRemovals.push_back(Retired);
}
void finishUninstall(const InstallLayout &Layout, const fs::path &Prefix) {
  if (PendingRemovals.empty())
    return;
  std::array<wchar_t, MAX_PATH> System{};
  if (!GetSystemDirectoryW(System.data(), static_cast<UINT>(System.size())))
    failWindows("Find Windows command processor");
  auto Shell = fs::path(System.data()) / "cmd.exe";
  auto Timer = fs::path(System.data()) / "ping.exe";
  std::vector<wchar_t> Environment;
  auto Raw = GetEnvironmentStringsW();
  if (!Raw)
    failWindows("Read cleanup environment");
  for (const wchar_t *P = Raw; *P; P += wcslen(P) + 1) {
    std::wstring_view Value(P);
    constexpr std::wstring_view Prefix = L"llmDelete";
    if (Value.size() >= Prefix.size() &&
        equalCi(Value.substr(0, Prefix.size()), Prefix))
      continue;
    Environment.insert(Environment.end(), Value.begin(), Value.end());
    Environment.push_back(0);
  }
  FreeEnvironmentStringsW(Raw);
  std::wstring Remove, Completed, Directories;
  auto Add = [&](std::wstring Key, const fs::path &Path) {
    auto Entry = Key + L"=" + Path.wstring();
    Environment.insert(Environment.end(), Entry.begin(), Entry.end());
    Environment.push_back(0);
    return L"\"%" + Key + L"%\"";
  };
  for (size_t I = 0; I < PendingRemovals.size(); ++I) {
    auto Reference =
        Add(L"llmDeleteFile" + std::to_wstring(I), PendingRemovals[I]);
    Remove += L"del /f /q " + Reference + L" 2>nul & ";
    Completed += L"if not exist " + Reference + L" ";
  }
  size_t I = 0;
  for (const auto &Path :
       {Prefix, Layout.ProgramRoot / "versions",
        Layout.CommandPath.parent_path(), Layout.ProgramRoot})
    Directories += L"rd " +
                   Add(L"llmDeleteDirectory" + std::to_wstring(I++), Path) +
                   L" 2>nul & ";
  Environment.push_back(0);
  auto CommandPath = quoteArg(Shell.wstring()) +
                     L" /d /v:off /s /c \"for /l %i in (1,1,120) do @(" +
                     Remove + Completed + L"(" + Directories +
                     L"exit /b 0) & " + quoteArg(Timer.wstring()) +
                     L" -n 2 127.0.0.1 >nul)\"";
  STARTUPINFOW Startup{};
  Startup.cb = sizeof(Startup);
  PROCESS_INFORMATION Child{};
  DWORD CreationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  BOOL InJob = FALSE;
  if (!IsProcessInJob(GetCurrentProcess(), nullptr, &InJob))
    failWindows("Inspect cleanup process job");
  if (InJob) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                   &Limits, sizeof(Limits), nullptr))
      failWindows("Inspect cleanup job limits");
    if (Limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK)
      CreationFlags |= CREATE_BREAKAWAY_FROM_JOB;
  }
  if (!CreateProcessW(Shell.c_str(), CommandPath.data(), nullptr, nullptr,
                      FALSE, CreationFlags, Environment.data(), System.data(),
                      &Startup, &Child))
    failWindows("Start final executable cleanup");
  CloseHandle(Child.hProcess);
  CloseHandle(Child.hThread);
  PendingRemovals.clear();
}
void activateCommand(const fs::path &Source, const fs::path &CommandPath) {
  rejectSymlinkAncestors(CommandPath);
  if (fs::exists(CommandPath) && sha256File(CommandPath) == sha256File(Source))
    return;
  auto Temporary =
      CommandPath.parent_path() / wide(".activate-" + uniqueId() + ".exe");
  fs::copy_file(Source, Temporary);
  if (!MoveFileExW(nativePath(Temporary).c_str(),
                   nativePath(CommandPath).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    auto Error = GetLastError();
    std::error_code Ignored;
    fs::remove(Temporary, Ignored);
    failWindows(
        "Activate llm (close running installed llm processes and retry)",
        Error);
  }
}
}
