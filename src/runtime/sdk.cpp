module llm.runtime;
import :internal;
import std;
namespace llm::runtime {
namespace fs = std::filesystem;
ToolchainBundle pinnedBundle(Target BuildTarget) {
  ToolchainBundle Bundle;
  switch (BuildTarget) {
  case Target::WindowsX64:
    Bundle.Id = "LLVM 23.1.3 + native SDK / windows-x86_64";
    Bundle.Packages.push_back(
        {"clang+llvm-23.1.3-x86_64-pc-windows-msvc",
         "https://github.com/llvm/llvm-project/releases/download/"
         "llvmorg-23.1.3/clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.xz",
         "9ee6a2192fbb30c776415af529927f76ca6b5d1d55699bb1315f9878e610b5b8",
         "clang+llvm-23.1.3-x86_64-pc-windows-msvc",
         "",
         "",
         {"bin/clang.exe", "bin/clang++.exe", "bin/clang-scan-deps.exe",
          "bin/clang-format.exe", "bin/clang-tidy.exe", "bin/lld.exe",
          "bin/llvm-ar.exe", "bin/llvm-lib.exe", "bin/llvm-readobj.exe",
          "lib/clang/23/include/stddef.h"},
         {},
         4294967296ULL,
         42949672960ULL,
         true});
    Bundle.Packages.push_back(
        {"Microsoft.VC.14.44.17.14.CRT.Headers.base",
         "https://download.visualstudio.microsoft.com/download/pr/"
         "c610cd8c-801b-44b8-a80a-82cc382aeb43/"
         "852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5/"
         "Microsoft.VC.14.44.17.14.CRT.Headers.base.vsix",
         "852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5",
         "",
         "zip",
         "sdk",
         {"msvc/include/vcruntime.h"},
         {{"Contents/VC/Tools/MSVC/14.44.35207/include/", "msvc/include/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"Microsoft.VC.14.44.17.14.CRT.x64.Desktop.base",
         "https://download.visualstudio.microsoft.com/download/pr/"
         "67cf767c-5e71-47c2-a54a-cd5631e28942/"
         "f01f701a7bcd9587a340898c851424f6a52bb913a70c185ff0d5bf0288c5831a/"
         "Microsoft.VC.14.44.17.14.CRT.x64.Desktop.base.vsix",
         "f01f701a7bcd9587a340898c851424f6a52bb913a70c185ff0d5bf0288c5831a",
         "",
         "zip",
         "sdk",
         {"msvc/lib/libcmt.lib", "msvc/lib/libcpmt.lib",
          "msvc/lib/libvcruntime.lib"},
         {{"Contents/VC/Tools/MSVC/14.44.35207/lib/x64/", "msvc/lib/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"Microsoft.VC.14.44.17.14.CRT.x64.Store.base",
         "https://download.visualstudio.microsoft.com/download/pr/"
         "67cf767c-5e71-47c2-a54a-cd5631e28942/"
         "9135b03c0df53c7a0aa9bef7230a1c2ff4263a0ee7baa7e419d034f484f6bb56/"
         "Microsoft.VC.14.44.17.14.CRT.x64.Store.base.vsix",
         "9135b03c0df53c7a0aa9bef7230a1c2ff4263a0ee7baa7e419d034f484f6bb56",
         "",
         "zip",
         "sdk",
         {"msvc/lib/oldnames.lib"},
         {{"Contents/VC/Tools/MSVC/14.44.35207/lib/x64/", "msvc/lib/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"microsoft.windows.sdk.cpp",
         "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/"
         "10.0.26100.9169/microsoft.windows.sdk.cpp.10.0.26100.9169.nupkg",
         "475269434dcd808a67853773272f972c3229c0e10c3ddc821290e70cc0f6904d",
         "",
         "zip",
         "sdk",
         {"windows/include/um/Windows.h", "windows/include/ucrt/stdio.h"},
         {{"c/Include/10.0.26100.0/", "windows/include/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"microsoft.windows.sdk.cpp.x64",
         "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp.x64/"
         "10.0.26100.9169/microsoft.windows.sdk.cpp.x64.10.0.26100.9169.nupkg",
         "df6226a051e320942abfbd57848b43d18772996ecd66beadad240f2a56ed2f7b",
         "",
         "zip",
         "sdk",
         {"windows/lib/um/kernel32.Lib", "windows/lib/ucrt/libucrt.lib"},
         {{"c/um/x64/", "windows/lib/um/"},
          {"c/ucrt/x64/", "windows/lib/ucrt/"}},
         536870912ULL,
         8589934592ULL,
         false});
    break;
  case Target::WindowsArm64:
    Bundle.Id = "LLVM 23.1.3 + native SDK / windows-arm64";
    Bundle.Packages.push_back(
        {"clang+llvm-23.1.3-aarch64-pc-windows-msvc",
         "https://github.com/llvm/llvm-project/releases/download/"
         "llvmorg-23.1.3/clang+llvm-23.1.3-aarch64-pc-windows-msvc.tar.xz",
         "4a42903db59f990a10c245eb32999f1e3247c37ba66de87f8f8dc2338ed9a79b",
         "clang+llvm-23.1.3-aarch64-pc-windows-msvc",
         "",
         "",
         {"bin/clang.exe", "bin/clang++.exe", "bin/clang-scan-deps.exe",
          "bin/clang-format.exe", "bin/clang-tidy.exe", "bin/lld.exe",
          "bin/llvm-ar.exe", "bin/llvm-lib.exe", "bin/llvm-readobj.exe",
          "lib/clang/23/include/stddef.h"},
         {},
         4294967296ULL,
         42949672960ULL,
         true});
    Bundle.Packages.push_back(
        {"Microsoft.VC.14.44.17.14.CRT.Headers.base",
         "https://download.visualstudio.microsoft.com/download/pr/"
         "c610cd8c-801b-44b8-a80a-82cc382aeb43/"
         "852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5/"
         "Microsoft.VC.14.44.17.14.CRT.Headers.base.vsix",
         "852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5",
         "",
         "zip",
         "sdk",
         {"msvc/include/vcruntime.h"},
         {{"Contents/VC/Tools/MSVC/14.44.35207/include/", "msvc/include/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"Microsoft.VC.14.44.17.14.CRT.ARM64.Desktop.base",
         "https://download.visualstudio.microsoft.com/download/pr/"
         "67cf767c-5e71-47c2-a54a-cd5631e28942/"
         "ba1aeca6d6470d2b3b318ec7bffb3e61f9a736cfb76d8a7d18e004a8e7c26651/"
         "Microsoft.VC.14.44.17.14.CRT.ARM64.Desktop.base.vsix",
         "ba1aeca6d6470d2b3b318ec7bffb3e61f9a736cfb76d8a7d18e004a8e7c26651",
         "",
         "zip",
         "sdk",
         {"msvc/lib/libcmt.lib", "msvc/lib/libcpmt.lib",
          "msvc/lib/libvcruntime.lib"},
         {{"Contents/VC/Tools/MSVC/14.44.35207/lib/arm64/", "msvc/lib/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"Microsoft.VC.14.44.17.14.CRT.ARM64.Store.base",
         "https://download.visualstudio.microsoft.com/download/pr/"
         "67cf767c-5e71-47c2-a54a-cd5631e28942/"
         "57ece91747be72fdd9ed0c39b83225011c8afba6b6f616a23ace54d0522f79e9/"
         "Microsoft.VC.14.44.17.14.CRT.ARM64.Store.base.vsix",
         "57ece91747be72fdd9ed0c39b83225011c8afba6b6f616a23ace54d0522f79e9",
         "",
         "zip",
         "sdk",
         {"msvc/lib/oldnames.lib"},
         {{"Contents/VC/Tools/MSVC/14.44.35207/lib/arm64/", "msvc/lib/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"microsoft.windows.sdk.cpp",
         "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/"
         "10.0.26100.9169/microsoft.windows.sdk.cpp.10.0.26100.9169.nupkg",
         "475269434dcd808a67853773272f972c3229c0e10c3ddc821290e70cc0f6904d",
         "",
         "zip",
         "sdk",
         {"windows/include/um/Windows.h", "windows/include/ucrt/stdio.h"},
         {{"c/Include/10.0.26100.0/", "windows/include/"}},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"microsoft.windows.sdk.cpp.arm64",
         "https://api.nuget.org/v3-flatcontainer/"
         "microsoft.windows.sdk.cpp.arm64/10.0.26100.9169/"
         "microsoft.windows.sdk.cpp.arm64.10.0.26100.9169.nupkg",
         "b5eb594aaf0e98c381d5852e45b42f375920ca6268870c151ec3405b63676542",
         "",
         "zip",
         "sdk",
         {"windows/lib/um/kernel32.Lib", "windows/lib/ucrt/libucrt.lib"},
         {{"c/um/arm64/", "windows/lib/um/"},
          {"c/ucrt/arm64/", "windows/lib/ucrt/"}},
         536870912ULL,
         8589934592ULL,
         false});
    break;
  case Target::LinuxX64:
    Bundle.Id = "LLVM 23.1.3 + native SDK / linux-x86_64";
    Bundle.Packages.push_back(
        {"LLVM-23.1.3-Linux-X64",
         "https://github.com/llvm/llvm-project/releases/download/"
         "llvmorg-23.1.3/LLVM-23.1.3-Linux-X64.tar.xz",
         "ca9fe385a5d9e2d88132dc0e23a00e9f87f38fa75f67207b5e457d208e71d013",
         "LLVM-23.1.3-Linux-X64",
         "",
         "",
         {"bin/clang", "bin/clang++", "bin/clang-scan-deps", "bin/clang-format",
          "bin/clang-tidy", "bin/lld", "bin/llvm-ar", "bin/llvm-readobj",
          "lib/clang/23/include/stddef.h"},
         {},
         4294967296ULL,
         42949672960ULL,
         true});
    Bundle.Packages.push_back(
        {"libc6 2.35-0ubuntu3.15",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/"
         "libc6_2.35-0ubuntu3.15_amd64.deb",
         "79e35256227e16a607c154cdeb8d76ff12d20e31de286ea7fd9ad3b96fe0452d",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"gcc-11-base 11.4.0-1ubuntu1~22.04.3",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-11/"
         "gcc-11-base_11.4.0-1ubuntu1~22.04.3_amd64.deb",
         "a0445a3889fd4441cc3b3a125d6d1589de3d2628ca4e92a352170ed0fea4d18b",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"gcc-12-base 12.3.0-1ubuntu1~22.04.3",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/"
         "gcc-12-base_12.3.0-1ubuntu1~22.04.3_amd64.deb",
         "7f9253b7e0976f0526fc21346c73ee006e185a7b2f2f865048ea78a2af55bc8d",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libgcc-s1 12.3.0-1ubuntu1~22.04.3",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/"
         "libgcc-s1_12.3.0-1ubuntu1~22.04.3_amd64.deb",
         "d383e642d83263147f7b4b69fb92cf1036e5246c6be3accf10b77f986684589a",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libstdc++6 12.3.0-1ubuntu1~22.04.3",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/"
         "libstdc++6_12.3.0-1ubuntu1~22.04.3_amd64.deb",
         "29ef3d289b272704b75141906c8f683db0f2bdb0942061366691333bfb289bdc",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"zlib1g 1:1.2.11.dfsg-2ubuntu9.2",
         "https://archive.ubuntu.com/ubuntu/pool/main/z/zlib/"
         "zlib1g_1.2.11.dfsg-2ubuntu9.2_amd64.deb",
         "9dc17e51a1be2d9ed63b7b84ef0e4e29c5abe6f1bc62cb03e7181483cce8a2f2",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libtinfo6 6.3-2ubuntu0.3",
         "https://archive.ubuntu.com/ubuntu/pool/main/n/ncurses/"
         "libtinfo6_6.3-2ubuntu0.3_amd64.deb",
         "3b8dff43a31168f5020928f2ab9a97324c5b47ae708311781f6bd01306c96214",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libcrypt1 1:4.4.27-1",
         "https://archive.ubuntu.com/ubuntu/pool/main/libx/libxcrypt/"
         "libcrypt1_4.4.27-1_amd64.deb",
         "3fa566e9f861a08736cbc5a97562d9d6e4f0c00450fbeafcb6d7583423b04a98",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libc6-dev 2.35-0ubuntu3.15",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/"
         "libc6-dev_2.35-0ubuntu3.15_amd64.deb",
         "ce51ae233a9f800a040de2d0cd5983ef86f558c623b342c80942301d7f453f48",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"linux-libc-dev 5.15.0-198.208",
         "https://archive.ubuntu.com/ubuntu/pool/main/l/linux/"
         "linux-libc-dev_5.15.0-198.208_amd64.deb",
         "d7ba314f1e0e79745f104eaeeb662723fb72b1d4ab45c923d93db32867dd11e7",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libgcc-11-dev 11.4.0-1ubuntu1~22.04.3",
         "https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-11/"
         "libgcc-11-dev_11.4.0-1ubuntu1~22.04.3_amd64.deb",
         "35ed68622daf796ac010f63c73abd8aff851fc6d862038461b053b55dbec93a6",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libcrypt-dev 1:4.4.27-1",
         "https://archive.ubuntu.com/ubuntu/pool/main/libx/libxcrypt/"
         "libcrypt-dev_4.4.27-1_amd64.deb",
         "a79be2f6e45823dcc09e04d5e98c88ec88d07d5b8895d05b875a8ade8b345efa",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    break;
  case Target::LinuxArm64:
    Bundle.Id = "LLVM 23.1.3 + native SDK / linux-arm64";
    Bundle.Packages.push_back(
        {"LLVM-23.1.3-Linux-ARM64",
         "https://github.com/llvm/llvm-project/releases/download/"
         "llvmorg-23.1.3/LLVM-23.1.3-Linux-ARM64.tar.xz",
         "56d005e339c979a696ed6c244e6472e16f3b1371e48987a07e6145bda7ee0d03",
         "LLVM-23.1.3-Linux-ARM64",
         "",
         "",
         {"bin/clang", "bin/clang++", "bin/clang-scan-deps", "bin/clang-format",
          "bin/clang-tidy", "bin/lld", "bin/llvm-ar", "bin/llvm-readobj",
          "lib/clang/23/include/stddef.h"},
         {},
         4294967296ULL,
         42949672960ULL,
         true});
    Bundle.Packages.push_back(
        {"libc6 2.35-0ubuntu3.15",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/glibc/"
         "libc6_2.35-0ubuntu3.15_arm64.deb",
         "dd88cc85dcfbac19c0c9b606676b7b1b87f851d75afa5c830fc5ba1293a30833",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"gcc-11-base 11.4.0-1ubuntu1~22.04.3",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-11/"
         "gcc-11-base_11.4.0-1ubuntu1~22.04.3_arm64.deb",
         "69d03f54155fedab6c941e63149cf6ddadaa3638a005bbc66ef0d8445c3e5af3",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"gcc-12-base 12.3.0-1ubuntu1~22.04.3",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-12/"
         "gcc-12-base_12.3.0-1ubuntu1~22.04.3_arm64.deb",
         "b2bc566681989941dd059d6391b01c41854857f26d83c53358287d620dfb0fd4",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libgcc-s1 12.3.0-1ubuntu1~22.04.3",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-12/"
         "libgcc-s1_12.3.0-1ubuntu1~22.04.3_arm64.deb",
         "46626e1af6b159061e055f843654ee1f0f1e65fa280331ed47ee76ba1b1f6045",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libstdc++6 12.3.0-1ubuntu1~22.04.3",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-12/"
         "libstdc++6_12.3.0-1ubuntu1~22.04.3_arm64.deb",
         "59d244119e6129c7e9c0897491c0e210e925d15881b2dedcf8965ceb4fbf1aa3",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"zlib1g 1:1.2.11.dfsg-2ubuntu9.2",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/z/zlib/"
         "zlib1g_1.2.11.dfsg-2ubuntu9.2_arm64.deb",
         "cfd99d1ca41e710fdd3f68bfdc4d36dd24e14377656764f00b215b89cf6976e8",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libtinfo6 6.3-2ubuntu0.3",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/n/ncurses/"
         "libtinfo6_6.3-2ubuntu0.3_arm64.deb",
         "971b5fd1cf8f7d68ba00f40d8eec6474fae6a6adbe19883aa42dd2b74c672e4a",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libcrypt1 1:4.4.27-1",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/libx/libxcrypt/"
         "libcrypt1_4.4.27-1_arm64.deb",
         "fb5f8d59de95bbb118a635e83e2a92077e13c6d297980dae3119df057bf88111",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libc6-dev 2.35-0ubuntu3.15",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/glibc/"
         "libc6-dev_2.35-0ubuntu3.15_arm64.deb",
         "e1da77e87451ca8f41b2757fe274bca357fd5f7498afe47c05360681d0727bae",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"linux-libc-dev 5.15.0-198.208",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/l/linux/"
         "linux-libc-dev_5.15.0-198.208_arm64.deb",
         "50c0842a1c9ca658fca7e327a0b1d69ef0a630b83b18a170204a6c226764b2f3",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libgcc-11-dev 11.4.0-1ubuntu1~22.04.3",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-11/"
         "libgcc-11-dev_11.4.0-1ubuntu1~22.04.3_arm64.deb",
         "8f65b96e09a0c0f9dace290d8e91ba9209ac991ef1c57803ff6ae8a0e8402527",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    Bundle.Packages.push_back(
        {"libcrypt-dev 1:4.4.27-1",
         "https://ports.ubuntu.com/ubuntu-ports/pool/main/libx/libxcrypt/"
         "libcrypt-dev_4.4.27-1_arm64.deb",
         "17805b64d386655edca4aafbf01e6b89bd3b9a35cde7a31da964dd766ad9c5b6",
         "",
         "deb",
         "sdk",
         {},
         {},
         536870912ULL,
         8589934592ULL,
         false});
    break;
  case Target::MacosArm64:
    Bundle.Id = "LLVM 23.1.3 + native SDK / macos-arm64";
    Bundle.Packages.push_back(
        {"LLVM-23.1.3-macOS-ARM64",
         "https://github.com/llvm/llvm-project/releases/download/"
         "llvmorg-23.1.3/LLVM-23.1.3-macOS-ARM64.tar.xz",
         "3dcfd88aec10be204a24037bcb0dc0ad777882e9dbe8ac60e2639060d8a794b1",
         "LLVM-23.1.3-macOS-ARM64",
         "",
         "",
         {"bin/clang", "bin/clang++", "bin/clang-scan-deps", "bin/clang-format",
          "bin/clang-tidy", "bin/lld", "bin/llvm-ar", "bin/llvm-readobj",
          "lib/clang/23/include/stddef.h"},
         {},
         4294967296ULL,
         42949672960ULL,
         true});
    Bundle.Packages.push_back(
        {"Apple macOS SDK 27.0 / CLT 27.0 (082-83364)",
         "https://swcdn.apple.com/content/downloads/58/48/"
         "082-83364-A_KCEBOO2NJS/0d2rj4y5ucjlkgcvqt6f6a4pergug5tu2b/"
         "CLTools_macOSNMOS_SDK.pkg",
         "d55351824fd17742fd6e1e7a252fa1028698f32147ee48be9a6b75442bf30575",
         "",
         "apple-pkg",
         "sdk",
         {"usr/include/stdio.h", "usr/lib/libSystem.tbd", "SDKSettings.json"},
         {{"Library/Developer/CommandLineTools/SDKs/MacOSX27.0.sdk/", ""}},
         536870912ULL,
         8589934592ULL,
         false});
    break;
  default:
    throw Failure("Target uses a separate SDK bundle: " +
                  std::string(targetName(BuildTarget)));
  }
  return Bundle;
}
ToolchainBundle hostBundle(Target Host) {
  auto Native = pinnedBundle(Host);
  ToolchainBundle Result{"LLVM 23.1.3 host / " + std::string(targetName(Host)),
                         {Native.Packages.front()}};
  for (auto Name : {"ld.lld", "lld-link", "ld64.lld"}) {
    std::string File = "bin/" + std::string(Name);
    if (Host == Target::WindowsX64 || Host == Target::WindowsArm64)
      File += ".exe";
    Result.Packages.front().RequiredFiles.push_back(pathFromText(File));
  }
  const char *Asset = nullptr, *Hash = nullptr;
  switch (Host) {
  case Target::WindowsX64:
    Asset = "ninja-win.zip";
    Hash = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65";
    break;
  case Target::WindowsArm64:
    Asset = "ninja-winarm64.zip";
    Hash = "e52f0bdef9dfb1003229dbd6508a508c4073fd017247002adc66e5e806cb0391";
    break;
  case Target::LinuxX64:
    Asset = "ninja-linux.zip";
    Hash = "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6";
    break;
  case Target::LinuxArm64:
    Asset = "ninja-linux-aarch64.zip";
    Hash = "fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc";
    break;
  case Target::MacosArm64:
    Asset = "ninja-mac.zip";
    Hash = "c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b";
    break;
  default:
    throw Failure("Invalid host target: " + std::string(targetName(Host)));
  }
  auto Name = (Host == Target::WindowsX64 || Host == Target::WindowsArm64)
                  ? "ninja.exe"
                  : "ninja";
  Result.Packages.push_back(
      {"Ninja 1.13.2 / " + std::string(targetName(Host)),
       "https://github.com/ninja-build/ninja/releases/download/v1.13.2/" +
           std::string(Asset),
       Hash,
       "",
       "zip",
       "bin",
       {pathFromText(Name)},
       {},
       10485760,
       20971520,
       false});
  const char *DebuggerHash = nullptr;
  switch (Host) {
  case Target::WindowsX64:
    DebuggerHash =
        "2f655f1d6a405de4891d907c7e73353af416671e035b4c505677bbad14acf9f8";
    break;
  case Target::WindowsArm64:
    DebuggerHash =
        "a3d59f80b523975909e4ae5aed602ce0b0961e958cd939394dcd58dcd7a74956";
    break;
  case Target::LinuxX64:
    DebuggerHash =
        "4558c263d3776c93da2e89ca69d35a99ecd7f786bc288d37e3165f35b4596d48";
    break;
  case Target::LinuxArm64:
    DebuggerHash =
        "36b7267d9327215938b862318ac983af43c83054721264070ec9aeb175b0e66e";
    break;
  case Target::MacosArm64:
    DebuggerHash =
        "0839c7e062ba8c2ff02f6b97a5b7fe664864eacf9fcc3935ac999d23fbf6fa42";
    break;
  default:
    throw Failure("Invalid host target: " + std::string(targetName(Host)));
  }
  const bool Windows =
      Host == Target::WindowsX64 || Host == Target::WindowsArm64;
  auto Suffix = Windows ? ".exe" : "";
  std::vector<Path> Required;
  for (const char *Tool : {"lldb", "lldb-server", "lldb-argdumper"})
    Required.emplace_back("bin/" + std::string(Tool) + Suffix);
  if (Windows) {
    for (const char *File : {
             "bin/liblldb.dll",
             "bin/python3.dll",
             "bin/python314.dll",
             "bin/python314.zip",
             "bin/python314._pth",
             "lib/site-packages/lldb/__init__.py",
             "lib/site-packages/lldb/native/_lldb.pyd",
             "licenses/python/LICENSE",
         })
      Required.emplace_back(File);
  } else if (Host == Target::LinuxX64 || Host == Target::LinuxArm64) {
    for (const char *File : {
             "lib/liblldb.so.23.1.3",
             "lib/libpython3.14.so.1.0",
             "lib/python3.14/encodings/__init__.py",
             "lib/python3.14/site-packages/lldb/__init__.py",
             "licenses/python/LICENSE",
         })
      Required.emplace_back(File);
  } else {
    for (const char *File : {
             "bin/debugserver",
             "lib/liblldb.23.1.3.dylib",
         })
      Required.emplace_back(File);
  }
  Required.emplace_back("licenses/llvm/LICENSE");
  Result.Packages.push_back(
      {"LLDB 23.1.3 / " + std::string(targetName(Host)),
       "https://github.com/llm-build-system/llm/releases/download/"
       "v1.0.0/lldb-23.1.3-" +
           std::string(targetName(Host)) + ".tar.gz",
       DebuggerHash,
       "",
       "tar.gz",
       "",
       std::move(Required),
       {},
       268435456,
       1073741824,
       false});
  return Result;
}
ToolchainBundle targetBundle(Target BuildTarget) {
  std::string SdkHash, Hash;
  switch (BuildTarget) {
  case Target::WindowsX64:
    Hash = "67102d5b21eadd52d400be37043d4a2d70ec0ee91c2ccb48a1a6674189e5ad17";
    break;
  case Target::WindowsArm64:
    Hash = "0a733cb7524ba7ba58a73f0d1802d391ea7494f672e22ecc9dd79c1319f668b4";
    break;
  case Target::LinuxX64:
    Hash = "28345b32d3afa185b02e072aedb4c6b08035834a8b34d11884ebc3b753c8bc65";
    break;
  case Target::LinuxArm64:
    Hash = "3d0d2c7d51d18fd07dfca6bb1794b346a1cd5043421f1dcc5a76a020a5ab4091";
    break;
  case Target::MacosArm64:
    Hash = "989659f459672a3419ead403e16342433aaa8f429240f8fe5ae1043ddc17a6fb";
    break;
  case Target::WindowsGnuX64:
    SdkHash =
        "1b3cf5e423791c125cf241ff465b8e812c8f119757099000dc2ff6ac0826bc46";
    Hash = "6b8ce5fce4bd3bacac653e33291a07770ef45d99e963445ca325c20f16e6d759";
    break;
  case Target::WindowsGnuArm64:
    SdkHash =
        "2ac49e457de6d3fb0cf47c33aac9d42ba0ac2138202a03d92874c4ffdeb5dc14";
    Hash = "31a3a5db953a85229d082aaa49f24140b3dde66500fd40f42a62e94f6c55aa24";
    break;
  case Target::LinuxMuslX64:
    SdkHash =
        "dc98cc711171c1faa2af67ae3fdba6979e4be32c1eba25a922c90d09abcaa20e";
    Hash = "82045f49f69c94e26eca4034191e6b7c79c4fa8991f8b49a40b082f38163e366";
    break;
  case Target::LinuxMuslArm64:
    SdkHash =
        "baf1fa969d402c3751f443f92ad2ba788015764027611d8892f99cf99f301477";
    Hash = "8020c1484072671ddedba9fe4551eb4d551f98c22aad6bc428b40ac1680b2b13";
    break;
  }
  ToolchainBundle Result{
      "LLVM 23.1.3 target SDK / " + std::string(targetName(BuildTarget)), {}};
  if (!SdkHash.empty()) {
    bool Windows = isWindowsTarget(BuildTarget);
    std::vector<Path> Required = {"lib/clang/23/include/stddef.h"};
    if (Windows) {
      for (auto File :
           {"sdk/include/stdio.h", "sdk/lib/crt2.o", "sdk/lib/libmingw32.a",
            "sdk/lib/libucrt.a", "licenses/mingw-w64/COPYING"})
        Required.emplace_back(File);
      Required.push_back(pathFromText(
          std::string("lib/clang/23/lib/windows/libclang_rt.builtins-") +
          (isArm64Target(BuildTarget) ? "aarch64.a" : "x86_64.a")));
    } else {
      for (auto File :
           {"sdk/usr/include/stdio.h", "sdk/usr/include/linux/futex.h",
            "sdk/usr/lib/crt1.o", "sdk/usr/lib/libc.a", "sdk/usr/lib/libc.so",
            "licenses/musl/LICENSE"})
        Required.emplace_back(File);
      Required.push_back(pathFromText("lib/clang/23/lib/" +
                                      std::string(targetName(BuildTarget)) +
                                      "/libclang_rt.builtins.a"));
    }
    Result.Packages.push_back(
        {Windows ? "MinGW-w64 UCRT / llvm-mingw 20261006"
                 : "musl 1.2.6 + Linux UAPI / LLVM compiler-rt 23.1.3",
         "https://github.com/llm-build-system/llm/releases/download/v1.0.0/"
         "sdk-" +
             std::string(targetName(BuildTarget)) + ".tar.gz",
         SdkHash,
         "",
         "tar.gz",
         "",
         std::move(Required),
         {},
         536870912ULL,
         2147483648ULL,
         false});
  } else {
    auto Native = pinnedBundle(BuildTarget);
    if (BuildTarget != Target::WindowsX64 &&
        BuildTarget != Target::WindowsArm64) {
      auto Runtime = Native.Packages.front();
      Runtime.Id += " target runtime";
      Runtime.ToolchainOnly = false;
      Runtime.RequiredFiles = {"lib/clang/23/include/stddef.h"};
      Runtime.Mappings = {{"lib/clang/23/", "lib/clang/23/"}};
      if (BuildTarget != Target::MacosArm64)
        Runtime.RequiredFiles.push_back(pathFromText(
            "lib/clang/23/lib/" + std::string(targetName(BuildTarget)) +
            "/libclang_rt.builtins.a"));
      Result.Packages.push_back(std::move(Runtime));
    }
    Result.Packages.insert(Result.Packages.end(), Native.Packages.begin() + 1,
                           Native.Packages.end());
    if (BuildTarget == Target::MacosArm64) {
      auto &Sdk = Result.Packages.back();
      auto Prefix = Sdk.Mappings.front().first;
      Sdk.Mappings.clear();
      for (auto Directory : {"System/", "usr/include/", "usr/lib/"})
        Sdk.Mappings.emplace_back(Prefix + Directory, Directory);
      for (auto Name : {"SDKSettings.json", "SDKSettings.plist"})
        Sdk.Mappings.emplace_back(Prefix + Name, Name);
    }
  }
  bool Windows =
      BuildTarget == Target::WindowsX64 || BuildTarget == Target::WindowsArm64;
  auto Library = std::string("lib/");
  std::vector<Path> Required = {
      "include/c++/v1/vector", "include/c++/v1/__config_site",
      "share/libc++/v1/std.cppm", "share/libc++/v1/std.compat.cppm",
      "libcxx/LICENSE.TXT"};
  if (Windows) {
    Required.push_back(pathFromText(Library + "libc++.lib"));
    Required.push_back(pathFromText(Library + "libc++experimental.lib"));
  } else {
    for (auto Name :
         {"libc++.a", "libc++abi.a", "libunwind.a", "libc++experimental.a"})
      Required.push_back(pathFromText(Library + Name));
    Required.push_back("libcxxabi/LICENSE.TXT");
    Required.push_back("libunwind/LICENSE.TXT");
  }
  Result.Id += " + libc++ 759327d9a39e";
  Result.Packages.push_back(
      {"libc++ 24.0.0git / 759327d9a39e / " +
           std::string(targetName(BuildTarget)),
       "https://github.com/llm-build-system/llm/releases/download/"
       "v1.0.0/libc++-" +
           std::string(targetName(BuildTarget)) + ".tar.gz",
       Hash,
       "",
       "",
       "",
       std::move(Required),
       {},
       33554432ULL,
       268435456ULL,
       false});
  return Result;
}
Path targetToolchain(const Path &Tools, Target BuildTarget) {
  auto TargetRoot = Tools / "targets" / pathFromText(targetName(BuildTarget));
  rejectSymlinkAncestors(TargetRoot);
  auto Status = fs::symlink_status(nativePath(TargetRoot));
  if (!fs::exists(Status))
    throw Failure("Target SDK is not installed. Run llm --install --target " +
                  std::string(targetName(BuildTarget)) + ".");
  if (!fs::is_directory(Status))
    throw Failure("Invalid target SDK directory: " + pathText(TargetRoot));
  for (const auto *Name : {"clang.cfg", "clang++.cfg"}) {
    auto File = TargetRoot / Name;
    rejectSymlinkAncestors(File);
    if (!fs::is_regular_file(nativePath(File)) ||
        !fs::file_size(nativePath(File)))
      throw Failure("Target SDK compiler configuration is missing or empty: " +
                    pathText(File));
  }
  return TargetRoot;
}
Path compilerConfig(const Path &Tools, Target BuildTarget, bool Cxx) {
  auto TargetRoot = targetToolchain(Tools, BuildTarget);
  return TargetRoot / (Cxx ? "clang++.cfg" : "clang.cfg");
}
std::string toolchainIdentity(const Path &Tools, Target BuildTarget) {
  return bundleFingerprint(hostBundle(nativeTarget())) + '\n' +
         bundleFingerprint(targetBundle(BuildTarget));
}
std::string bundleFingerprint(const ToolchainBundle &Bundle) {
  std::ostringstream Value;
  Value << Bundle.Id;
  for (const auto &Package : Bundle.Packages) {
    Value << '|' << Package.Id << ':' << Package.Url << ':' << Package.Sha256
          << ':' << pathText(Package.Destination) << ':' << Package.ArchiveRoot
          << ':' << (Package.Format == "deb") << ':' << Package.ToolchainOnly
          << ':' << Package.MaxDownload << ':' << Package.MaxExpanded << ':'
          << Package.Format;
    for (const auto &[Source, Destination] : Package.Mappings)
      Value << ':' << Source << '>' << Destination;
    for (const auto &File : Package.RequiredFiles)
      Value << ':' << pathText(File);
  }
  return Value.str();
}
Path debuggerPath(const Path &Toolchain) {
  auto Program = Toolchain / "bin/lldb";
#ifdef _WIN32
  Program += ".exe";
#endif
  rejectSymlinkAncestors(Program);
  if (!fs::is_regular_file(Program))
    throw Failure("LLDB is missing. Run llm --install.");
  return Program;
}
std::map<std::string, std::string> debuggerEnvironment(const Path &Toolchain) {
#ifdef __linux__
  return {{"PYTHONHOME", pathText(Toolchain)},
          {"PYTHONDONTWRITEBYTECODE", "1"},
          {"LD_LIBRARY_PATH", pathText(Toolchain / "lib") + ':' +
                                  pathText(Toolchain / "host-runtime")}};
#else
  (void)Toolchain;
  return {};
#endif
}
Path installedToolchain(const Context &Call) {
  auto Prefix = installationLayout(Call).ProgramRoot / "versions" /
                pathFromText(Call.Version);
  rejectSymlinkAncestors(Prefix);
  auto Status = fs::symlink_status(nativePath(Prefix));
  if (!fs::exists(Status))
    throw Failure("This llm release is not installed. Run llm --install.");
  if (!fs::is_directory(Status))
    throw Failure("Invalid llm installation directory: " + pathText(Prefix));
  auto Identity = installationFile(Prefix);
  if (!fs::is_regular_file(nativePath(Identity)))
    throw Failure("Existing llm installation has no regular inventory: " +
                  pathText(Identity));
  std::ifstream Receipt(nativePath(Identity));
  std::string Version, Catalog, ExecutableHash;
  auto Host = hostBundle(nativeTarget());
  if (!readInstallIdentity(Receipt, Version, Catalog, ExecutableHash) ||
      Version != Call.Version)
    throw Failure(
        "Existing llm installation has an invalid release identity: " +
        pathText(Identity));
  if (Catalog != bundleFingerprint(Host))
    throw Failure(
        "Installed host toolchain catalog does not match this release: " +
        pathText(Identity));
  for (const auto &Package : Host.Packages)
    for (const auto &File : Package.RequiredFiles)
      if (!fs::is_regular_file(
              nativePath(Prefix / "toolchain" / Package.Destination / File)))
        throw Failure(
            "Installed host toolchain component is missing: " +
            pathText(Prefix / "toolchain" / Package.Destination / File));
  return Prefix / "toolchain";
}
#ifndef _WIN32
namespace {
void prepareWindowsPaths(const Path &TargetRoot) {
  auto Directory = [&](auto &&Self, const Path &Relative) -> std::string {
    std::vector<Path> Entries;
    for (const auto &Entry :
         fs::directory_iterator(nativePath(TargetRoot / Relative)))
      Entries.push_back(Entry.path());
    std::ranges::sort(Entries);
    std::string Contents;
    for (const auto &Entry : Entries) {
      if (!Contents.empty())
        Contents += ',';
      auto Name = jsonString(pathText(Entry.filename()));
      auto Child = Entry.lexically_relative(TargetRoot);
      if (fs::is_directory(nativePath(Entry)))
        Contents += "{\"type\":\"directory\",\"name\":" + Name +
                    ",\"contents\":" + Self(Self, Child) + '}';
      else if (fs::is_regular_file(nativePath(Entry)))
        Contents += "{\"type\":\"file\",\"name\":" + Name +
                    ",\"external-contents\":" + jsonString(pathText(Child)) +
                    '}';
      else
        throw Failure("Unexpected Windows SDK header entry");
    }
    return '[' + Contents + ']';
  };
  std::string Roots;
  for (auto Name : {"sdk/msvc/include", "sdk/windows/include/ucrt",
                    "sdk/windows/include/shared", "sdk/windows/include/um",
                    "sdk/windows/include/winrt"}) {
    if (!fs::is_directory(nativePath(TargetRoot / Name)))
      throw Failure("Windows SDK include directory is missing");
    if (!Roots.empty())
      Roots += ',';
    Roots += "{\"type\":\"directory\",\"name\":" + jsonString(Name) +
             ",\"contents\":" + Directory(Directory, Path(Name)) + '}';
  }
  writeText(TargetRoot / "paths.yaml",
            "{\"version\":0,\"case-sensitive\":false,"
            "\"use-external-names\":true,\"overlay-relative\":true,"
            "\"root-relative\":\"overlay-dir\",\"roots\":[" +
                Roots + "]}\n");
  std::vector<Path> Libraries;
  for (auto Name : {"sdk/msvc/lib", "sdk/windows/lib"})
    for (const auto &Entry :
         fs::recursive_directory_iterator(nativePath(TargetRoot / Name)))
      if (Entry.is_regular_file())
        Libraries.push_back(Entry.path());
  for (const auto &File : Libraries) {
    auto Name = pathText(File.filename());
    std::ranges::transform(Name, Name.begin(), [](unsigned char Byte) {
      return static_cast<char>(std::tolower(Byte));
    });
    auto Alias = File.parent_path() / pathFromText(Name);
    if (!fs::exists(nativePath(Alias)))
      fs::create_hard_link(nativePath(File), nativePath(Alias));
  }
}
}
#endif
void prepareTargetToolchain(Context &Call, const Path &Tools,
                            const Path &TargetRoot, Target BuildTarget) {
  cancelled(Call);
  auto Program = [](std::string Name) {
#ifdef _WIN32
    Name += ".exe";
#endif
    return "\"<CFGDIR>/../../bin/" + Name + "\"";
  };
  bool Windows =
      BuildTarget == Target::WindowsX64 || BuildTarget == Target::WindowsArm64;
#ifndef _WIN32
  if (Windows)
    prepareWindowsPaths(TargetRoot);
#endif
  const auto Resource =
      Windows ? "<CFGDIR>/../../lib/clang/23" : "<CFGDIR>/lib/clang/23";
  bool GnuWindows = isWindowsTarget(BuildTarget) && !Windows;
  std::string Common = "--target=" + std::string(targetName(BuildTarget)) +
                       "\n-resource-dir=\"" + Resource + "\"\n";
  Common += Windows
                ? "-fuse-ld=lld\n-B\"<CFGDIR>/../../bin/\"\n"
                  "-fms-compatibility-version=19.44\n"
                : "--ld-path=" +
                      Program(BuildTarget == Target::MacosArm64 ? "ld64.lld"
                                                                : "ld.lld") +
                      "\n";
  for (const auto *Relative : {"sdk/paths.yaml", "paths.yaml"}) {
    auto Overlay = TargetRoot / Relative;
    if (!fs::exists(fs::symlink_status(Overlay)))
      continue;
    rejectSymlinkAncestors(Overlay);
    if (!fs::is_regular_file(Overlay))
      throw Failure("Invalid generated SDK overlay: " + pathText(Overlay));
    Common += "-ivfsoverlay \"<CFGDIR>/" + std::string(Relative) + "\"\n";
  }
  std::string Cxx;
  if (Windows) {
    auto System = "-isystem \"<CFGDIR>/../../lib/clang/23/include\"\n-isystem "
                  "\"<CFGDIR>/sdk/msvc/include\"\n-isystem "
                  "\"<CFGDIR>/sdk/windows/include/ucrt\"\n-isystem "
                  "\"<CFGDIR>/sdk/windows/include/shared\"\n-isystem "
                  "\"<CFGDIR>/sdk/windows/include/um\"\n-isystem "
                  "\"<CFGDIR>/sdk/windows/include/winrt\"\n";
    Common +=
        "-fms-runtime-lib=static\n-nostdlibinc\n-nostdinc++\n"
        "-D_STATIC_INLINE_UCRT_FUNCTIONS=0\n"
        "-D_CRT_STDIO_ISO_WIDE_SPECIFIERS=1\n-L\"<CFGDIR>/sdk/msvc/lib\"\n"
        "-L\"<CFGDIR>/sdk/windows/lib/ucrt\"\n-L\"<CFGDIR>/sdk/windows/lib/"
        "um\"\n";
    Cxx = Common + "-isystem \"<CFGDIR>/include/c++/v1\"\n" + System;
    Common += System;
  } else if (BuildTarget == Target::MacosArm64) {
    Common +=
        "-fuse-ld=lld\n-isysroot \"<CFGDIR>/sdk\"\n-mmacosx-version-min=14.0\n";
    Cxx = Common +
          "-stdlib=libc++\n-nostdinc++\n-isystem \"<CFGDIR>/include/c++/v1\"\n"
          "-nostdlib++\n-Wl,-load_hidden,\"<CFGDIR>/lib/libc++.a\"\n"
          "-Wl,-load_hidden,\"<CFGDIR>/lib/libc++abi.a\"\n"
          "-Wl,-load_hidden,\"<CFGDIR>/lib/libunwind.a\"\n";
  } else if (GnuWindows || isMuslTarget(BuildTarget)) {
    Common += "--sysroot=\"<CFGDIR>/sdk\"\n-rtlib=compiler-rt\n"
              "-unwindlib=libunwind\n-static-libgcc\n-L\"<CFGDIR>/lib\"\n";
    if (GnuWindows)
      Common += "-static\n-fuse-cxa-atexit\n";
    Cxx = Common + "-stdlib=libc++\n-nostdinc++\n-isystem "
                   "\"<CFGDIR>/include/c++/v1\"\n-nostdlib++\n";
  } else {
    std::string GccTriple = BuildTarget == Target::LinuxArm64
                                ? "aarch64-linux-gnu"
                                : "x86_64-linux-gnu";
    Common += "--sysroot=\"<CFGDIR>/sdk\"\n--gcc-install-dir=\"<CFGDIR>/sdk/"
              "usr/lib/gcc/" +
              GccTriple +
              "/11\"\n-rtlib=compiler-rt\n-unwindlib=libunwind\n-static-"
              "libgcc\n-L\"<CFGDIR>/lib\"\n";
    Cxx = Common +
          "-stdlib=libc++\n-nostdinc++\n-isystem "
          "\"<CFGDIR>/include/c++/v1\"\n-static-libstdc++\n-Wl,-Bstatic\n"
          "-lc++abi\n-lunwind\n-Wl,-"
          "Bdynamic\n";
  }
  writeText(TargetRoot / "clang.cfg", Common);
  writeText(TargetRoot / "clang++.cfg", Cxx);
#ifdef __linux__
  if (BuildTarget == nativeTarget()) {
    fs::create_directories(nativePath(Tools / "host-runtime"));
    auto Architecture = BuildTarget == Target::LinuxArm64 ? "aarch64-linux-gnu"
                                                          : "x86_64-linux-gnu";
    for (const auto *Name : {"libz.so.1", "libtinfo.so.6"}) {
      auto Source = TargetRoot / "sdk/lib" / Architecture / Name;
      if (!fs::is_regular_file(nativePath(Source)))
        throw Failure("Required host runtime is missing: " + pathText(Source));
      fs::copy_file(nativePath(Source),
                    nativePath(Tools / "host-runtime" / Name),
                    fs::copy_options::overwrite_existing);
    }
  }
#else
  (void)Tools;
#endif
}
void prepareToolchain(Context &Call, const fs::path &Toolchain) {
  prepareTargetToolchain(Call, Toolchain,
                         Toolchain / "targets" /
                             pathFromText(targetName(nativeTarget())),
                         nativeTarget());
}

void validateTargetToolchain(Context &Call, const Path &Tools,
                             const Path &TargetRoot, Target BuildTarget) {
  for (const auto &Package : targetBundle(BuildTarget).Packages)
    for (const auto &File : Package.RequiredFiles)
      if (!fs::is_regular_file(
              nativePath(TargetRoot / Package.Destination / File)) ||
          !fs::file_size(nativePath(TargetRoot / Package.Destination / File)))
        throw Failure("Target SDK is missing " + pathText(File));
  auto Probe = fs::canonical(fs::temp_directory_path()) /
               ("llm-target-proof-" + uniqueId());
  fs::create_directory(nativePath(Probe));
  struct Cleanup {
    Path Directory;
    ~Cleanup() {
      std::error_code Ignored;
      fs::remove_all(nativePath(Directory), Ignored);
    }
  } Guard{Probe};
  for (bool Cxx : {false, true}) {
    auto Source = Probe / (Cxx ? "main.cpp" : "main.c");
    auto Output = Probe / artifactName(Cxx ? "proof-cpp" : "proof-c",
                                       ProductType::Executable, BuildTarget);
    writeText(Source, Cxx ? "#include <vector>\nint main(){try{throw "
                            "std::vector<int>{42};}catch(const "
                            "std::vector<int>& v){return v.at(0)!=42;}}\n"
                          : "#include <stdio.h>\nint main(void){return "
                            "puts(\"SDK ready\")<0;}\n");
    std::string Diagnostics;
    Strings Args{"--no-default-config",
                 "--config=" +
                     pathText(TargetRoot / (Cxx ? "clang++.cfg" : "clang.cfg")),
                 Cxx ? "-std=c++23" : "-std=c23",
                 pathText(Source),
                 "-o",
                 pathText(Output)};
    auto Defaults = defaultLinkArguments(TargetRoot, BuildTarget, Cxx);
    Args.insert(Args.end(), Defaults.begin(), Defaults.end());
    auto Result =
        runProcess(Call, compilerPath(Tools, Cxx), Args,
                   {.Directory = Probe,
                    .ToolBin = Tools / "bin",
                    .Environment = toolEnvironment(TargetRoot, BuildTarget),
                    .CaptureOutput = &Diagnostics});
    if (!Result.success())
      throw Failure("Target SDK compile/link check failed:\n" + Diagnostics);
    if (executableTarget(BuildTarget) &&
        !runProcess(Call, Output, {},
                    {.Directory = Probe, .CaptureOutput = &Diagnostics})
             .success())
      throw Failure("Native SDK execution check failed:\n" + Diagnostics);
  }
}
void validateToolchain(Context &Call, const Path &Toolchain) {
  std::string DebuggerVersion;
  if (!runProcess(Call, debuggerPath(Toolchain), {"--version"},
                  {.Directory = Toolchain,
                   .ToolBin = Toolchain / "bin",
                   .Environment = debuggerEnvironment(Toolchain),
                   .CaptureOutput = &DebuggerVersion})
           .success() ||
      !DebuggerVersion.starts_with("lldb version 23.1.3"))
    throw Failure("Installed LLDB 23.1.3 cannot start");

  validateTargetToolchain(Call, Toolchain,
                          targetToolchain(Toolchain, nativeTarget()),
                          nativeTarget());
  auto Ninja = Toolchain / "bin/ninja";
#ifdef _WIN32
  Ninja += ".exe";
#endif
  std::string Version;
  if (!runProcess(Call, Ninja, {"--version"},
                  {.Directory = Toolchain, .CaptureOutput = &Version})
           .success() ||
      (Version != "1.13.2\n" && Version != "1.13.2\r\n"))
    throw Failure("Installed Ninja is not Stock-Ninja 1.13.2");
  auto Probe = fs::canonical(fs::temp_directory_path()) /
               ("llm-module-proof-" + uniqueId());
  fs::create_directory(Probe);
  struct Cleanup {
    Path Root;
    ~Cleanup() {
      std::error_code E;
      fs::remove_all(Root, E);
    }
  } Guard{Probe};
  writeText(Probe / ".llm", "type='executable'\n");
  writeText(Probe / "main.cpp",
            "import std.compat;\nint main(){std::vector<int> v{42};return "
            "v.at(0)!=42;}\n");
  auto Nested = Call;
  Nested.Toolchain = Toolchain;
  auto Project = readManifest(Probe);
  auto Graph = loadGraph(Nested, Project, nativeTarget());
  Command Request;
  Request.Action = Operation::Build;
  auto Built = buildProject(Nested, Request, Graph);
  if (!runProcess(Nested, Built.Executable, {}, {.Directory = Probe}).success())
    throw Failure("Native standard module execution failed");
}
}
