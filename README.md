# llm - LLVM Make

llm 1.0.0 builds C, C++ and assembly projects with an LLVM toolchain. A project is a
folder containing a `.llm` file: it declares what to build, its dependencies and
any settings that differ from the defaults.

llm finds source files, resolves dependencies and creates a build graph.
Ninja schedules that graph. Clang and LLVM tools compile, assemble, archive and
link the result. C++ named modules and their dependencies are part of the build.

The goal is a consistent workflow on Windows, Linux and macOS with small,
declarative project files. You do not need to maintain a list of every source
file, write a build script for an ordinary project or install a separate Ninja.

llm is not a compiler, an IDE or an operating-system package manager. It cannot
make platform-specific source code portable or execute a foreign target's binary.
LLVM Make is an independent project, not an official LLVM project.

## Contents

- [Installation and platforms](#installation-and-platforms)
- [Create and run a project](#create-and-run-a-project)
- [The .llm file](#the-llm-file)
- [Libraries and dependencies](#libraries-and-dependencies)
- [Tests](#tests)
- [Command reference](#command-reference)
- [Cross-compilation](#cross-compilation)
- [Build outputs and incremental builds](#build-outputs-and-incremental-builds)
- [Building llm itself](#building-llm-itself)
- [Security](#security)
- [License](#license)

## Installation and platforms

Download [llm 1.0.0](https://github.com/llm-build-system/llm/releases/tag/v1.0.0)
for your **host**: the operating system and CPU that will run llm. A **target** is
the platform you are building an application for. The target defaults to the host.

Choose `llm-1.0.0-<target>.tar.gz` for your host. Each archive contains the
executable, LICENSE and the original licenses and notices of its bundled
libraries under `licenses/`. Extract the complete archive. The installer requires
these license files beside the executable and preserves them unchanged in the
installation. The separate `libc++` archives are downloaded automatically by
`llm --install`.

| Platform | CPU | Target name | Runtime |
| --- | --- | --- | --- |
| Windows | x64 | `x86_64-pc-windows-msvc` | Microsoft |
| Windows | x64 | `x86_64-w64-windows-gnu` | MinGW-w64, UCRT |
| Windows | ARM64 | `aarch64-pc-windows-msvc` | Microsoft |
| Windows | ARM64 | `aarch64-w64-windows-gnu` | MinGW-w64, UCRT |
| Linux | x64 | `x86_64-unknown-linux-gnu` | glibc |
| Linux | x64 | `x86_64-unknown-linux-musl` | musl |
| Linux | ARM64 | `aarch64-unknown-linux-gnu` | glibc |
| Linux | ARM64 | `aarch64-unknown-linux-musl` | musl |
| macOS | ARM64 | `aarch64-apple-darwin` | Apple |

Windows defaults to the Microsoft target and Linux defaults to glibc. The five
host downloads use these defaults. Other target SDKs are installed explicitly:

```sh
llm --install --target x86_64-w64-windows-gnu
llm build --target x86_64-w64-windows-gnu
llm --check-installation --target x86_64-w64-windows-gnu
llm --uninstall --target x86_64-w64-windows-gnu
```

Accepted LLVM aliases are `amd64` for `x86_64`, `arm64` for `aarch64`,
`win32` for `windows`, and `macos` or `macosx` for `darwin` on Apple targets.
GNU Windows accepts either `w64` or `pc` as the vendor, as well as the traditional
`x86_64-w64-mingw32` and `aarch64-w64-mingw32` forms with architecture aliases.
Aliases share the canonical target's installation, dependencies and output paths.
Versioned triples and other ABI variants are rejected.

All targets use Clang, LLD and libc++. Each ABI has its own SDK and matching
runtime. GNU Windows links its C++ runtime statically and uses the Windows UCRT.
musl uses its own C library and dynamic loader. To build a self-contained musl
executable, select static linking explicitly with `flags.link = ["-static"]`.
Runtime and SDK selection never switches ABI implicitly.

Extract the download. On Windows, open PowerShell in the extracted directory:

```powershell
.\llm.exe --install
```

Open a new terminal afterwards to pick up the installed command on `PATH`.
On Linux and macOS:

```sh
chmod +x llm
./llm --install
```

Installation downloads, verifies and extracts the tools, native target SDK and
prebuilt runtime. Users do not need an existing compiler, CMake or Ninja.

Choose a different installation directory with `--path`:

```sh
./llm --install --path "$HOME/tools/llm"
```

On Windows, for example:

```powershell
.\llm.exe --install --path "D:\Tools\llm"
```

The directory contains `bin/llm` (`bin/llm.exe` on Windows), versioned
installations under `versions/` and the download cache. A custom installation
requires write access to this directory, without administrator privileges or
sudo when it is user-writable. The installer registers its `bin/` directory
in the user PATH. Open a new terminal after installation.
The installed command locates its toolchain relative to its own location.
Checking and uninstalling use the invoked installation. These operations
also accept `--path` to select a location explicitly. Adding a target through
the installed command uses that installation.

Installation defaults to the current user. The default directories are
`%LOCALAPPDATA%\llm` on Windows, `~/.local/share/llm` on Linux and
`~/Library/Application Support/llm` on macOS. Windows registers the command in
the user `PATH`. Linux and macOS configure the account's Bash, Zsh or Fish startup
files. Bash uses `.bashrc` and its active login profile. Zsh honors exported
`ZDOTDIR`, and Fish honors `XDG_CONFIG_HOME`. Other login shells produce an explicit
installation error. Existing entries are preserved, repeated installation adds no
duplicates, and uninstall removes only entries recorded as created by llm.

Use `llm --install --global` for a system-wide installation. On Linux and
macOS, run `sudo ./llm --install --global`. On Windows, global installation
requests administrator authorization. `--global` and `--path` are mutually
exclusive. The installed command adds targets to its own installation.

After installation, use a normal terminal for project work:

```sh
llm --version
llm --check-installation
```

The pinned host tools are LLVM/Clang 23.1.3, LLDB 23.1.3 and Ninja 1.13.2.
The installer includes the debugger runtime, including Python where needed. All nine
targets use libc++. The runtime source is pinned to upstream commit
`759327d9a39e6bbcd31383f16396dde04fa424a6` (24.0.0git). Linux and macOS also use
libc++abi and libunwind from that commit. GNU Windows also uses libc++abi and
libunwind. Microsoft targets retain the Microsoft ABI and required CRT libraries.
The operating-system SDK is specific to the target.

## Create and run a project

```sh
llm new hello
cd hello
```

The new directory contains:

```text
hello/
├── .llm
├── .gitignore
└── src/
```

The generated `.llm` contains the only required declaration:

```toml
type = "executable"
```

`src/` starts empty. Add `src/main.cpp`:

```cpp
import std;

int main() {
  std::cout << "hello from llm\n";
}
```

```sh
llm run
```

llm builds the application and runs it. The application prints `hello from llm`.
Run `llm build` again without changing inputs: Ninja reports no work to do.

For a C application, use a `.c` source instead:

```c
#include <stdio.h>

int main(void) {
  puts("hello from C");
  return 0;
}
```

C projects do not need C++ modules. Do not keep both example `main` functions in
the same project.

## The `.llm` file

`.llm` uses TOML: `key = value`, strings in quotes, arrays in brackets and
`# comments`. Tables such as `[dependencies]` group related settings. Root
settings must appear before the first table, or TOML will assign them to the
preceding table.

There is no `[project]` wrapper, required source list or project version field.
Unknown declarations are errors. Paths are relative to the directory containing
that `.llm`, unless a particular setting is documented otherwise. Use `/` in
portable paths, including on Windows.

### Project declarations

```toml
type = "executable"
name = "hello"
standards = ["C23", "C++23"]
excludes = ["scratch/**"]
```

| Declaration | Required | Meaning |
| --- | --- | --- |
| `type` | Yes | The product to build. See the table below |
| `name` | No | Product name. Defaults to the project directory's name |
| `standards` | No | One language standard for C and/or C++. Defaults to C23 and C++23 |
| `sources` | No | Explicit source paths or patterns in addition to automatic selection |
| `excludes` | No | Source paths or patterns to omit |
| `resources` | No | Files, directories or patterns to include when packaging |
| `libraries` | No | System libraries or existing library files to link |

Supported product types:

| `type` | Result |
| --- | --- |
| `"executable"` | An application with an entry point |
| `"static-library"` | A compiled library linked into another product |
| `"shared-library"` | A DLL, `.so` or `.dylib` |
| `"header-only-library"` | Headers and usage requirements, with no compiled sources |
| `"test"` | An executable discovered and run by `llm test` |

Dependencies must refer to library projects, not executables or tests.
A header-only project must not contain selected compiled sources.

Accepted standards are `C89`, `C90`, `C99`, `C11`, `C17`, `C23`, `C++98`,
`C++03`, `C++11`, `C++14`, `C++17`, `C++20` and `C++23`. Choose at most one per
language. Selecting a C++ standard does not turn C sources into C++ sources.

### Source discovery and patterns

llm recursively discovers these source extensions:

| Language | Extensions |
| --- | --- |
| C | `.c` |
| C++ | `.cpp`, `.cc`, `.cxx`, `.C`, `.CPP`, `.CC`, `.CXX`, `.cp`, `.c++` |
| C++ module interfaces | `.ixx`, `.cppm`, `.mpp`, `.ccm`, `.cxxm`, `.c++m` |
| Assembly | `.s`, `.S` |

`src/` is a convention, not a requirement. Sources may be elsewhere in the project.
`bin/`, `obj/` and `.git/` are excluded from discovery. A nested directory with
its own `.llm` is a separate project and is not absorbed into its parent.
A directory called `tests/` has no special source-selection meaning without its
own project file.

You normally omit `sources`. It can select files explicitly for a platform.
Matching files assigned only to inactive platforms are not selected automatically.
Generated source outputs declared by `[[generate]]` are also selected automatically.
`excludes` takes precedence over source selection.

Source, resource and generator-input patterns stay inside the project. They do
not accept absolute paths or `..`. To use another project, declare a dependency.
`*` matches within one path component, `?` matches one non-separator character,
and `**` matches across directories. For example:

```toml
excludes = ["scratch/**", "src/old-*.cpp"]
```

### Includes, definitions and visibility

```toml
[includes]
private = ["src/detail"]
public = ["include"]

[defines]
private = ["ENABLE_TRACE=1"]
public = ["MYLIB_STATIC"]
```

Both `private` and `public` apply when compiling the declaring project.
`public` requirements are also exported to projects depending on that library.
`private` requirements are not. Definitions omit the compiler's `-D` prefix.

When present in the source inventory, `src/` is an implicit local include
directory and `include/` is an implicit include directory. A library exports its
`include/` directory to consumers. Declare `[includes]` for other locations.
Generated headers' output directories are added to the declaring project's
include search path.

### Dependencies and registries

```toml
[registries]
official = "https://github.com/llm-build-system/llm-registry"

[dependencies]
private = ["official:json@3.12.0", "../utility"]
public = ["official:toml@3.4.0"]
```

A local dependency refers to a directory containing another `.llm`. Relative
local dependency paths can use `../`. That differs from source patterns.
A registry reference has the form `alias:package@requirement`.

`private` dependencies are available to the declaring project. `public`
dependencies additionally expose their public headers, definitions and modules
to consumers. A private dependency can still be needed at link time. `private`
does not mean it is absent from the final application.

See [Libraries and dependencies](#libraries-and-dependencies) for version
selection, authentication and the lockfile.

### Platform settings

Settings at the root apply to all targets. Add target-specific settings in:

- `[windows]`, `[linux]`, `[macos]`
- `[windows.x86_64]`, `[windows.aarch64]`
- `[linux.x86_64]`, `[linux.aarch64]`
- `[windows.msvc]`, `[windows.gnu]`
- `[linux.gnu]`, `[linux.musl]`

macOS ARM64 uses `[macos]` directly.
These tables describe the **target**, even during a cross-build.
Settings merge in order from root to operating system, architecture and ABI.

```toml
type = "executable"

[windows]
excludes = ["src/platform-posix.cpp"]
libraries = ["user32"]

[windows.defines]
private = ["WIN32_LEAN_AND_MEAN", "NOMINMAX"]

[linux]
excludes = ["src/platform-windows.cpp"]
libraries = ["pthread"]

[macos]
excludes = ["src/platform-windows.cpp"]
libraries = ["CoreFoundation.framework"]
```

Root settings are applied first, then the OS settings, then the architecture
settings. Lists are appended. A later language standard replaces the earlier
one. For `[inputs]`, a later entry for the same source replaces its earlier list.
The test settings `arguments`, `working-directory` and `timeout` are replaced
when redeclared. Environment entries are merged by variable name.

The same grouped declarations can be scoped, for example
`[linux.aarch64.flags]` or `[windows.includes]`. Project `type`, `name` and
`[registries]` are declared at the root, not inside platform settings.

### Compiler flags and system libraries

```toml
[flags]
compile = ["-Wall", "-Wextra"]
c = ["-Wstrict-prototypes"]
cxx = ["-Wconversion"]
asm = []
link = []
```

Each array element is one argument. `compile` applies to C and C++.
`c`, `cxx` and `asm` apply to their respective languages. `link` applies to the
link command. These are LLVM/Clang options, not translated GCC/MSVC settings.

Use `standards` and `--target` for language and target selection. llm reserves
compiler controls for output paths, toolchain selection, module mappings and
depfiles. Overriding those through raw flags is rejected. Debug is the default
profile (`-O0`, debug information). `--release` selects `-O2` and `NDEBUG`.

Use `libraries` for external link requirements, normally scoped by platform:

```toml
[windows]
libraries = ["user32", "vendor/lib/example.lib"]

[linux]
libraries = ["pthread", "vendor/lib/libexample.a"]

[macos]
libraries = ["CoreFoundation.framework"]
```

Named Microsoft-ABI libraries are resolved from the target SDK. GNU Windows,
Linux and macOS names are passed to the target linker as library names.
`.framework` selects an Apple
framework. Paths refer to existing libraries. Prefer project dependencies when
llm should build a library's sources and propagate its usage requirements.

### Assembly inputs

`.s` is assembled directly. `.S` uses the C preprocessor first. Both are
discovered automatically. Use `[inputs]` for additional assembler dependencies:

```toml
[inputs]
"src/data.S" = ["src/data.inc", "assets/data.bin"]
```

The keys and additional inputs must be explicit project-relative paths, without
wildcards. List all assembler `.include` and `.incbin` inputs here, including
nested assembler includes. Clang's depfiles track C preprocessor includes in
`.S` automatically.
`[inputs]` can also attach explicit file dependencies to C/C++ sources when needed.

### Generated files

```toml
[[generate]]
inputs = ["schema/message.schema"]
outputs = ["obj/generated/message.cpp", "obj/generated/message.hpp"]
command = ["schema-compiler", "schema/message.schema", "obj/generated/message.cpp", "obj/generated/message.hpp"]
```

`[[generate]]` is an array of rules. Repeat it for additional generators. All
three declarations are required, although `inputs` may be empty. Inputs are
project-relative files or patterns. Outputs must be unique explicit files under
`obj/`. They must not overwrite llm's bookkeeping files.

`command` is a program followed by individual arguments, executed in the declaring
project's directory. It is not an implicit shell command: redirection, pipes and
variable substitution are not interpreted by llm. The example assumes a host
program called `schema-compiler` is available. Use your actual generator and its
arguments. Required generator tools must be provided explicitly.

Generators run before affected scans and translations. A missing declared output
or a failed generator fails the build. Generated source files do not need a
`sources` entry. Generated headers are included through their output directory.

### Resources

```toml
type = "executable"
resources = ["assets/**", "settings/default.toml"]
```

Resources are copied into distribution packages, preserving project-relative
paths. They are not compiled, and declaring them does not copy them next to a
normal local build. Directories include their contents. A pattern matching no
resource is an error.

## Libraries and dependencies

### A local C++ module library

A library project can contain just these files:

```text
hello-from-llm/
├── .llm
└── src/
    └── hello.cppm
```

`.llm`:

```toml
type = "static-library"
name = "hello-from-llm"
```

`src/hello.cppm`:

```cpp
export module hello;

export namespace hello {
const char *message() {
  return "hello from llm";
}
}
```

In a sibling application, add:

```toml
type = "executable"

[dependencies]
private = ["../hello-from-llm"]
```

Its `src/main.cpp` can import the library's module:

```cpp
import std;
import hello;

int main() {
  std::cout << hello::message() << '\n';
}
```

`llm run` builds the library and application in the correct order and prints
`hello from llm`. No hand-written module compile order or linker command is
needed. To expose a dependency through another library's interface, declare
that dependency as `public`.

### Registry packages and versions

Declare one or more aliases in `[registries]`, then use
`alias:package@requirement`. With exactly one registry the alias can be omitted.
With multiple registries it is required. The source registry remains part of a
package's identity, so equally named packages from different registries remain
distinct.

| Requirement | Meaning |
| --- | --- |
| `@3.12.0` | Exactly this version |
| `@3.12.*` | Any stable patch version matching 3.12 |
| `@3.*.*` | Any stable version matching major version 3 |
| `@*` or omitted | Any stable version |
| `@^1.2.3` | Compatible updates: `[1.2.3, 2.0.0)` |
| `@~1.2.3` | Patch updates: `[1.2.3, 1.3.0)` |
| `@[1.2.3, 2.0.0)` | Include 1.2.3, exclude 2.0.0 |
| `@[1.2.3, 2.0.0]` | Include both endpoints |
| `@(1.2.3, 2.0.0)` | Exclude both endpoints |
| `@(1.2.3, 2.0.0]` | Exclude 1.2.3, include 2.0.0 |
| `@[1.2.3, *)` | At least 1.2.3, without an upper limit |
| `@(1.2.3, *)` | Greater than 1.2.3, without an upper limit |
| `@(*, 2.0.0)` | Any stable version below 2.0.0 |
| `@(*, 2.0.0]` | Any stable version up to and including 2.0.0 |

Square brackets include a finite endpoint. Round brackets exclude it. Inside an
interval, `*` means unbounded and must have a round bracket on its side.
`(*, *)` accepts any stable version, like `*`. Both bounds must be written.
`[1.2.3,)` and `[,2.0.0)` are invalid. Spaces and tabs around the bounds are
optional, so `[1.2.3,2.0.0)` and `[1.2.3, 2.0.0)` mean the same thing.

The lower bound must not exceed the upper bound. Equal finite bounds require
both square brackets, such as `[1.2.3, 1.2.3]`. For an exact package version,
normally write `1.2.3` directly, without brackets.

Caret (`^`) ranges keep the leftmost nonzero component fixed: `^0.2.3` is at
least 0.2.3 and below 0.3.0. `^0.0.3` is at least 0.0.3 and below 0.0.4.
`^0.0.0` selects only stable 0.0.0. Tilde (`~`) ranges keep the major and minor
components fixed, including for `0.x` versions.

`^` and `~` must be immediately followed by a complete `major.minor.patch`
version. Finite interval bounds also require complete versions. SemVer build
metadata is permitted: ranges use version precedence, ignoring build metadata.
Exact requirements retain the complete version identity.

Comparison operators (`>`, `>=`, `<`, `<=`, `=`), empty interval bounds,
single-bound forms such as `[1.2.3]`, partial versions such as `^1.2`, and OR
operators (`|`, `||`) are not supported. Use an interval instead of combined
comparisons. Prereleases still require an exact version. They are not selected
by wildcards, caret/tilde ranges or intervals. Compatible requirements across
the graph must resolve together. A conflict is an error.

Quote dependency arguments containing operators to prevent shell interpretation:

```sh
llm add "official:json@^3.12.0"
llm add "official:toml@~3.4.0"
llm add "official:curl@[8.23.0, 9.0.0)"
llm add "official:json@[3.12.0, *)"
```

For example, with `official` declared:

```sh
llm add official:json@3.12.0
```

llm updates `[dependencies]`, resolves the package and records selected versions.
The json package provides its headers, so a source can use
`#include <nlohmann/json.hpp>` without copying the library into the application.

For registries requiring authentication, log in from a project declaring the
alias:

```sh
llm login official
```

Enter a GitHub token with read access to the registry and package repositories
at the prompt. Tokens are stored outside the project. Never put credentials in
`.llm`, `.lock`, a registry URL or source control. `llm logout official` removes
the saved credential for that registry.

Registry and package addresses support `https://`, `http://` and `file://`.
Token authentication requires HTTPS. A remote registry cannot reference local
file archives. Local file URLs are absolute, such as `file:///path/to/registry`
or `file:///C:/path/to/registry` on Windows.

### The `.lock` file

`.llm` declares acceptable versions. `.lock` records the selected versions and
source archive SHA-256 values. Commit both files. A normal build reuses valid
locked selections. `llm update` resolves again within the declared requirements.
An exact requirement does not become a newer version just because `update` runs.
Intervals and caret/tilde ranges also reuse the exact locked version during
normal builds. `llm update` may select a newer version that still satisfies the
range.

Lockfile blocks group packages under their registry URL:

```text
https://github.com/llm-build-system/llm-registry
json@3.12.0:<sha256>
toml@3.4.0:<sha256>
```

`<sha256>` represents the actual 64-character digest written by llm. It is not
literal lockfile content. llm verifies source archives and caches package sources
and per-target project instances outside your project tree. You do not manually
copy registry sources into a `deps/` folder.

## Tests

A test is an explicit `.llm` project with `type = "test"`. It can be a standalone
program or depend on libraries under test. No test framework is required: exit
code zero means success, and a nonzero exit code means failure.

From the parent project:

```sh
llm new tests --type test
```

Create `tests/src/main.cpp` with the test entry point. A complete test manifest
could be:

```toml
type = "test"
name = "utility-tests"
arguments = ["--quick"]
working-directory = "."
timeout = 10.0

[dependencies]
private = ["../../utility"]

[environment]
TEST_MODE = "local"
```

Here `../../utility` is an example path to a separate library project. Replace
it with the library you test. Dependencies are declared in the test project's
own `.llm`, not inherited automatically from the parent application.

| Test-only declaration | Meaning |
| --- | --- |
| `arguments` | Arguments passed to each test invocation. Defaults to none |
| `working-directory` | Runtime directory. Relative to the test project, default `.` |
| `timeout` | Optional positive finite seconds, at least 0.001. No configured deadline when omitted |
| `[environment]` | Environment variable overrides for the test process |

These declarations can also be platform-scoped and are invalid for other product
types. Command-line arguments after `--` are appended to manifest arguments.
`llm test --timeout` overrides the manifest timeout.

`llm test` discovers nested test projects without descending through a different
nested project boundary. Test directory names are arbitrary. `tests/` is only a
convention. Normal parent builds do not absorb sources from nested test projects.

## Command reference

Commands find the nearest `.llm`, starting in the current directory and searching
its parents. Use `-p PATH` to select a directory containing `.llm` explicitly.
That selection does not search its parents. Flags belong after the command,
for example `llm build --release`, not `llm --release build`.

### Common options

| Option | Available on | Effect |
| --- | --- | --- |
| `-p PATH`, `--project PATH` | Commands except `new` | Select the project directory |
| `-v`, `--verbose` | Commands except `new` | Show executed commands/details |
| `-q`, `--quiet` | Commands except `new` | Suppress progress/success messages |
| `--color auto\|always\|never` | Commands except `new` | Select diagnostic color |
| `-h`, `--help` | All commands | Show usage without performing the command |
| `-t TRIPLE`, `--target TRIPLE` | `build`, `rebuild`, `run`, `debug`, `check`, `test`, `analyze`, `package`, `list` | Select the target. Execution/analysis still require the native target |
| `-r`, `--release` | The same commands as `--target`, except `debug` | Select Release instead of Debug |
| `-j N`, `--jobs N` | Build commands, including `test`, `analyze`, `package` | Limit parallel work to a positive integer |

`--quiet` and `--verbose` cannot be combined. Value options accept separated
values (`--jobs 4`) or `=` (`--jobs=4`). Compact forms such as `-j4` are not
supported. `new` accepts its destination as a positional path and `--type`.

### `llm new [PATH] [--type TYPE]`

Create `.llm`, an empty conventional `src/` and `.gitignore` entries for build
outputs. The path defaults to the current directory. The type defaults to
`executable`. Existing project files are not overwritten. No `main.cpp` or test
framework is generated.

```sh
llm new app
llm new utility --type static-library
llm new tests --type test
```

All five product types in the manifest reference are accepted by `--type`.

### `llm build [options]`

Resolve declared dependencies and build the selected product and its dependency
graph. Debug is the default. Existing outputs are rebuilt when their tracked
inputs or commands change. This command does not run the application.

```sh
llm build
llm build --release -j 4
llm build -p ../utility
```

### `llm rebuild [options]`

Remove the selected project's tracked outputs for the selected target/profile,
then build again. Other target/profile directories are not selected for removal.
This is different from `clean`, which has no target/profile selector.

```sh
llm rebuild --release
```

### `llm run [options] [-- ARGUMENTS...]`

Build an `executable` project and run it on the host. Arguments after `--` are
passed to the application, even if they look like llm options. The application's
working directory is the directory from which llm was invoked, including when
`-p` selects a different project. Its exit code is returned by llm.

```sh
llm run -- --name example
llm run --release
llm run --no-build -- --help
```

`--no-build` runs an existing output without checking whether it is up to date.
It fails if the executable is missing. Without that option, a failed build does
not run a stale executable. Foreign targets cannot be run.

### `llm debug [options] [-- ARGUMENTS...]`

Build the selected native `executable` project in Debug mode and open LLDB in
the current terminal. A failed build does not open the debugger or run an older
executable. The program is loaded but does not start until you enter `run` in
LLDB. Arguments after `--` belong to your program. Its working directory is the
directory from which llm was invoked. It inherits your program environment.

```sh
llm debug
llm debug --project app --jobs 8
llm debug -- arg1 "argument with spaces"
```

At the LLDB prompt:

```text
breakpoint set --name main
run
next
frame variable
bt
continue
quit
```

Use `step` to enter a function, `finish` to leave it and `expression -- VALUE`
to evaluate an expression. Ctrl+C interrupts the running program and returns to
the debugger. llm returns LLDB's exit code, which is not automatically the exit
code of the debugged program.

This command requires an interactive terminal. Foreign targets, libraries,
`--release` and `--no-build` are not accepted. It selects the program from your
project rather than accepting arbitrary executables or process IDs.

On macOS, native Debug executables receive the `get-task-allow` entitlement
through Apple's `codesign`. macOS may require developer authorization. For a
headless development machine, an administrator can explicitly enable it with
`sudo /usr/sbin/DevToolsSecurity -enable`. llm does not change that system policy.

### `llm check [options]`

Compile selected sources and dependencies without the final archive/link step.
This checks actual translation, including module dependencies. It is more than
checking TOML syntax. It does not execute the product.

```sh
llm check
llm check --target aarch64-unknown-linux-gnu
```

### `llm test [SUITE] [options] [-- ARGUMENTS...]`

Build and run the associated explicit test projects. By default all discovered
suites are selected. A positional `SUITE` is its project-relative directory path,
such as `tests/unit`. Use `-p` to run a test project directly.

```sh
llm test
llm test tests/unit
llm test --list
llm test --filter 'tests/**' --timeout 20 -j 4
llm test -p tests/unit -- --quick
```

| Test option | Effect |
| --- | --- |
| `--list` | Print selected suite names without building or running them |
| `--filter GLOB` | Select suites whose relative path matches the pattern |
| `--all` | Explicitly select all associated suites. Cannot accompany `SUITE` |
| `--timeout SECONDS` | Override each suite's configured timeout |
| `--no-build` | Run existing test binaries without building |
| `-- ARGUMENTS...` | Append arguments to each selected suite's manifest arguments |

`-j N` also limits concurrently running test processes. A failing process,
preparation error or timeout fails the test run. Tests require the native target.
If no matching tests exist, llm reports that no tests were found.

### `llm format [PATH...] [--check] [options]`

Format C/C++ sources, module interfaces and headers with clang-format. With no
paths, format the project's eligible files and associated test projects. Paths
restrict selection to files/directories inside the selected project. Assembly is
not formatted.

```sh
llm format
llm format src include
llm format --check
```

An explicit `.clang-format` or `_clang-format` must be available in the source's
ancestor directories. `.clang-format-ignore` rules are honored. `--check` leaves
files unchanged and fails if formatting changes would be needed. Use `--` before
file names that begin with `-`.

### `llm analyze [options]`

Prepare the compiler/module context and run clang-tidy on the selected project's
C/C++ sources. Dependency sources are not individually analyzed as application
sources. Analysis uses the build's arguments and module assignments.

```sh
llm analyze --checks='-*,bugprone-*,performance-*'
llm analyze --tests --warnings-as-errors='bugprone-*'
```

| Analysis option | Effect |
| --- | --- |
| `--tests` | Include associated test projects |
| `--checks GLOBS` | Select clang-tidy checks |
| `--warnings-as-errors GLOBS` | Treat matching warnings as errors |

Use the project's `.clang-tidy` for persistent settings. Analysis requires the
native target and may compile inputs needed for modules. It is not a text-only
lint pass.

### `llm list [--json] [options]`

Show the effective project settings, selected sources, dependencies and test
projects. `--target` changes which platform settings are displayed. `--json`
writes machine-readable information to standard output. It does not build the
product or fetch missing dependencies just to list them. Unresolved references
can be shown as unresolved.

```sh
llm list
llm list --target aarch64-pc-windows-msvc
llm list --json
```

### `llm clean [options]`

Remove outputs tracked as belonging to the selected project, including its
tracked target/profile outputs. Source files, `.llm` and `.lock` remain.
Dependency caches outside the project are not uninstalled. `clean` does not accept
`--target` or `--release`. Use `rebuild` to rebuild one selected profile.

```sh
llm clean
llm clean -p ../utility
```

### `llm package [options]`

Build and create a `.tar.gz` distribution in
`bin/<target>/<profile>/<name>-<target>.tar.gz`. Executables include their required
project runtime libraries and declared resources. Library packages include the
interface and metadata needed by consumers. Prebuilt library packages are
bound to their target, profile and pinned toolchain. Test products cannot be
packaged for distribution.

```sh
llm package --release
llm package --target aarch64-unknown-linux-gnu --release
```

A failed build is not packaged. Temporary staging stays under `obj/`. The finished
archive goes to `bin/`. This command creates a local archive, not a registry entry
or GitHub release, and does not upload anything.

### `llm add DEP [--public] [options]`

Add or change a direct dependency in `.llm` and resolve the corresponding lockfile
selection. Dependencies are private by default. `--public` records a public
library dependency. Local project paths and registry references are supported.

```sh
llm add ../utility
llm add official:json@3.12.0
llm add official:toml@3.4.0 --public
```

The registry alias must already be declared in `.llm`. This command does not
publish a package.

### `llm remove DEP [options]`

Remove a direct dependency declaration and update the lockfile consistently.
A registry package can be identified by its qualified name without a version.
This does not delete the upstream repository or uninstall the toolchain.

```sh
llm remove ../utility
llm remove official:json
```

### `llm update [options]`

Resolve registry dependencies again within the requirements declared in `.llm`
and update `.lock`. Associated tests are included when updating a parent project.
It does not upgrade the compiler, modify upstream sources or widen an exact
package requirement.

```sh
llm update
```

### `llm login REGISTRY` and `llm logout REGISTRY`

`REGISTRY` is an alias declared in the selected project's `[registries]` table.
`login` prompts for a token, validates access and saves it outside the project.
`logout` removes the saved credential. Token login requires HTTPS.

```sh
llm login official
llm logout official
```

Use `-p PATH` when authenticating from a different project. Access-controlled
GitHub registries need read access to both the index and referenced package
repositories.
No token belongs in command examples or project configuration.

### Global installation and information options

These options follow `llm` directly, without a subcommand:

| Invocation | Effect |
| --- | --- |
| `llm --help` | Show commands and global options |
| `llm --version` | Print llm's version |
| `llm --version --verbose` | Also show component information |
| `llm --install` | Install this release's host tools and native target |
| `llm --install --global` | Install system-wide with administrator privileges |
| `llm --install --path PATH` | Install in the selected directory |
| `llm --install --target TRIPLE` | Add the explicitly selected target SDK |
| `llm --check-installation` | Verify the native installation |
| `llm --check-installation --target TRIPLE` | Verify host tools and the selected target |
| `llm --uninstall --target TRIPLE` | Remove an optional target SDK |
| `llm --uninstall` | Remove this llm release while preserving other versions and caches |

System installation and uninstallation require administrator privileges on
Windows or `sudo` on Linux/macOS. A custom installation needs write access to
its directory. `--install`, `--check-installation` and `--uninstall` accept
`--global` or `--path PATH`, including when selecting an optional target.
An extracted download installs in the user directory by default. The installed
command uses its own installation unless a location is selected explicitly.
Optional targets require an existing native installation.
Remove the native SDK by uninstalling the whole release, not as an optional target.
Missing or damaged required installation data is an error. Builds do not silently
install or switch toolchains.

## Cross-compilation

Install host tools once and explicitly add the SDK for another target:

```sh
llm --install --target aarch64-unknown-linux-gnu
llm --check-installation --target aarch64-unknown-linux-gnu
llm build --target aarch64-unknown-linux-gnu --release
```

Use elevation for the installation step as described above. Host generator
programs still run on the host. `build`, `rebuild`, `check`, `package` and `list`
support foreign targets. `run`, `debug`, `test` and `analyze` require a compatible
native target. Windows supports both Microsoft and GNU targets for its native
CPU. Linux execution requires the native C-runtime target, so musl builds on a
glibc host are cross-builds even when the CPU matches.
Cross-compilation does not replace target-specific libraries or source selection.

## Build outputs and incremental builds

Products live under `bin/<target>/<debug|release>/`. `obj/` contains intermediate
outputs separated by target, profile and project: the Ninja graph, Clang objects,
compiled modules and scans, plus module argument files and Ninja dyndep data.
Standard modules have their own compiler-context directory under `obj/.../std/`.
Ninja maintains its own incremental log and dependency database.

llm starts Ninja once per graph. Generator actions, scans, module assignment,
compilation and linking are scheduled in that graph. C and C++ header dependencies
come from Clang depfiles. A normal build does not reread scans/module arguments
after Ninja. Analysis and packaging request the information they need explicitly.

`obj/.owned` records relative output paths for safe cleanup. `obj/.guard` prevents
conflicting simultaneous operations. Keep `bin/` and `obj/` out of source control.
Commit `.llm`, `.lock`, source files and relevant tool configuration. Missing
required tools or corrupt cached data produce an error rather than an implicit
replacement build path.

Known limitations include automatic header-unit imports and the known Clang
Windows named-module `_alloca` ownership defect. libc++ standard modules avoid
the failing Microsoft standard-module declaration path, but user modules can
still encounter the compiler defect. No custom compiler patches are applied.

## Building llm itself

This repository uses the same project model as user projects. Install llm 1.0.0
as described above, then obtain its source:

```sh
git clone --branch v1.0.0 https://github.com/llm-build-system/llm.git
cd llm
```

Build the release configuration:

```sh
llm build --release
```

Use an installed executable outside this checkout's `bin/` directory. Windows
cannot replace an executable that is currently running. Git is needed to clone
the repository. Compilation uses the toolchain installed by `llm --install`.

## Security

Report vulnerabilities privately through
[GitHub](https://github.com/llm-build-system/llm/security/advisories/new).
Do not post security-sensitive details in public issues or discussions. See the
[security reporting policy](https://github.com/llm-build-system/.github/blob/main/SECURITY.md).

For general contact and project updates, use [@3754Rx on X](https://x.com/3754Rx).

## License

Copyright © 2026 the llm contributors.

llm's original source code and documentation are licensed under the
Apache License, Version 2.0. See [LICENSE](LICENSE).

Third-party components retain their own licenses and copyright notices.
Each llm release archive contains the executable, llm's `LICENSE` and
`licenses/<product>/LICENSE`, with additional notices where supplied.
Third-party license documents retain their original contents and formats.
The archives include the original licenses of bzip2, curl, Expat, nlohmann/json,
libarchive, OpenSSL, toml++, XZ/liblzma, zlib and Zstandard. Additional notices
from their included source files are collected in each package's `NOTICE`
where applicable.

The separate libc++ runtime archives retain LLVM's original license paths:
`libcxx/LICENSE.TXT`, `libcxxabi/LICENSE.TXT` and `libunwind/LICENSE.TXT`,
for the components included on each platform.
