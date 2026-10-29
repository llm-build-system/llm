module llm.runtime;
import :internal;
import std;

namespace llm::runtime {
namespace fs = std::filesystem;

std::vector<TestSuite> discoverTests(const Manifest &Project) {
  if (Project.Type == ProductType::Test)
    return {{Project, ""}};
  std::vector<TestSuite> Suites;
  for (fs::recursive_directory_iterator Entry(Project.Root), End; Entry != End;
       ++Entry) {
    auto File = Entry->path();
    if (redirectsPath(File)) {
      Entry.disable_recursion_pending();
      continue;
    }
    if (!Entry->is_directory())
      continue;
    auto Relative = File.lexically_relative(Project.Root);
    auto First = *Relative.begin();
    if (First == "obj" || First == "bin" || First == ".git") {
      Entry.disable_recursion_pending();
      continue;
    }
    if (!fs::is_regular_file(File / ".llm"))
      continue;
    Entry.disable_recursion_pending();
    auto Child = readManifest(File);
    if (Child.Type == ProductType::Test)
      Suites.push_back({std::move(Child), pathText(Relative)});
  }
  std::ranges::sort(Suites, {}, &TestSuite::Selector);
  return Suites;
}

namespace {
std::string suiteLabel(const TestSuite &Suite) {
  return Suite.Selector.empty() ? Suite.Project.Name : Suite.Selector;
}
Path testArtifact(const Manifest &Project, const Command &Request) {
  return Project.Root / "bin" / pathFromText(targetName(Request.BuildTarget)) /
         (Request.Mode == Profile::Release ? "release" : "debug") /
         artifactName(Project.Name, ProductType::Executable,
                      Request.BuildTarget);
}
}

int testProject(Context &Call, const Command &Request,
                const Manifest &Project) {
  if (Request.All && !Request.Selection.empty())
    throw Failure("--all cannot be combined with a test suite selector");
  auto Suites = discoverTests(Project);
  if (!Request.Selection.empty() &&
      std::ranges::none_of(Suites, [&](const auto &Suite) {
        return Suite.Selector == Request.Selection;
      }))
    throw Failure("Unknown test suite: " + Request.Selection);
  std::erase_if(Suites, [&](const auto &Suite) {
    return !matchesGlob(Request.Filter, suiteLabel(Suite)) ||
           (!Request.Selection.empty() && Suite.Selector != Request.Selection);
  });
  if (Request.ListOnly) {
    for (const auto &Suite : Suites)
      emit(Call, EventKind::StandardOutput, suiteLabel(Suite) + '\n');
    return 0;
  }
  if (Suites.empty()) {
    emit(Call, EventKind::Note, "no tests found");
    return 0;
  }
  auto Owner = Project.Root;
  std::vector<Manifest> Roots{Project};
  for (const auto &Suite : Suites)
    Roots.push_back(Suite.Project);
  auto Packages = Request.NoBuild
                      ? std::map<std::string, Path>{}
                      : resolveDependencies(Call, Request, Owner, Roots);
  struct Prepared {
    std::string Label;
    Path Executable, Directory;
    Strings Arguments;
    std::map<std::string, std::string> Environment;
    std::chrono::milliseconds Timeout{};
  };
  std::vector<Prepared> PreparedTests;
  std::size_t Failed = 0, Passed = 0;
  if (!Request.NoBuild) {
    std::vector<ProjectGraph> Graphs;
    for (const auto &Suite : Suites)
      Graphs.push_back(
          loadGraph(Call, Suite.Project, Request.BuildTarget, Packages));
    auto Build = Request;
    Build.Action = Operation::Build;
    buildProjects(Call, Build, Graphs, Owner);
  }

  for (const auto &Suite : Suites) {
    cancelled(Call);
    try {
      auto Settings =
          take(effectiveSettings(Suite.Project, Request.BuildTarget));
      auto Executable = testArtifact(Suite.Project, Request);
      rejectSymlinkAncestors(Executable);
      if (!fs::is_regular_file(Executable))
        throw Failure(
            "Test executable is missing. Run llm test without --no-build");
      auto Directory = Settings.WorkingDirectory
                           ? pathFromText(*Settings.WorkingDirectory)
                           : Path(".");
      if (Directory.is_relative())
        Directory = Suite.Project.Root / Directory;
      if (!fs::is_directory(Directory))
        throw Failure("Test working-directory does not exist: " +
                      pathText(Directory));
      Prepared Test{suiteLabel(Suite), Executable, Directory,
                    Settings.TestArguments, Settings.Environment};
      Test.Arguments.insert(Test.Arguments.end(), Request.Arguments.begin(),
                            Request.Arguments.end());
      auto Timeout = Request.Timeout ? Request.Timeout : Settings.Timeout;
      if (Timeout)
        Test.Timeout = std::chrono::milliseconds(
            static_cast<std::int64_t>(*Timeout * 1000.0));
      PreparedTests.push_back(std::move(Test));
    } catch (const Failure &Error) {
      cancelled(Call);
      ++Failed;
      emit(Call, EventKind::Error,
           "Test " + suiteLabel(Suite) +
               ": build/preparation failed: " + Error.what());
    }
  }
  auto Concurrency = Request.Jobs ? static_cast<std::size_t>(Request.Jobs) : 1;
  for (std::size_t Start = 0; Start < PreparedTests.size();
       Start += Concurrency) {
    auto Stop = std::min(PreparedTests.size(), Start + Concurrency);
    struct TestOutput {
      ProcessResult Result;
      std::string Text;
    };
    std::vector<std::future<TestOutput>> Running;
    for (auto Index = Start; Index < Stop; ++Index) {
      const auto *Test = &PreparedTests[Index];
      Running.push_back(std::async(std::launch::async, [&Call, Test] {
        TestOutput Output;
        Output.Result = runProcess(Call, Test->Executable, Test->Arguments,
                                   {.Directory = Test->Directory,
                                    .Timeout = Test->Timeout,
                                    .Environment = Test->Environment,
                                    .CaptureOutput = &Output.Text});
        return Output;
      }));
    }
    for (auto Index = Start; Index < Stop; ++Index) {
      const auto &Test = PreparedTests[Index];
      try {
        auto Output = Running[Index - Start].get();
        auto Result = Output.Result;
        cancelled(Call);
        if (Result.success())
          ++Passed;
        else
          ++Failed;
        if (!Result.success() || Call.Output.Verbose) {
          const auto &Text = Output.Text;
          if (!Text.empty())
            emit(Call, EventKind::ToolOutput,
                 Text + (Text.back() == '\n' ? "" : "\n"));
        }
        auto Status = "Test " + Test.Label + ": " +
                      (Result.success()  ? "passed"
                       : Result.TimedOut ? "timed out"
                                         : "failed");
        emit(Call, Result.success() ? EventKind::Progress : EventKind::Error,
             Result.success()
                 ? Status
                 : Status + " (exit " + std::to_string(Result.ExitCode) + ")");
      } catch (const std::exception &Error) {
        cancelled(Call);
        ++Failed;
        emit(Call, EventKind::Error,
             "Test " + Test.Label + ": " + Error.what());
      }
    }
  }
  emit(Call, EventKind::Progress,
       "Tests: " + std::to_string(Passed) + " passed, " +
           std::to_string(Failed) + " failed.");
  return Failed ? 1 : 0;
}
}
