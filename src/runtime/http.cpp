module;

#include <curl/curl.h>

module llm.runtime;

import :internal;
import std;

namespace llm::runtime {
namespace {
void lowerAscii(std::string &Text) {
  for (auto &Byte : Text)
    if (Byte >= 'A' && Byte <= 'Z')
      Byte += 'a' - 'A';
}

bool redirectStatus(unsigned Status) {
  return Status == 301 || Status == 302 || Status == 303 || Status == 307 ||
         Status == 308;
}

struct Transfer {
  Context &Call;
  std::ofstream File;
  HttpResponse Response;
  std::uint64_t Total = 0;
  std::uint64_t Limit;
  std::size_t HeaderBytes = 0;
  std::exception_ptr Error;
};

std::size_t bodyCallback(char *Bytes, std::size_t Size, std::size_t Count,
                         void *Opaque) noexcept {
  auto &State = *static_cast<Transfer *>(Opaque);
  try {
    cancelled(State.Call);
    if (Size && Count > std::numeric_limits<std::size_t>::max() / Size)
      throw Failure("HTTPS response size overflow");
    auto Length = Size * Count;
    if (Length > State.Limit - State.Total)
      throw Failure("HTTPS response exceeds size limit");
    State.Total += Length;
    State.File.write(Bytes, static_cast<std::streamsize>(Length));
    if (!State.File)
      throw Failure("Cannot write HTTPS response");
    return Length;
  } catch (...) {
    State.Error = std::current_exception();
    return 0;
  }
}

std::size_t headerCallback(char *Bytes, std::size_t Size, std::size_t Count,
                           void *Opaque) noexcept {
  auto &State = *static_cast<Transfer *>(Opaque);
  try {
    cancelled(State.Call);
    if (Size && Count > std::numeric_limits<std::size_t>::max() / Size)
      throw Failure("HTTP response header size overflow");
    auto Length = Size * Count;
    if (Length > 65536 || State.HeaderBytes > 256 * 1024 - Length)
      throw Failure("HTTP response headers exceed size limit");
    State.HeaderBytes += Length;
    std::string_view Line(Bytes, Length);
    if (Line.starts_with("HTTP/")) {
      State.Response.Headers.clear();
      return Length;
    }
    auto Colon = Line.find(':');
    if (Colon == std::string_view::npos)
      return Length;
    std::string Name(Line.substr(0, Colon));
    lowerAscii(Name);
    auto Start = Line.find_first_not_of(" \t", Colon + 1);
    auto End = Line.find_last_not_of(" \t\r\n");
    auto Value = Start == std::string_view::npos || End < Start
                     ? std::string{}
                     : std::string(Line.substr(Start, End - Start + 1));
    if (Value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
      throw Failure("Invalid HTTP response header");
    if ((Name == "location" || Name == "etag") &&
        State.Response.Headers.contains(Name))
      throw Failure("Ambiguous HTTP response identity header");
    State.Response.Headers[std::move(Name)] = std::move(Value);
    return Length;
  } catch (...) {
    State.Error = std::current_exception();
    return 0;
  }
}

int progressCallback(void *Opaque, curl_off_t, curl_off_t, curl_off_t,
                     curl_off_t) noexcept {
  auto &State = *static_cast<Transfer *>(Opaque);
  try {
    cancelled(State.Call);
    return 0;
  } catch (...) {
    State.Error = std::current_exception();
    return 1;
  }
}

HttpResponse getOnce(Context &Call, const std::string &Url, const Path &Output,
                     const HttpRequest &Request) {
  static const auto Initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (Initialized != CURLE_OK)
    throw Failure("Cannot initialize HTTPS library");
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> Handle(curl_easy_init(),
                                                             curl_easy_cleanup);
  if (!Handle)
    throw Failure("Cannot initialize HTTPS request");
  Transfer State{
      Call, std::ofstream(Output, std::ios::binary), {}, 0, Request.Limit};
  if (!State.File)
    throw Failure("Cannot create HTTPS response");
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> Headers(
      nullptr, curl_slist_free_all);
  for (const auto &[Name, Value] : Request.Headers) {
    auto Next = curl_slist_append(Headers.get(), (Name + ": " + Value).c_str());
    if (!Next)
      throw Failure("Cannot allocate HTTP headers");
    Headers.release();
    Headers.reset(Next);
  }
  auto Set = [&](CURLoption Option, auto Value) {
    if (curl_easy_setopt(Handle.get(), Option, Value) != CURLE_OK)
      throw Failure("Cannot configure HTTPS request");
  };
  Set(CURLOPT_URL, Url.c_str());
  Set(CURLOPT_PROTOCOLS_STR, "http,https");
  Set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  Set(CURLOPT_FOLLOWLOCATION, 0L);
  Set(CURLOPT_FAILONERROR, 0L);
  Set(CURLOPT_CONNECTTIMEOUT, 30L);
  Set(CURLOPT_LOW_SPEED_LIMIT, 1L);
  Set(CURLOPT_LOW_SPEED_TIME, 30L);
  Set(CURLOPT_NOSIGNAL, 1L);
  Set(CURLOPT_SSL_VERIFYPEER, 1L);
  Set(CURLOPT_SSL_VERIFYHOST, 2L);
  Set(CURLOPT_NOPROGRESS, 0L);
  Set(CURLOPT_XFERINFOFUNCTION, progressCallback);
  Set(CURLOPT_XFERINFODATA, &State);
  Set(CURLOPT_WRITEFUNCTION, bodyCallback);
  Set(CURLOPT_WRITEDATA, &State);
  Set(CURLOPT_HEADERFUNCTION, headerCallback);
  Set(CURLOPT_HEADERDATA, &State);
  Set(CURLOPT_HTTPHEADER, Headers.get());
  std::string Agent = "llm/" + Call.Version;
  Set(CURLOPT_USERAGENT, Agent.c_str());
  if (auto Roots = std::getenv("SSL_CERT_FILE"); Roots && *Roots) {
    Set(CURLOPT_CAINFO, Roots);
  } else {
    Set(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
  }
  auto Status = curl_easy_perform(Handle.get());
  cancelled(Call);
  if (State.Error)
    std::rethrow_exception(State.Error);
  if (Status != CURLE_OK)
    throw Failure("HTTPS request failed: " +
                  std::string(curl_easy_strerror(Status)));
  long Code = 0;
  if (curl_easy_getinfo(Handle.get(), CURLINFO_RESPONSE_CODE, &Code) !=
          CURLE_OK ||
      Code < 100 || Code > 599)
    throw Failure("Invalid HTTPS response status");
  State.Response.Status = static_cast<unsigned>(Code);
  State.Response.Url = Url;
  State.File.close();
  if (!State.File)
    throw Failure("Cannot finish HTTPS response");
  return std::move(State.Response);
}
}

std::string redirectUrl(std::string_view Base, std::string_view Location) {
  auto Source = take(parseRegistryUrl(Base));
  if (Location.starts_with("https://") || Location.starts_with("http://")) {
    auto Target = take(parseRegistryUrl(Location));
    if (Source.Scheme == "https" && Target.Scheme != "https")
      throw Failure("HTTPS redirects cannot downgrade to HTTP");
    return std::string(Location);
  }
  if (Location.starts_with("//")) {
    auto Result = Source.Scheme + ":" + std::string(Location);
    take(parseRegistryUrl(Result));
    return Result;
  }
  auto Colon = Location.find(':');
  auto Separator = Location.find_first_of("/?");
  if (Location.empty() ||
      (Colon != std::string_view::npos &&
       (Separator == std::string_view::npos || Colon < Separator)) ||
      Location.find_first_of("\r\n\t #\\\0", 0, 8) != std::string_view::npos)
    throw Failure("Invalid HTTPS redirect");
  std::string Resource;
  if (Location.front() == '/')
    Resource = Location;
  else if (Location.front() == '?')
    Resource = Source.Resource.substr(0, Source.Resource.find('?')) +
               std::string(Location);
  else {
    auto BasePath = Source.Resource.substr(0, Source.Resource.find('?'));
    Resource =
        BasePath.substr(0, BasePath.rfind('/') + 1) + std::string(Location);
  }
  auto Query = Resource.find('?');
  auto Normalized = pathFromText(Resource.substr(0, Query)).lexically_normal();
  auto Result = Source.Origin + pathText(Normalized) +
                (Query == std::string::npos ? "" : Resource.substr(Query));
  take(parseRegistryUrl(Result));
  return Result;
}

HttpResponse httpsGet(Context &Call, std::string Url, const Path &Output,
                      const HttpRequest &Request) {
  take(parseHttpsUrl(Url));
  return httpGet(Call, std::move(Url), Output, Request);
}

HttpResponse httpGet(Context &Call, std::string Url, const Path &Output,
                     const HttpRequest &Request) {
  auto Source = take(parseRegistryUrl(Url));
  if (Source.Scheme == "file")
    throw Failure("HTTP requests require an HTTP or HTTPS URL");
  rejectSymlinkAncestors(Output);
  HttpRequest Active = Request;
  Active.Headers.clear();
  for (const auto &[Name, Value] : Request.Headers) {
    if (Name.empty() ||
        Name.find_first_of(":\r\n \t\0", 0, 6) != std::string::npos ||
        Value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
      throw Failure("Invalid HTTP header");
    auto Normalized = Name;
    lowerAscii(Normalized);
    if (!Active.Headers.emplace(std::move(Normalized), Value).second)
      throw Failure("Duplicate HTTP header");
  }
  bool Complete = false;
  struct Cleanup {
    const Path &File;
    const bool &Complete;
    ~Cleanup() {
      if (!Complete) {
        std::error_code Error;
        std::filesystem::remove(File, Error);
      }
    }
  } Guard{Output, Complete};
  for (unsigned Redirects = 0; Redirects <= 10; ++Redirects) {
    cancelled(Call);
    if (take(parseRegistryUrl(Url)).Scheme == "http") {
      Active.Headers.erase("authorization");
      Active.Headers.erase("cookie");
      Active.Headers.erase("proxy-authorization");
    }
    auto Response = getOnce(Call, Url, Output, Active);
    if (!redirectStatus(Response.Status)) {
      Complete = true;
      return Response;
    }
    if (Redirects == 10)
      throw Failure("Too many HTTPS redirects");
    auto Location = Response.Headers.find("location");
    if (Location == Response.Headers.end())
      throw Failure("HTTPS redirect has no location");
    auto Next = redirectUrl(Url, Location->second);
    if (take(parseRegistryUrl(Next)).Origin !=
        take(parseRegistryUrl(Url)).Origin) {
      Active.Headers.erase("authorization");
      Active.Headers.erase("cookie");
      Active.Headers.erase("proxy-authorization");
    }
    Url = std::move(Next);
  }
  std::unreachable();
}

}
