#ifdef _WIN32
#include <windows.h>
#endif

import std;
import llm.cli;

namespace {
constexpr std::string_view Version = "1.0.0";
}

#ifdef _WIN32
int wmain(int Count, wchar_t **Values) try {
  std::vector<std::string> Arguments;
  Arguments.reserve(Count);
  for (int Index = 0; Index < Count; ++Index) {
    auto Length = static_cast<int>(std::wcslen(Values[Index]));
    int Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Values[Index],
                                   Length, nullptr, 0, nullptr, nullptr);
    if (!Size && Length)
      throw std::runtime_error("Cannot decode command-line arguments");
    std::string Text(Size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Values[Index], Length,
                        Text.data(), Size, nullptr, nullptr);
    Arguments.push_back(std::move(Text));
  }
  return llm::runCli(std::move(Arguments), Version);
} catch (const std::exception &Error) {
  std::cerr << "llm: error: " << Error.what() << '\n';
  return 1;
}
#else
int main(int Count, char **Values) try {
  return llm::runCli({Values, Values + Count}, Version);
} catch (const std::exception &Error) {
  std::cerr << "llm: error: " << Error.what() << '\n';
  return 1;
}
#endif
