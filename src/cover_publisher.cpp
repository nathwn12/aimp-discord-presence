//  Copyright (c) 2024 Exle
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#include "cover_publisher.h"

#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "utils.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

using CoverPublisher::Result;

void EmitLog(const std::string& line);


constexpr DWORD kTotalTimeoutMs = 6000;
constexpr DWORD kRetryBackoffMs = 500;
constexpr DWORD kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxCacheBytes = 1024 * 1024;
constexpr long long kNegativeCacheTtlSeconds = 60;
constexpr size_t kBoundaryLength = 29;

constexpr DWORD kDriveConsentWindowMs = 120 * 1000;
constexpr DWORD kDriveConsentPollMs = 250;
constexpr DWORD kDriveReceiveTimeoutMs = 2000;
constexpr DWORD kDriveConsentCooldownMs = 10 * 60 * 1000;
constexpr long long kDriveAccessTokenSkewSeconds = 60;
constexpr size_t kMaxDriveResponseBytes = 64 * 1024;
constexpr size_t kMaxLoopbackRequestBytes = 8 * 1024;
constexpr char kDriveFolderName[] = "aimp-covers";
constexpr char kDriveAuthEndpoint[] = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr char kDriveScope[] = "https://www.googleapis.com/auth/drive.file";
constexpr char kDriveLh3UrlPrefix[] = "https://lh3.googleusercontent.com/d/";

constexpr UINT kMaxImageDimension = 512;
constexpr size_t kSkipReencodeMaxBytes = 256 * 1024;
constexpr float kJpegQuality = 0.85f;

constexpr wchar_t kAgentName[] = L"AIMP-Discord-Presence/2.2.1";

enum class UploadShape {
  kCatbox,
  kUguu,
  kLitterbox,
  kDrive,
};

struct UploadHost {
  std::wstring name;   // log marker, e.g. "litter.catbox.moe"
  std::wstring host;   // for WinHttpConnect
  std::wstring path;   // request target, query string included
  UploadShape shape;
};

std::string HostNameUtf8(const UploadHost& upload_host) {
  return Utils::ToString(upload_host.name);
}

const size_t kBaseUploadHostCount = 3;
const UploadHost kBaseUploadHosts[kBaseUploadHostCount] = {
    {L"catbox.moe", L"catbox.moe", L"/user/api.php", UploadShape::kCatbox},
    {L"uguu.se", L"uguu.se", L"/upload?output=text", UploadShape::kUguu},
    {L"litter.catbox.moe", L"litterbox.catbox.moe", L"/resources/internals/api.php", UploadShape::kLitterbox},
};

std::mutex g_host_mutex;

std::string g_drive_client_id;
std::string g_drive_client_secret;
std::string g_drive_state_path;

enum class CoverHostPolicy { kAuto, kDrive, kKeyless };

CoverHostPolicy g_cover_host = CoverHostPolicy::kAuto;

CoverHostPolicy ParseCoverHostPolicy(const std::string& value) {
  static const char kWhitespace[] = " \t\r\n";
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::strchr(kWhitespace, value[begin]) != nullptr) {
    ++begin;
  }
  while (end > begin && std::strchr(kWhitespace, value[end - 1]) != nullptr) {
    --end;
  }
  std::string lower;
  lower.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    const char c = value[i];
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  if (lower == "drive") {
    return CoverHostPolicy::kDrive;
  }
  if (lower == "keyless") {
    return CoverHostPolicy::kKeyless;
  }
  return CoverHostPolicy::kAuto;
}

struct HostChainPlan {
  bool drive = false;
};

HostChainPlan PlanHostChain(CoverHostPolicy policy, bool drive_available) {
  HostChainPlan plan;
  switch (policy) {
    case CoverHostPolicy::kDrive:
      plan.drive = drive_available;
      break;
    case CoverHostPolicy::kKeyless:
      break;
    case CoverHostPolicy::kAuto:
    default:
      plan.drive = drive_available;
      break;
  }
  return plan;
}

enum class PermanentHost { kDrive };

std::vector<PermanentHost> PlanCacheUpgrade(CoverHostPolicy policy, bool drive_available) {
  const HostChainPlan plan = PlanHostChain(policy, drive_available);
  std::vector<PermanentHost> hosts;
  if (plan.drive) {
    hosts.push_back(PermanentHost::kDrive);
  }
  return hosts;
}

std::string PolicyName(CoverHostPolicy policy) {
  switch (policy) {
    case CoverHostPolicy::kDrive:
      return "drive";
    case CoverHostPolicy::kKeyless:
      return "keyless";
    case CoverHostPolicy::kAuto:
    default:
      return "auto";
  }
}

std::string DriveUnavailableReason(bool client_id, bool client_secret, bool state_path) {
  if (!client_id && !client_secret) {
    return "no-credentials";
  }
  if (!client_id) {
    return "no-client-id";
  }
  if (!client_secret) {
    return "no-client-secret";
  }
  if (!state_path) {
    return "no-state-path";
  }
  return std::string();
}

struct HostConfigSummary {
  CoverHostPolicy policy = CoverHostPolicy::kAuto;
  bool drive_usable = false;
  std::string drive_reason;   // why Drive is unusable; empty when usable
};

std::string FormatHostConfigLine(const HostConfigSummary& summary) {
  const HostChainPlan plan = PlanHostChain(summary.policy, summary.drive_usable);
  std::string line = std::string("local-cover hosts policy=") + PolicyName(summary.policy);
  line += std::string(" drive=") + (plan.drive ? "on" : "off");
  if (!plan.drive) {
    const std::string drive_reason =
        summary.drive_usable ? std::string("policy") : summary.drive_reason;
    if (!drive_reason.empty()) {
      line += " drive-reason=" + drive_reason;
    }
  }
  line += " keyless=on";
  return line;
}

bool DriveConfiguredLocked() {
  return !g_drive_client_id.empty() && !g_drive_client_secret.empty() && !g_drive_state_path.empty();
}

std::vector<UploadHost> ActiveHosts() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  const HostChainPlan plan = PlanHostChain(g_cover_host, DriveConfiguredLocked());
  std::vector<UploadHost> hosts;
  if (plan.drive) {
    UploadHost drive;
    drive.name = L"drive.google.com";
    drive.host = L"www.googleapis.com";
    drive.path = L"/upload/drive/v3/files?uploadType=multipart&fields=id";
    drive.shape = UploadShape::kDrive;
    hosts.push_back(drive);
  }
  for (size_t i = 0; i < kBaseUploadHostCount; ++i) {
    hosts.push_back(kBaseUploadHosts[i]);
  }
  return hosts;
}

bool DriveUploadActive() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return PlanHostChain(g_cover_host, DriveConfiguredLocked()).drive;
}

constexpr char kLitterboxUrlPattern[] = R"(^https://litter\.catbox\.moe/[a-z0-9]{6}\.(png|jpg|jpeg)$)";
constexpr char kUguuUrlPattern[] = R"(^https://([A-Za-z0-9-]+\.)?uguu\.se/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
constexpr char kCatboxUrlPattern[] = R"(^https://files\.catbox\.moe/[A-Za-z0-9]+\.(png|jpg|jpeg|gif|webp|bmp)$)";
constexpr char kDriveLh3UrlPattern[] =
    R"(^https://lh3\.googleusercontent\.com/d/[A-Za-z0-9_-]{10,}$)";
std::string UploadUrlPatternFor(const UploadHost& upload_host) {
  switch (upload_host.shape) {
    case UploadShape::kCatbox:
      return std::string(kCatboxUrlPattern);
    case UploadShape::kUguu:
      return std::string(kUguuUrlPattern);
    case UploadShape::kDrive:
      return std::string(kDriveLh3UrlPattern);
    case UploadShape::kLitterbox:
    default:
      return std::string(kLitterboxUrlPattern);
  }
}
constexpr char kMimePattern[] = R"(^[A-Za-z0-9][A-Za-z0-9.+-]*/[A-Za-z0-9][A-Za-z0-9.+-]*$)";

Result Failure(const std::string& reason) {
  Result result;
  result.reason = reason;
  return result;
}

std::string TrimWhitespace(const std::string& text) {
  const char* kWhitespace = " \t\r\n";
  const size_t begin = text.find_first_not_of(kWhitespace);
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(kWhitespace);
  return text.substr(begin, end - begin + 1);
}

bool LooksLikeUploadUrl(const std::string& text) {
  static const std::regex litterbox_pattern(kLitterboxUrlPattern);
  static const std::regex uguu_pattern(kUguuUrlPattern);
  static const std::regex catbox_pattern(kCatboxUrlPattern);
  static const std::regex drive_pattern(kDriveLh3UrlPattern);
  return std::regex_match(text, litterbox_pattern) || std::regex_match(text, uguu_pattern) ||
         std::regex_match(text, catbox_pattern) || std::regex_match(text, drive_pattern);
}

bool LooksLikeHostUrl(const std::string& text, const UploadHost& upload_host) {
  const std::string url_pattern = UploadUrlPatternFor(upload_host);
  static const std::map<std::string, std::regex> patterns = {
      {kLitterboxUrlPattern, std::regex(kLitterboxUrlPattern)},
      {kUguuUrlPattern, std::regex(kUguuUrlPattern)},
      {kCatboxUrlPattern, std::regex(kCatboxUrlPattern)},
      {kDriveLh3UrlPattern, std::regex(kDriveLh3UrlPattern)},
  };
  const auto found = patterns.find(url_pattern);
  return found != patterns.end() && std::regex_match(text, found->second);
}

std::string HostNameForUrl(const std::string& url) {
  if (LooksLikeHostUrl(url, UploadHost{L"catbox.moe", L"", L"", UploadShape::kCatbox})) {
    return "catbox.moe";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"uguu.se", L"", L"", UploadShape::kUguu})) {
    return "uguu.se";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"litter.catbox.moe", L"", L"", UploadShape::kLitterbox})) {
    return "litter.catbox.moe";
  }
  if (LooksLikeHostUrl(url, UploadHost{L"drive.google.com", L"", L"", UploadShape::kDrive})) {
    return "drive.google.com";
  }
  return std::string();
}

bool IsEphemeralCachedUrl(const std::string& url) {
  return LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kCatbox}) ||
         LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kUguu}) ||
         LooksLikeHostUrl(url, UploadHost{L"", L"", L"", UploadShape::kLitterbox});
}

bool IsLegacyGitHubCachedUrl(const std::string& url) {
  static const char kPrefix[] = "https://raw.githubusercontent.com/";
  return url.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0;
}

enum class CachedUrlAction {
  kUse,  // durable (Drive) or unknown shape: answer the hit
  kUpgradeToPermanent,
  kRevalidate,  // ephemeral or legacy URL, no permanent host: health-check, re-publish if dead
};

CachedUrlAction PlanCachedUrl(const std::string& url, bool permanent_active) {
  if (IsEphemeralCachedUrl(url) || IsLegacyGitHubCachedUrl(url)) {
    return permanent_active ? CachedUrlAction::kUpgradeToPermanent : CachedUrlAction::kRevalidate;
  }
  return CachedUrlAction::kUse;
}

std::string SanitizeMime(const char* mime) {
  const std::string candidate = mime != nullptr ? std::string(mime) : std::string();
  static const std::regex pattern(kMimePattern);
  if (std::regex_match(candidate, pattern)) {
    return candidate;
  }
  return "application/octet-stream";
}

const char* FilenameForMime(const std::string& mime) {
  if (mime == "image/png") {
    return "cover.png";
  }
  if (mime == "image/jpeg") {
    return "cover.jpg";
  }
  if (mime == "image/gif") {
    return "cover.gif";
  }
  if (mime == "image/webp") {
    return "cover.webp";
  }
  if (mime == "image/bmp") {
    return "cover.bmp";
  }
  return "cover.bin";
}


std::string MultipartPrefix(const std::string& boundary, const std::string& mime,
                            UploadShape shape) {
  const std::string delimiter = "--" + boundary + "\r\n";
  std::string prefix;
  if (shape == UploadShape::kLitterbox) {
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
    prefix += "fileupload\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"time\"\r\n\r\n";
    prefix += "72h\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  } else if (shape == UploadShape::kCatbox) {
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n";
    prefix += "fileupload\r\n";
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"";
  } else {
    prefix += delimiter;
    prefix += "Content-Disposition: form-data; name=\"files[]\"; filename=\"";
  }
  prefix += FilenameForMime(mime);
  prefix += "\"\r\nContent-Type: ";
  prefix += mime;
  prefix += "\r\n\r\n";
  return prefix;
}

std::string MultipartSuffix(const std::string& boundary) {
  return "\r\n--" + boundary + "--\r\n";
}

std::string BuildMultipartBody(const std::string& boundary, const std::string& mime,
                               const std::vector<unsigned char>& image,
                               UploadShape shape = UploadShape::kLitterbox) {
  std::string body = MultipartPrefix(boundary, mime, shape);
  if (!image.empty()) {
    body.append(reinterpret_cast<const char*>(image.data()), image.size());
  }
  body += MultipartSuffix(boundary);
  return body;
}


std::string DriveMultipartContentType(const std::string& boundary) {
  return "multipart/related; boundary=" + boundary;
}

std::string BuildDriveMetadataPart(const std::string& name, const std::string& folder_id) {
  return "{\"name\":\"" + name + "\",\"parents\":[\"" + folder_id + "\"]}";
}

std::string BuildDriveMultipartBody(const std::string& boundary, const std::string& name,
                                    const std::string& folder_id, const std::string& mime,
                                    const std::vector<unsigned char>& image) {
  const std::string delimiter = "--" + boundary + "\r\n";
  std::string body;
  body += delimiter;
  body += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
  body += BuildDriveMetadataPart(name, folder_id);
  body += "\r\n";
  body += delimiter;
  body += "Content-Type: " + mime + "\r\n\r\n";
  if (!image.empty()) {
    body.append(reinterpret_cast<const char*>(image.data()), image.size());
  }
  body += "\r\n";
  body += "--" + boundary + "--\r\n";
  return body;
}

std::string MakeBoundary() {
  static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const uint64_t seed = static_cast<uint64_t>(std::random_device()()) ^ static_cast<uint64_t>(GetTickCount64()) ^
                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::mt19937_64 generator(seed);
  std::uniform_int_distribution<size_t> pick(0, sizeof(kAlphabet) - 2);
  std::string boundary = "----";
  while (boundary.size() < kBoundaryLength) {
    boundary.push_back(kAlphabet[pick(generator)]);
  }
  return boundary;
}


Result ClassifyUploadResponse(DWORD status, const std::string& body,
                              UploadShape shape = UploadShape::kLitterbox) {
  const std::string text = TrimWhitespace(body);
  if (status == 200) {
    if (text.empty()) {
      return Failure("empty-body");
    }
    if (LooksLikeHostUrl(text, UploadHost{L"", L"", L"", shape})) {
      Result result;
      result.ok = true;
      result.url = text;
      return result;
    }
    return Failure("http-200-unexpected-body");
  }
  if (status == 412) {
    return Failure(text.empty() ? std::string("http-412") : text);
  }
  return Failure("http-" + std::to_string(status));
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return std::string();
  }
  size_t cursor = position + key.size() + 2;
  const auto skip_space = [&]() {
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                    json[cursor] == '\r' || json[cursor] == '\n')) {
      ++cursor;
    }
  };
  skip_space();
  if (cursor >= json.size() || json[cursor] != ':') {
    return std::string();
  }
  ++cursor;
  skip_space();
  if (cursor >= json.size() || json[cursor] != '"') {
    return std::string();
  }
  ++cursor;
  std::string value;
  while (cursor < json.size() && json[cursor] != '"') {
    if (json[cursor] == '\\' && cursor + 1 < json.size()) {
      ++cursor;
    }
    value.push_back(json[cursor]);
    ++cursor;
  }
  return value;
}


bool LooksLikeDriveFileId(const std::string& id) {
  if (id.size() < 10 || id.size() > 200) {
    return false;
  }
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
          c == '_')) {
      return false;
    }
  }
  return true;
}

std::string DriveLh3Url(const std::string& file_id) {
  return std::string(kDriveLh3UrlPrefix) + file_id;
}

long long ExtractJsonNumberField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return 0;
  }
  size_t cursor = position + key.size() + 2;
  while (cursor < json.size() &&
         (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != ':') {
    return 0;
  }
  ++cursor;
  while (cursor < json.size() &&
         (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  long long value = 0;
  bool any = false;
  while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
    value = value * 10 + (json[cursor] - '0');
    any = true;
    ++cursor;
  }
  return any ? value : 0;
}

std::string FirstDriveFileId(const std::string& json) {
  const size_t files = json.find("\"files\"");
  const std::string scope = files == std::string::npos ? json : json.substr(files);
  const std::string id = ExtractJsonStringField(scope, "id");
  return LooksLikeDriveFileId(id) ? id : std::string();
}

Result ClassifyDriveUpload(DWORD status, const std::string& body) {
  if (status == 200 || status == 201) {
    const std::string id = ExtractJsonStringField(TrimWhitespace(body), "id");
    if (!LooksLikeDriveFileId(id)) {
      return Failure("drive-http-" + std::to_string(status) + "-unexpected-body");
    }
    Result result;
    result.ok = true;
    result.url = DriveLh3Url(id);
    return result;
  }
  if (status == 401 || status == 403) {
    return Failure("drive-http-" + std::to_string(status));
  }
  if (status == 429) {
    return Failure("drive-quota");
  }
  return Failure("drive-http-" + std::to_string(status));
}

std::string BuildDriveFolderQuery() {
  const std::string query = "name='" + std::string(kDriveFolderName) +
                            "' and mimeType='application/vnd.google-apps.folder' and trashed=false";
  return "/drive/v3/files?q=" + Utils::UriEncode(query) +
         "&spaces=drive&pageSize=10&fields=files(id)";
}

std::string BuildDriveFolderMetadata() {
  return std::string("{\"name\":\"") + kDriveFolderName +
         "\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
}

std::string BuildDrivePermissionMetadata() { return "{\"role\":\"reader\",\"type\":\"anyone\"}"; }

enum class DrivePermissionVerdict {
  kGranted,        // 200/201: the grant was created now
  kAlreadyShared,  // 409, or a 4xx (other than 400/403) whose body says it exists
  kDenied,         // 403: the API refused to share (org policy, sharing disabled)
  kRejected,       // 400 and every other status: a malformed request or a fault
};

bool BodySaysPermissionExists(const std::string& body) {
  std::string lower;
  lower.reserve(body.size() < 4096 ? body.size() : 4096);
  for (size_t i = 0; i < body.size() && lower.size() < 4096; ++i) {
    const char c = body[i];
    lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return lower.find("already exists") != std::string::npos ||
         lower.find("alreadyexists") != std::string::npos ||
         lower.find("already shared") != std::string::npos;
}

DrivePermissionVerdict ClassifyDrivePermission(DWORD status, const std::string& body) {
  if (status >= 200 && status < 300) {
    return DrivePermissionVerdict::kGranted;
  }
  if (status == 400) {
    return DrivePermissionVerdict::kRejected;
  }
  if (status == 403) {
    return DrivePermissionVerdict::kDenied;
  }
  if (status == 409) {
    return DrivePermissionVerdict::kAlreadyShared;
  }
  if (status >= 400 && status < 500 && BodySaysPermissionExists(body)) {
    return DrivePermissionVerdict::kAlreadyShared;
  }
  return DrivePermissionVerdict::kRejected;
}


constexpr uint32_t kSha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr uint32_t kSha256InitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

uint32_t RotateRight(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

void Sha256Transform(uint32_t state[8], const unsigned char block[64]) {
  uint32_t schedule[64];
  for (size_t i = 0; i < 16; ++i) {
    schedule[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
                  static_cast<uint32_t>(block[i * 4 + 2]) << 8 | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
    const uint32_t s1 =
        RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + sigma1 + choice + kSha256RoundConstants[i] + schedule[i];
    const uint32_t sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

class Sha256 {
 public:
  Sha256() { std::copy(kSha256InitialState, kSha256InitialState + 8, state_); }

  void Update(const unsigned char* data, size_t length) {
    if (data == nullptr || length == 0) {
      return;
    }
    total_bytes_ += length;
    size_t offset = 0;
    if (buffer_size_ > 0) {
      const size_t needed = sizeof(buffer_) - buffer_size_;
      const size_t take = length < needed ? length : needed;
      std::memcpy(buffer_ + buffer_size_, data, take);
      buffer_size_ += take;
      offset = take;
      if (buffer_size_ == sizeof(buffer_)) {
        Sha256Transform(state_, buffer_);
        buffer_size_ = 0;
      }
    }
    while (length - offset >= sizeof(buffer_)) {
      Sha256Transform(state_, data + offset);
      offset += sizeof(buffer_);
    }
    if (offset < length) {
      std::memcpy(buffer_, data + offset, length - offset);
      buffer_size_ = length - offset;
    }
  }

  void Final(unsigned char digest[32]) {
    const uint64_t total_bits = total_bytes_ * 8;
    unsigned char padding[72] = {};
    padding[0] = 0x80;
    const size_t pad_length = buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
    Update(padding, pad_length);
    unsigned char length_bytes[8];
    for (size_t i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<unsigned char>(total_bits >> (56 - i * 8));
    }
    Update(length_bytes, 8);
    for (size_t i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
      digest[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
      digest[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
      digest[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
    }
  }

 private:
  uint32_t state_[8] = {};
  unsigned char buffer_[64] = {};
  size_t buffer_size_ = 0;
  uint64_t total_bytes_ = 0;
};

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  unsigned char digest[32] = {};
  Sha256 hasher;
  hasher.Update(bytes.empty() ? nullptr : bytes.data(), bytes.size());
  hasher.Final(digest);
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0f]);
  }
  return hex;
}

//
// RFC 7636 S256: a 43-character base64url verifier (32 random bytes) and its
// challenge, the base64url SHA-256 of the verifier. Neither value is secret in
// the sense of a credential - the challenge travels in the browser URL and the
// verifier only in the token POST - but both are unguessable.

std::string Base64UrlEncode(const unsigned char* data, size_t length) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string encoded;
  encoded.reserve((length * 4 + 2) / 3);
  size_t i = 0;
  while (i + 3 <= length && data != nullptr) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
    encoded.push_back(kAlphabet[chunk & 0x3f]);
    i += 3;
  }
  const size_t remaining = length - i;
  if (data != nullptr && remaining == 1) {
    const uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
  } else if (data != nullptr && remaining == 2) {
    const uint32_t chunk =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3f]);
  }
  return encoded;
}

std::string RandomToken(size_t byte_count) {
  std::random_device source;
  std::vector<unsigned char> bytes(byte_count);
  for (size_t i = 0; i < byte_count; ++i) {
    bytes[i] = static_cast<unsigned char>(source() & 0xffu);
  }
  return Base64UrlEncode(bytes.data(), bytes.size());
}

std::string PkceCodeVerifier() { return RandomToken(32); }

std::string PkceCodeChallenge(const std::string& verifier) {
  unsigned char digest[32] = {};
  Sha256 hasher;
  hasher.Update(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size());
  hasher.Final(digest);
  return Base64UrlEncode(digest, sizeof(digest));
}

std::string OAuthStateToken() { return RandomToken(16); }

std::string BuildDriveAuthUrl(const std::string& client_id, const std::string& redirect_uri,
                              const std::string& verifier, const std::string& state) {
  std::string url = kDriveAuthEndpoint;
  url += "?client_id=";
  url += Utils::UriEncode(client_id);
  url += "&redirect_uri=";
  url += Utils::UriEncode(redirect_uri);
  url += "&response_type=code";
  url += "&scope=";
  url += Utils::UriEncode(std::string(kDriveScope));
  url += "&access_type=offline";
  url += "&prompt=consent";
  url += "&code_challenge=";
  url += PkceCodeChallenge(verifier);
  url += "&code_challenge_method=S256";
  url += "&state=";
  url += Utils::UriEncode(state);
  return url;
}


struct CompressionPlan {
  bool reencode = false;
  UINT width = 0;
  UINT height = 0;
};

CompressionPlan PlanCompression(UINT width, UINT height, size_t byte_size) {
  CompressionPlan plan;
  if (width == 0 || height == 0) {
    return plan;
  }
  if (width <= kMaxImageDimension && height <= kMaxImageDimension &&
      byte_size <= kSkipReencodeMaxBytes) {
    return plan;
  }
  plan.reencode = true;
  const UINT shorter = width < height ? width : height;
  const UINT side = shorter > kMaxImageDimension ? kMaxImageDimension : shorter;
  plan.width = side;
  plan.height = side;
  return plan;
}

struct PreparedImage {
  std::vector<unsigned char> bytes;  // exactly what gets uploaded
  std::string mime;                  // announces those bytes
  std::string note;                  // one-line size/dimension record for the log
  bool reencoded = false;
};

std::string HexHResult(HRESULT hr) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
  return buffer;
}

std::string ImageNote(size_t input_bytes, size_t output_bytes, UINT source_width, UINT source_height,
                      UINT output_width, UINT output_height, const char* mode) {
  return "input=" + std::to_string(input_bytes) + " output=" + std::to_string(output_bytes) +
         " dims=" + std::to_string(source_width) + "x" + std::to_string(source_height) + "->" +
         std::to_string(output_width) + "x" + std::to_string(output_height) + " mode=" + mode;
}

PreparedImage OriginalImage(const std::vector<unsigned char>& source, const char* mime, UINT width, UINT height,
                            const std::string& wic_error) {
  PreparedImage prepared;
  prepared.bytes = source;
  prepared.mime = mime != nullptr ? std::string(mime) : std::string();
  prepared.note = ImageNote(source.size(), source.size(), width, height, width, height, "original");
  if (!wic_error.empty()) {
    prepared.note += " wic-failed=" + wic_error;
  }
  return prepared;
}

PreparedImage PrepareImageForUpload(const std::vector<unsigned char>& source, const char* mime) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool own_com = SUCCEEDED(com);
  PreparedImage prepared;
  UINT source_width = 0;
  UINT source_height = 0;
  std::string error;

  {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> input_stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) {
      hr = factory->CreateStream(&input_stream);
    }
    if (SUCCEEDED(hr)) {
      hr = input_stream->InitializeFromMemory(const_cast<BYTE*>(source.data()), static_cast<DWORD>(source.size()));
    }
    if (SUCCEEDED(hr)) {
      hr = factory->CreateDecoderFromStream(input_stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) {
      hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
      hr = frame->GetSize(&source_width, &source_height);
    }
    if (FAILED(hr)) {
      error = "decode:" + HexHResult(hr);
    }

    const CompressionPlan plan = PlanCompression(source_width, source_height, source.size());
    if (error.empty() && !plan.reencode) {
      prepared = OriginalImage(source, mime, source_width, source_height, std::string());
    } else if (error.empty()) {
      Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
      Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
      Microsoft::WRL::ComPtr<IStream> output_stream;
      Microsoft::WRL::ComPtr<IWICStream> wic_output;
      Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
      Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> encoder_frame;
      Microsoft::WRL::ComPtr<IPropertyBag2> properties;

      hr = factory->CreateBitmapClipper(&clipper);
      if (SUCCEEDED(hr)) {
        const UINT shorter = source_width < source_height ? source_width : source_height;
        WICRect crop = {};
        crop.X = static_cast<INT>((source_width - shorter) / 2);
        crop.Y = static_cast<INT>((source_height - shorter) / 2);
        crop.Width = static_cast<INT>(shorter);
        crop.Height = static_cast<INT>(shorter);
        hr = clipper->Initialize(frame.Get(), &crop);
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapScaler(&scaler);
      }
      if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(clipper.Get(), plan.width, plan.height, WICBitmapInterpolationModeFant);
      }
      if (SUCCEEDED(hr)) {
        hr = CreateStreamOnHGlobal(nullptr, TRUE, output_stream.GetAddressOf());
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(&wic_output);
      }
      if (SUCCEEDED(hr)) {
        hr = wic_output->InitializeFromIStream(output_stream.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(wic_output.Get(), WICBitmapEncoderNoCache);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&encoder_frame, &properties);
      }
      if (SUCCEEDED(hr)) {
        wchar_t quality_name[] = L"ImageQuality";
        PROPBAG2 option = {};
        option.pstrName = quality_name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = kJpegQuality;
        hr = properties->Write(1, &option, &value);
        VariantClear(&value);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->Initialize(properties.Get());
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->SetSize(plan.width, plan.height);
      }
      WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->SetPixelFormat(&pixel_format);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->WriteSource(scaler.Get(), nullptr);
      }
      if (SUCCEEDED(hr)) {
        hr = encoder_frame->Commit();
      }
      if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
      }

      HGLOBAL memory = nullptr;
      const void* data = nullptr;
      SIZE_T size = 0;
      if (SUCCEEDED(hr)) {
        hr = GetHGlobalFromStream(output_stream.Get(), &memory);
      }
      if (SUCCEEDED(hr) && memory != nullptr) {
        data = GlobalLock(memory);
        size = GlobalSize(memory);
        if (data == nullptr || size == 0) {
          hr = E_FAIL;
        }
      } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
      }
      if (SUCCEEDED(hr)) {
        prepared.bytes.assign(static_cast<const unsigned char*>(data),
                              static_cast<const unsigned char*>(data) + size);
        prepared.mime = "image/jpeg";
        prepared.note = ImageNote(source.size(), prepared.bytes.size(), source_width, source_height, plan.width,
                                  plan.height, "jpeg-q85");
        prepared.reencoded = true;
      } else {
        error = "encode:" + HexHResult(hr);
      }
      if (data != nullptr) {
        GlobalUnlock(memory);
      }
    }
  }

  if (own_com) {
    CoUninitialize();
  }
  if (!error.empty()) {
    return OriginalImage(source, mime, source_width, source_height, error);
  }
  return prepared;
}

std::string UploadCacheKey(const PreparedImage& image) { return Sha256Hex(image.bytes); }


struct CacheEntry {
  bool ok = false;
  std::string payload;  // URL when ok, failure reason otherwise
  long long expires_at = 0;
};

std::mutex g_cache_mutex;
std::string g_cache_path;
std::map<std::string, CacheEntry> g_cache;

bool IsCacheKey(const std::string& key) {
  if (key.size() != 64) {
    return false;
  }
  for (char c : key) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool ParseLongLong(const std::string& text, long long* value) {
  if (text.empty() || text.size() > 18) {
    return false;
  }
  long long parsed = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }
  *value = parsed;
  return true;
}

std::string SanitizeCacheText(const std::string& text) {
  std::string clean;
  clean.reserve(std::min<size_t>(text.size(), 200));
  for (char c : text) {
    if (clean.size() >= 200) {
      break;
    }
    clean.push_back(c == '\t' || c == '\r' || c == '\n' ? ' ' : c);
  }
  return clean;
}

bool ParseCacheLine(const std::string& line, std::string* key, CacheEntry* entry) {
  if (line.empty() || line[0] == '#') {
    return false;
  }
  const size_t first = line.find('\t');
  if (first == std::string::npos || !IsCacheKey(line.substr(0, first))) {
    return false;
  }
  const size_t second = line.find('\t', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const std::string state = line.substr(first + 1, second - first - 1);
  const std::string payload = line.substr(second + 1);
  CacheEntry parsed;
  if (state == "ok") {
    if (!LooksLikeUploadUrl(payload) && !IsLegacyGitHubCachedUrl(payload)) {
      return false;
    }
    parsed.ok = true;
    parsed.payload = payload;
  } else if (state == "fail") {
    const size_t third = payload.find('\t');
    if (third == std::string::npos || !ParseLongLong(payload.substr(0, third), &parsed.expires_at) ||
        parsed.expires_at <= 0) {
      return false;
    }
    parsed.ok = false;
    parsed.payload = payload.substr(third + 1);
  } else {
    return false;
  }
  *key = line.substr(0, first);
  *entry = parsed;
  return true;
}

std::string FormatCacheLine(const std::string& key, const CacheEntry& entry) {
  if (entry.ok) {
    return key + "\tok\t" + entry.payload;
  }
  return key + "\tfail\t" + std::to_string(entry.expires_at) + "\t" + entry.payload;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  out->clear();
  const std::wstring wide = Utils::ToWString(path);
  if (wide.empty()) {
    return false;
  }
  HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buffer[4096];
  bool ok = true;
  for (;;) {
    DWORD read = 0;
    if (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) == FALSE) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    out->append(buffer, read);
    if (out->size() > kMaxCacheBytes) {
      ok = false;
      break;
    }
  }
  CloseHandle(file);
  if (!ok) {
    out->clear();
  }
  return ok;
}

bool WriteWholeFile(const std::string& path, const std::string& content) {
  const std::wstring wide = Utils::ToWString(path);
  if (wide.empty()) {
    return false;
  }
  const std::wstring temp = wide + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool ok = true;
  const char* data = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1 << 20));
    DWORD written = 0;
    if (WriteFile(file, data, chunk, &written, nullptr) == FALSE || written != chunk) {
      ok = false;
      break;
    }
    data += chunk;
    remaining -= chunk;
  }
  CloseHandle(file);
  if (!ok || MoveFileExW(temp.c_str(), wide.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

void LoadCacheLocked() {
  g_cache.clear();
  std::string content;
  if (!ReadWholeFile(g_cache_path, &content)) {
    return;
  }
  size_t start = 0;
  for (;;) {
    const size_t end = content.find('\n', start);
    const std::string line =
        end == std::string::npos ? content.substr(start) : content.substr(start, end - start);
    std::string key;
    CacheEntry entry;
    if (ParseCacheLine(line, &key, &entry)) {
      g_cache[key] = entry;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

void SaveCacheLocked() {
  std::string content = "# cover publisher cache v1\n";
  for (const auto& item : g_cache) {
    content += FormatCacheLine(item.first, item.second);
    content += "\n";
  }
  WriteWholeFile(g_cache_path, content);  // best effort: unwritable paths degrade to memory only
}

bool CacheLookup(const std::string& key, long long now, Result* out) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache_path.empty()) {
    return false;
  }
  const auto found = g_cache.find(key);
  if (found == g_cache.end()) {
    return false;
  }
  if (!found->second.ok && found->second.expires_at <= now) {
    g_cache.erase(found);
    SaveCacheLocked();
    return false;
  }
  Result result;
  if (found->second.ok) {
    result.ok = true;
    result.url = found->second.payload;
    result.host = HostNameForUrl(result.url);
  } else {
    result.reason = found->second.payload;
  }
  *out = result;
  return true;
}

void CacheStore(const std::string& key, const Result& result, long long now) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache_path.empty()) {
    return;
  }
  const auto kept = g_cache.find(key);
  if (!result.ok && kept != g_cache.end() && kept->second.ok) {
    return;
  }
  CacheEntry entry;
  if (result.ok && (LooksLikeUploadUrl(result.url) || IsLegacyGitHubCachedUrl(result.url))) {
    entry.ok = true;
    entry.payload = result.url;
  } else {
    entry.ok = false;
    entry.expires_at = now + kNegativeCacheTtlSeconds;
    entry.payload = SanitizeCacheText(result.ok ? std::string("unexpected-body") : result.reason);
  }
  g_cache[key] = entry;
  SaveCacheLocked();
}


struct DriveState {
  std::string refresh_token;
  std::string folder_id;
};

bool IsDriveRefreshToken(const std::string& token) {
  if (token.size() < 16 || token.size() > 2048) {
    return false;
  }
  for (char c : token) {
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '\'') {
      return false;
    }
  }
  return true;
}

bool ParseDriveState(const std::string& content, DriveState* state) {
  DriveState parsed;
  size_t start = 0;
  for (;;) {
    const size_t end = content.find('\n', start);
    std::string line =
        end == std::string::npos ? content.substr(start) : content.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && line[0] != '#') {
      const size_t equals = line.find('=');
      if (equals == std::string::npos || equals == 0) {
        return false;  // a line that is not key=value: corrupt
      }
      const std::string key = line.substr(0, equals);
      const std::string value = line.substr(equals + 1);
      if (key == "refresh_token") {
        parsed.refresh_token = value;
      } else if (key == "folder_id") {
        parsed.folder_id = LooksLikeDriveFileId(value) ? value : std::string();
      } else {
        return false;  // an unknown key means this is not our file
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  if (!IsDriveRefreshToken(parsed.refresh_token)) {
    return false;
  }
  *state = parsed;
  return true;
}

std::string FormatDriveState(const DriveState& state) {
  std::string content = "# aimp-discord-presence drive state v1\n";
  content += "refresh_token=" + state.refresh_token + "\n";
  if (!state.folder_id.empty()) {
    content += "folder_id=" + state.folder_id + "\n";
  }
  return content;
}

bool LoadDriveState(const std::string& path, DriveState* state) {
  if (path.empty()) {
    return false;
  }
  std::string content;
  if (!ReadWholeFile(path, &content)) {
    return false;
  }
  return ParseDriveState(content, state);
}

bool SaveDriveState(const std::string& path, const DriveState& state) {
  if (path.empty()) {
    return false;
  }
  return WriteWholeFile(path, FormatDriveState(state));
}


class WinHttpHandle {
 public:
  explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
  ~WinHttpHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        WinHttpCloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  HINTERNET get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

class Deadline {
 public:
  explicit Deadline(DWORD budget_ms)
      : end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms)) {}

  bool Expired() const { return std::chrono::steady_clock::now() >= end_; }

  DWORD RemainingMs() const {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) {
      return 1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count();
    if (remaining < 1) {
      return 1;
    }
    if (remaining > static_cast<long long>(kTotalTimeoutMs)) {
      return kTotalTimeoutMs;
    }
    return static_cast<DWORD>(remaining);
  }

 private:
  std::chrono::steady_clock::time_point end_;
};

WinHttpHandle OpenSession(const Deadline& deadline) {
  WinHttpHandle session(WinHttpOpen(kAgentName, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    return session;
  }
  const int timeout = static_cast<int>(deadline.RemainingMs());
  WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
  return session;
}

bool QueryStatusCode(HINTERNET request, DWORD* status, DWORD* error) {
  DWORD size = sizeof(DWORD);
  *status = 0;
  *error = 0;
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX) == FALSE) {
    *error = GetLastError();
    return false;
  }
  return true;
}

bool QueryContentType(HINTERNET request, std::wstring* content_type) {
  DWORD size = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (size == 0) {
    return false;
  }
  std::wstring buffer(size / sizeof(wchar_t), L'\0');
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return false;
  }
  buffer.resize(size / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  *content_type = buffer;
  return true;
}

bool StartsWithImageType(const std::wstring& content_type) {
  const wchar_t kPrefix[] = L"image/";
  if (content_type.size() < 6) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    wchar_t c = content_type[i];
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (c != kPrefix[i]) {
      return false;
    }
  }
  return true;
}

struct ReadOutcome {
  bool ok = false;
  const char* tag = "read-failed";
  DWORD error = 0;
};

ReadOutcome ReadResponseBody(HINTERNET request, const Deadline& deadline, std::string* out,
                             size_t max_bytes = kMaxResponseBytes) {
  out->clear();
  ReadOutcome outcome;
  for (;;) {
    if (deadline.Expired()) {
      outcome.tag = "read-timeout";
      outcome.error = ERROR_WINHTTP_TIMEOUT;  // our own budget ran out
      return outcome;
    }
    DWORD available = 0;
    if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
      outcome.tag = "data-available-failed";
      outcome.error = GetLastError();
      return outcome;
    }
    if (available == 0) {
      outcome.ok = true;
      return outcome;
    }
    if (available > max_bytes) {
      available = static_cast<DWORD>(max_bytes);
    }
    const size_t offset = out->size();
    out->resize(offset + available);
    DWORD read = 0;
    if (WinHttpReadData(request, out->data() + offset, available, &read) == FALSE) {
      outcome.tag = "read-data-failed";
      outcome.error = GetLastError();
      return outcome;
    }
    out->resize(offset + read);
    if (read == 0 || out->size() >= max_bytes) {
      outcome.ok = true;
      return outcome;
    }
  }
}

struct UploadAttempt {
  Result result;
  bool transport_failure = false;
};

std::string TransportReason(const char* tag, const char* phase, DWORD error) {
  const char* category = error == ERROR_WINHTTP_TIMEOUT ? "timeout" : phase;
  return std::string(tag) + " phase=" + category + " winhttp=" + std::to_string(error);
}

UploadAttempt TransportFailure(const char* tag, const char* phase, DWORD error) {
  UploadAttempt attempt;
  attempt.result = Failure(TransportReason(tag, phase, error));
  attempt.transport_failure = true;
  return attempt;
}

UploadAttempt AttemptResult(const Result& result) {
  UploadAttempt attempt;
  attempt.result = result;
  return attempt;
}

UploadAttempt UploadMultipartBody(const UploadHost& upload_host, const std::string& body,
                                  const std::string& boundary) {
  if (body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    return TransportFailure("session-open-failed", "connect", GetLastError());
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), upload_host.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    return TransportFailure("connect-failed", "connect", GetLastError());
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"POST", upload_host.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    return TransportFailure("request-open-failed", "send", GetLastError());
  }
  const std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + Utils::ToWString(boundary);
  const DWORD body_size = static_cast<DWORD>(body.size());
  if (WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                         body_size, body_size, 0) == FALSE) {
    return TransportFailure("send-failed", "send", GetLastError());
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return TransportFailure("receive-failed", "receive", GetLastError());
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    return AttemptResult(Failure("status-query-failed phase=receive winhttp=" + std::to_string(status_error)));
  }
  std::string response;
  const ReadOutcome read = ReadResponseBody(request.get(), deadline, &response);
  if (!read.ok) {
    if (status == 200 || status == 412) {
      return AttemptResult(ClassifyUploadResponse(status, response, upload_host.shape));
    }
    return TransportFailure(read.tag, "receive", read.error);
  }
  return AttemptResult(ClassifyUploadResponse(status, response, upload_host.shape));
}


struct DriveConfig {
  std::string client_id;
  std::string client_secret;
  std::string state_path;
};

DriveConfig SnapshotDriveConfig() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  DriveConfig config;
  config.client_id = g_drive_client_id;
  config.client_secret = g_drive_client_secret;
  config.state_path = g_drive_state_path;
  return config;
}

bool DriveConfigured(const DriveConfig& config) {
  return !config.client_id.empty() && !config.client_secret.empty() && !config.state_path.empty();
}

struct HttpResponse {
  bool transport_ok = false;
  DWORD status = 0;
  std::string body;
  std::string transport_reason;
};

HttpResponse SendHttp(const std::wstring& method, const std::wstring& host, const std::wstring& path,
                      const std::wstring& headers, const std::string& body,
                      size_t max_bytes = kMaxDriveResponseBytes) {
  HttpResponse response;
  if (body.size() > 0xffffffffull) {
    response.transport_reason = "body-too-large";
    return response;
  }
  const Deadline deadline(kTotalTimeoutMs);
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    response.transport_reason = TransportReason("session-open-failed", "connect", GetLastError());
    return response;
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    response.transport_reason = TransportReason("connect-failed", "connect", GetLastError());
    return response;
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE));
  if (!request) {
    response.transport_reason = TransportReason("request-open-failed", "send", GetLastError());
    return response;
  }
  const DWORD body_size = static_cast<DWORD>(body.size());
  if (WinHttpSendRequest(request.get(),
                         headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                         headers.empty() ? 0 : static_cast<DWORD>(-1),
                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                         body_size, body_size, 0) == FALSE) {
    response.transport_reason = TransportReason("send-failed", "send", GetLastError());
    return response;
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    response.transport_reason = TransportReason("receive-failed", "receive", GetLastError());
    return response;
  }
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &response.status, &status_error)) {
    response.transport_ok = true;
    response.status = 0;
    response.transport_reason = "status-query-failed phase=receive winhttp=" +
                                std::to_string(status_error);
    return response;
  }
  const ReadOutcome read = ReadResponseBody(request.get(), deadline, &response.body, max_bytes);
  if (!read.ok) {
    response.transport_reason = TransportReason(read.tag, "receive", read.error);
    return response;
  }
  response.transport_ok = true;
  return response;
}

std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string body;
  for (const auto& field : fields) {
    if (!body.empty()) {
      body += "&";
    }
    body += Utils::UriEncode(field.first);
    body += "=";
    body += Utils::UriEncode(field.second);
  }
  return body;
}

struct DriveTokenGrant {
  std::string access_token;
  std::string refresh_token;
  long long expires_in = 3600;
};

bool ParseDriveTokenGrant(const std::string& body, DriveTokenGrant* grant) {
  const std::string text = TrimWhitespace(body);
  const std::string access = ExtractJsonStringField(text, "access_token");
  if (access.empty()) {
    return false;
  }
  DriveTokenGrant parsed;
  parsed.access_token = access;
  const std::string refresh = ExtractJsonStringField(text, "refresh_token");
  if (!refresh.empty()) {
    parsed.refresh_token = refresh;
  }
  const long long expires = ExtractJsonNumberField(text, "expires_in");
  if (expires > 0) {
    parsed.expires_in = expires;
  }
  *grant = parsed;
  return true;
}

bool PostDriveToken(const std::vector<std::pair<std::string, std::string>>& fields,
                    DriveTokenGrant* grant, std::string* reason) {
  const HttpResponse response =
      SendHttp(L"POST", L"oauth2.googleapis.com", L"/token",
               L"Content-Type: application/x-www-form-urlencoded\r\n", FormEncode(fields));
  if (!response.transport_ok) {
    *reason = response.transport_reason.empty() ? std::string("token-transport-failed")
                                                : "token-" + response.transport_reason;
    return false;
  }
  if (response.status != 200) {
    *reason = "token-http-" + std::to_string(response.status);
    return false;
  }
  if (!ParseDriveTokenGrant(response.body, grant)) {
    *reason = "token-unexpected-body";
    return false;
  }
  return true;
}

std::mutex g_drive_session_mutex;
std::string g_drive_access_token;
long long g_drive_access_expires_at = 0;
std::string g_drive_folder_id;

std::atomic<bool> g_drive_consent_cancel{false};
std::atomic<long long> g_drive_consent_failed_at{0};


bool EnsureWinsock(std::string* reason) {
  static std::once_flag once;
  static bool started = false;
  std::call_once(once, []() {
    WSADATA data = {};
    started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  if (!started) {
    *reason = "consent-winsock-unavailable";
  }
  return started;
}

struct LoopbackParams {
  std::string code;
  std::string state;
  std::string error;
};

std::string QueryParameter(const std::string& query, const std::string& key) {
  size_t start = 0;
  for (;;) {
    const size_t end = query.find('&', start);
    const std::string pair =
        end == std::string::npos ? query.substr(start) : query.substr(start, end - start);
    const size_t equals = pair.find('=');
    const std::string name = equals == std::string::npos ? pair : pair.substr(0, equals);
    if (name == key) {
      return Utils::UriDecode(equals == std::string::npos ? std::string() : pair.substr(equals + 1));
    }
    if (end == std::string::npos) {
      return std::string();
    }
    start = end + 1;
  }
}

LoopbackParams ParseLoopbackTarget(const std::string& target) {
  LoopbackParams params;
  const size_t question = target.find('?');
  if (question == std::string::npos) {
    return params;
  }
  const std::string query = target.substr(question + 1);
  params.code = QueryParameter(query, "code");
  params.state = QueryParameter(query, "state");
  params.error = QueryParameter(query, "error");
  return params;
}

std::string RequestTarget(const std::string& request) {
  const size_t line_end = request.find("\r\n");
  const std::string line = line_end == std::string::npos ? request : request.substr(0, line_end);
  const size_t first = line.find(' ');
  if (first == std::string::npos) {
    return std::string();
  }
  const size_t second = line.find(' ', first + 1);
  if (second == std::string::npos) {
    return std::string();
  }
  return line.substr(first + 1, second - first - 1);
}

std::string ConsentPage(const char* message) {
  const std::string body =
      std::string("<!doctype html><html><head><meta charset=\"utf-8\">"
                  "<title>AIMP Discord Presence</title></head><body><p>") +
      message + "</p></body></html>";
  return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

struct ConsentCallback {
  bool ok = false;
  std::string code;
  std::string error;  // "cancelled", "timeout", "unexpected-request", or Google's error code
};


long long NowMillis() {
  return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

long long RemainingMillis(long long deadline_ms, long long now_ms) {
  return now_ms < deadline_ms ? deadline_ms - now_ms : 0;
}

long long ReceiveSliceMillis(long long deadline_ms, long long now_ms, long long cap_ms) {
  const long long remaining = RemainingMillis(deadline_ms, now_ms);
  if (remaining <= 0 || cap_ms <= 0) {
    return 0;
  }
  return remaining < cap_ms ? remaining : cap_ms;
}

constexpr size_t kDriveConsentMaxConnections = 2;

class SocketHandle {
 public:
  explicit SocketHandle(SOCKET socket = INVALID_SOCKET) : socket_(socket) {}
  ~SocketHandle() { Reset(); }
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  SOCKET get() const { return socket_; }
  bool valid() const { return socket_ != INVALID_SOCKET; }

  void Reset(SOCKET socket = INVALID_SOCKET) {
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
    }
    socket_ = socket;
  }

 private:
  SOCKET socket_;
};

enum class ConsentStep { kKeepWaiting, kAccept, kCancelled, kExpired };

ConsentStep DecideConsentStep(int select_result, bool cancelled, long long now_ms,
                              long long deadline_ms) {
  if (cancelled) {
    return ConsentStep::kCancelled;
  }
  if (RemainingMillis(deadline_ms, now_ms) <= 0) {
    return ConsentStep::kExpired;
  }
  return select_result > 0 ? ConsentStep::kAccept : ConsentStep::kKeepWaiting;
}

std::string ConsentErrorToken(const std::string& error) {
  if (error == "access_denied") {
    return "access_denied";
  }
  if (error == "invalid_scope") {
    return "invalid_scope";
  }
  if (error == "invalid_request") {
    return "invalid_request";
  }
  return "error";
}

ConsentCallback WaitForConsentCallback(SOCKET listener, const std::string& expected_state,
                                       long long deadline_ms) {
  ConsentCallback callback;
  const std::string waiting_page = ConsentPage("Waiting for the authorization reply. Return to AIMP.");
  size_t accepted = 0;
  for (;;) {
    if (accepted >= kDriveConsentMaxConnections) {
      callback.error = "timeout";
      return callback;
    }
    const long long wait_ms = ReceiveSliceMillis(deadline_ms, NowMillis(), kDriveConsentPollMs);
    if (wait_ms <= 0) {
      callback.error = "timeout";
      return callback;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener, &read_set);
    timeval slice = {};
    slice.tv_sec = static_cast<long>(wait_ms / 1000);
    slice.tv_usec = static_cast<long>((wait_ms % 1000) * 1000);
    const int ready = select(0, &read_set, nullptr, nullptr, &slice);
    const ConsentStep step =
        DecideConsentStep(ready, g_drive_consent_cancel.load(), NowMillis(), deadline_ms);
    if (step == ConsentStep::kKeepWaiting) {
      if (ready < 0) {
        Sleep(kDriveConsentPollMs);  // a select error must not spin the worker
      }
      continue;  // a poll tick or a spurious wakeup: nothing has been consumed
    }
    if (step == ConsentStep::kCancelled) {
      callback.error = "cancelled";
      return callback;
    }
    if (step == ConsentStep::kExpired) {
      callback.error = "timeout";
      return callback;
    }
    SocketHandle client(accept(listener, nullptr, nullptr));
    if (!client.valid()) {
      continue;  // the connection died before accept: no client was served
    }
    ++accepted;  // only a socket actually accepted consumes the budget

    const long long receive_ms =
        ReceiveSliceMillis(deadline_ms, NowMillis(), kDriveReceiveTimeoutMs);
    if (receive_ms <= 0) {
      callback.error = "timeout";
      return callback;  // the client socket is closed by the guard
    }
    DWORD receive_timeout = static_cast<DWORD>(receive_ms);
    setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout));

    std::string request;
    char buffer[1024];
    for (;;) {
      if (g_drive_consent_cancel.load()) {
        callback.error = "cancelled";
        return callback;
      }
      if (ReceiveSliceMillis(deadline_ms, NowMillis(), kDriveReceiveTimeoutMs) <= 0) {
        callback.error = "timeout";
        return callback;
      }
      const int received = recv(client.get(), buffer, static_cast<int>(sizeof(buffer)), 0);
      if (received <= 0) {
        break;  // closed, or the receive timeout fired: drop this connection
      }
      request.append(buffer, static_cast<size_t>(received));
      if (request.find("\r\n\r\n") != std::string::npos ||
          request.size() >= kMaxLoopbackRequestBytes) {
        break;
      }
    }
    if (g_drive_consent_cancel.load()) {
      callback.error = "cancelled";
      return callback;
    }
    if (request.find("\r\n\r\n") == std::string::npos) {
      continue;  // a silent preconnect or a truncated request: drop it, keep waiting
    }

    const LoopbackParams params = ParseLoopbackTarget(RequestTarget(request));
    const bool answers_this_consent =
        params.state == expected_state && (!params.code.empty() || !params.error.empty());
    const std::string page =
        answers_this_consent
            ? ConsentPage(params.error.empty()
                              ? "Google Drive is connected. You can close this tab."
                              : "Authorization was not granted. You can close this tab.")
            : waiting_page;
    send(client.get(), page.data(), static_cast<int>(page.size()), 0);
    client.Reset();  // close before answering, so the browser's page is complete

    if (answers_this_consent) {
      callback.ok = params.error.empty();
      callback.code = params.code;
      callback.error =
          params.error.empty() ? std::string() : ConsentErrorToken(params.error);
      return callback;
    }
    callback.error = "unexpected-request";
    return callback;
  }
}

bool RunDriveConsent(const DriveConfig& config, DriveState* state, std::string* reason) {
  const long long consent_deadline_ms = NowMillis() + static_cast<long long>(kDriveConsentWindowMs);
  std::string winsock_reason;
  if (!EnsureWinsock(&winsock_reason)) {
    *reason = winsock_reason;
    return false;
  }
  if (g_drive_consent_cancel.load()) {
    *reason = "consent-cancelled";
    return false;
  }

  SocketHandle listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (!listener.valid()) {
    *reason = "consent-socket-failed";
    return false;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;  // ephemeral port, chosen by the OS
  bool listening =
      bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
      listen(listener.get(), 1) == 0;
  unsigned short port = 0;
  if (listening) {
    sockaddr_in bound = {};
    int bound_length = static_cast<int>(sizeof(bound));
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
      port = ntohs(bound.sin_port);
    }
    listening = port != 0;
  }
  if (!listening) {
    *reason = "consent-listen-failed";
    return false;  // the socket is closed by the guard
  }

  const std::string redirect_uri = "http://127.0.0.1:" + std::to_string(port) + "/";
  const std::string verifier = PkceCodeVerifier();
  const std::string oauth_state = OAuthStateToken();
  const std::string auth_url = BuildDriveAuthUrl(config.client_id, redirect_uri, verifier, oauth_state);

  const HINSTANCE launched =
      ShellExecuteA(nullptr, "open", auth_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(launched) <= 32) {
    *reason = "consent-browser-open-failed";
    return false;
  }
  EmitLog("local-cover drive auth=consent port=" + std::to_string(port));

  const ConsentCallback callback =
      WaitForConsentCallback(listener.get(), oauth_state, consent_deadline_ms);
  listener.Reset();  // stop listening before the token exchange
  if (!callback.ok) {
    *reason = callback.error.empty() ? std::string("consent-failed") : "consent-" + callback.error;
    EmitLog("local-cover drive auth=declined reason=" + *reason);
    return false;
  }
  if (RemainingMillis(consent_deadline_ms, NowMillis()) <
      static_cast<long long>(kTotalTimeoutMs)) {
    *reason = "consent-timeout";
    EmitLog("local-cover drive auth=declined reason=" + *reason);
    return false;
  }

  std::vector<std::pair<std::string, std::string>> fields;
  fields.emplace_back("code", callback.code);
  fields.emplace_back("client_id", config.client_id);
  fields.emplace_back("client_secret", config.client_secret);
  fields.emplace_back("redirect_uri", redirect_uri);
  fields.emplace_back("grant_type", "authorization_code");
  fields.emplace_back("code_verifier", verifier);
  DriveTokenGrant grant;
  if (!PostDriveToken(fields, &grant, reason)) {
    EmitLog("local-cover drive auth=exchange-failed reason=" + *reason);
    return false;
  }
  if (!IsDriveRefreshToken(grant.refresh_token)) {
    *reason = "consent-no-refresh-token";
    EmitLog("local-cover drive auth=failed reason=" + *reason);
    return false;
  }
  DriveState consented;
  consented.refresh_token = grant.refresh_token;
  consented.folder_id = state->folder_id;  // a re-consent keeps the known folder
  if (!SaveDriveState(config.state_path, consented)) {
    EmitLog("local-cover drive auth=consented state=unwritten");
  } else {
    EmitLog("local-cover drive auth=consented");
  }
  *state = consented;
  return true;
}

bool DriveAccessToken(const DriveConfig& config, std::string* token, std::string* reason) {
  std::lock_guard<std::mutex> lock(g_drive_session_mutex);
  const long long now = static_cast<long long>(std::time(nullptr));
  if (!g_drive_access_token.empty() &&
      now + kDriveAccessTokenSkewSeconds < g_drive_access_expires_at) {
    *token = g_drive_access_token;
    return true;
  }

  DriveState state;
  if (!LoadDriveState(config.state_path, &state)) {
    const long long tick = static_cast<long long>(GetTickCount64());
    const long long failed_at = g_drive_consent_failed_at.load();
    if (failed_at != 0 && tick - failed_at >= 0 &&
        tick - failed_at < static_cast<long long>(kDriveConsentCooldownMs)) {
      *reason = "drive-not-authorized";
      return false;
    }
    if (!RunDriveConsent(config, &state, reason)) {
      g_drive_consent_failed_at.store(static_cast<long long>(GetTickCount64()));
      return false;
    }
    g_drive_consent_failed_at.store(0);
  }

  std::vector<std::pair<std::string, std::string>> fields;
  fields.emplace_back("client_id", config.client_id);
  fields.emplace_back("client_secret", config.client_secret);
  fields.emplace_back("refresh_token", state.refresh_token);
  fields.emplace_back("grant_type", "refresh_token");
  DriveTokenGrant grant;
  if (!PostDriveToken(fields, &grant, reason)) {
    EmitLog("local-cover drive auth=refresh-failed reason=" + *reason);
    if (*reason == "token-http-400" || *reason == "token-http-401") {
      const std::wstring wide = Utils::ToWString(config.state_path);
      if (!wide.empty()) {
        DeleteFileW(wide.c_str());
      }
      g_drive_consent_failed_at.store(static_cast<long long>(GetTickCount64()));
      *reason = "drive-reauth-required";
    }
    return false;
  }
  g_drive_access_token = grant.access_token;
  const long long lifetime = grant.expires_in > 0 ? grant.expires_in : 3600;
  g_drive_access_expires_at = now + lifetime;
  EmitLog("local-cover drive auth=refreshed");
  *token = grant.access_token;
  return true;
}

bool SaveDriveFolderId(const DriveConfig& config, const std::string& folder_id) {
  DriveState state;
  if (!LoadDriveState(config.state_path, &state)) {
    return false;  // never write a state file without a token
  }
  if (state.folder_id == folder_id) {
    return true;
  }
  state.folder_id = folder_id;
  return SaveDriveState(config.state_path, state);
}

bool EnsureDriveFolder(const DriveConfig& config, const std::string& access_token,
                       std::string* folder_id, std::string* reason) {
  {
    std::lock_guard<std::mutex> lock(g_drive_session_mutex);
    if (LooksLikeDriveFileId(g_drive_folder_id)) {
      *folder_id = g_drive_folder_id;
      return true;
    }
  }
  DriveState state;
  if (LoadDriveState(config.state_path, &state) && LooksLikeDriveFileId(state.folder_id)) {
    std::lock_guard<std::mutex> lock(g_drive_session_mutex);
    g_drive_folder_id = state.folder_id;
    *folder_id = state.folder_id;
    return true;
  }

  const std::wstring headers =
      L"Authorization: Bearer " + Utils::ToWString(access_token) + L"\r\n";
  const HttpResponse list =
      SendHttp(L"GET", L"www.googleapis.com", Utils::ToWString(BuildDriveFolderQuery()), headers,
               std::string());
  if (!list.transport_ok) {
    *reason = list.transport_reason.empty() ? std::string("folder-transport-failed")
                                            : "folder-" + list.transport_reason;
    return false;
  }
  if (list.status != 200) {
    *reason = "folder-http-" + std::to_string(list.status);
    return false;
  }

  std::string id = FirstDriveFileId(list.body);
  if (id.empty()) {
    const HttpResponse created =
        SendHttp(L"POST", L"www.googleapis.com", L"/drive/v3/files?fields=id",
                 headers + L"Content-Type: application/json\r\n", BuildDriveFolderMetadata());
    if (!created.transport_ok) {
      *reason = created.transport_reason.empty() ? std::string("folder-create-transport-failed")
                                                 : "folder-create-" + created.transport_reason;
      return false;
    }
    if (created.status != 200 && created.status != 201) {
      *reason = "folder-create-http-" + std::to_string(created.status);
      return false;
    }
    id = ExtractJsonStringField(created.body, "id");
    if (!LooksLikeDriveFileId(id)) {
      *reason = "folder-create-unexpected-body";
      return false;
    }
  }

  const std::wstring permission_path =
      L"/drive/v3/files/" + Utils::ToWString(id) + L"/permissions";
  const HttpResponse permission =
      SendHttp(L"POST", L"www.googleapis.com", permission_path,
               headers + L"Content-Type: application/json\r\n", BuildDrivePermissionMetadata());
  if (!permission.transport_ok) {
    *reason = permission.transport_reason.empty() ? std::string("permission-transport-failed")
                                                  : "permission-" + permission.transport_reason;
    return false;
  }
  const DrivePermissionVerdict verdict =
      ClassifyDrivePermission(permission.status, permission.body);
  if (verdict == DrivePermissionVerdict::kDenied) {
    EmitLog("local-cover drive folder permission=denied status=" +
            std::to_string(permission.status));
    *reason = "permission-denied";
    return false;
  }
  if (verdict == DrivePermissionVerdict::kRejected) {
    EmitLog("local-cover drive folder permission=rejected status=" +
            std::to_string(permission.status));
    *reason = permission.status == 400 ? std::string("permission-bad-request")
                                       : "permission-http-" + std::to_string(permission.status);
    return false;
  }
  if (verdict == DrivePermissionVerdict::kAlreadyShared) {
    EmitLog("local-cover drive folder permission=already status=" +
            std::to_string(permission.status));
  }

  SaveDriveFolderId(config, id);  // best effort: the memory cache survives the session
  {
    std::lock_guard<std::mutex> lock(g_drive_session_mutex);
    g_drive_folder_id = id;
  }
  *folder_id = id;
  return true;
}

UploadAttempt UploadDriveImage(const std::string& name, const std::string& mime,
                               const std::vector<unsigned char>& image) {
  const DriveConfig config = SnapshotDriveConfig();
  if (!DriveConfigured(config)) {
    return AttemptResult(Failure("drive-not-configured"));
  }
  std::string access_token;
  std::string reason;
  if (!DriveAccessToken(config, &access_token, &reason)) {
    return AttemptResult(Failure(reason.empty() ? std::string("drive-auth-failed") : reason));
  }
  std::string folder_id;
  if (!EnsureDriveFolder(config, access_token, &folder_id, &reason)) {
    return AttemptResult(Failure(reason));
  }

  const std::string boundary = MakeBoundary();
  const std::string body =
      BuildDriveMultipartBody(boundary, name, folder_id, SanitizeMime(mime.c_str()), image);
  if (body.size() > 0xffffffffull) {
    return AttemptResult(Failure("body-too-large"));
  }
  const std::wstring headers =
      L"Authorization: Bearer " + Utils::ToWString(access_token) + L"\r\n" +
      L"Content-Type: " + Utils::ToWString(DriveMultipartContentType(boundary)) + L"\r\n";
  const HttpResponse response =
      SendHttp(L"POST", L"www.googleapis.com",
               L"/upload/drive/v3/files?uploadType=multipart&fields=id", headers, body);
  if (!response.transport_ok) {
    UploadAttempt attempt;
    attempt.result = Failure(response.transport_reason.empty()
                                 ? std::string("upload-transport-failed")
                                 : "upload-" + response.transport_reason);
    attempt.transport_failure = true;
    return attempt;
  }
  if (response.status == 0) {
    return AttemptResult(Failure("upload-" + response.transport_reason));
  }
  return AttemptResult(ClassifyDriveUpload(response.status, response.body));
}

UploadAttempt AttemptHostUpload(const UploadHost& upload_host, const PreparedImage& prepared) {
  if (upload_host.shape == UploadShape::kDrive) {
    return UploadDriveImage(UploadCacheKey(prepared), prepared.mime, prepared.bytes);
  }
  const std::string content_type = SanitizeMime(prepared.mime.c_str());
  const std::string boundary = MakeBoundary();
  const std::string body = BuildMultipartBody(boundary, content_type, prepared.bytes, upload_host.shape);
  return UploadMultipartBody(upload_host, body, boundary);
}

bool CrackUrl(const std::string& url, std::wstring* host, std::wstring* path) {
  const std::wstring wide = Utils::ToWString(url);
  if (wide.empty()) {
    return false;
  }
  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components) == FALSE) {
    return false;
  }
  if (components.nScheme != INTERNET_SCHEME_HTTPS || components.lpszHostName == nullptr ||
      components.dwHostNameLength == 0) {
    return false;
  }
  *host = std::wstring(components.lpszHostName, components.dwHostNameLength);
  std::wstring whole_path;
  if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0) {
    whole_path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
    whole_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (whole_path.empty()) {
    whole_path = L"/";
  }
  *path = whole_path;
  return true;
}

struct HealthCheck {
  bool ok = false;
  std::string reason;
};

HealthCheck HealthCheckUrl(const std::string& url) {
  HealthCheck check;
  check.reason = "health-check-failed";
  const Deadline deadline(kTotalTimeoutMs);
  std::wstring host;
  std::wstring path;
  if (!CrackUrl(url, &host, &path)) {
    check.reason = "health-check-crack-url-failed";
    return check;
  }
  WinHttpHandle session = OpenSession(deadline);
  if (!session) {
    check.reason = TransportReason("health-check-session-open-failed", "connect", GetLastError());
    return check;
  }
  WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) {
    check.reason = TransportReason("health-check-connect-failed", "connect", GetLastError());
    return check;
  }
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    check.reason = TransportReason("health-check-request-open-failed", "send", GetLastError());
    return check;
  }
  if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
    check.reason = TransportReason("health-check-send-failed", "send", GetLastError());
    return check;
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    check.reason = TransportReason("health-check-receive-failed", "receive", GetLastError());
    return check;
  }
  DWORD status = 0;
  DWORD status_error = 0;
  if (!QueryStatusCode(request.get(), &status, &status_error)) {
    check.reason = "health-check-status-query-failed phase=receive winhttp=" + std::to_string(status_error);
    return check;
  }
  if (status != 200) {
    check.reason = "health-check-status=" + std::to_string(status);
    return check;
  }
  std::wstring content_type;
  if (!QueryContentType(request.get(), &content_type)) {
    check.reason = "health-check-content-type-missing";
    return check;
  }
  if (!StartsWithImageType(content_type)) {
    check.reason = "health-check-content-type=" + Utils::ToString(content_type);
    return check;
  }
  check.ok = true;
  check.reason.clear();
  return check;
}

struct HostOutcome {
  Result result;
  int attempts = 0;
};

HostOutcome TryUploadHost(const UploadHost& upload_host, const PreparedImage& prepared) {
  HostOutcome outcome;
  UploadAttempt attempt = AttemptHostUpload(upload_host, prepared);
  outcome.attempts = 1;
  if (attempt.transport_failure) {
    Sleep(kRetryBackoffMs);
    attempt = AttemptHostUpload(upload_host, prepared);
    outcome.attempts = 2;
  }
  outcome.result = attempt.result;
  if (outcome.result.ok) {
    const HealthCheck health = HealthCheckUrl(outcome.result.url);
    if (!health.ok) {
      outcome.result = Failure(health.reason);
    }
  }
  outcome.result.host = HostNameUtf8(upload_host);
  outcome.result.attempts = outcome.attempts;
  return outcome;
}

std::string ExceptionText(const char* what) {
  if (what == nullptr) {
    return "unknown";
  }
  std::string text;
  for (const char* c = what; *c != '\0' && text.size() < 120; ++c) {
    text.push_back((*c == '\r' || *c == '\n' || *c == '\t') ? ' ' : *c);
  }
  return text.empty() ? std::string("unknown") : text;
}

HostOutcome TryUploadHostGuarded(const UploadHost& upload_host, const PreparedImage& prepared) {
  try {
    return TryUploadHost(upload_host, prepared);
  } catch (const std::exception& error) {
    EmitLog("local-cover host=" + HostNameUtf8(upload_host) + " exception=" +
            ExceptionText(error.what()));
  } catch (...) {
    EmitLog("local-cover host=" + HostNameUtf8(upload_host) + " exception=unknown");
  }
  HostOutcome outcome;
  outcome.result = Failure("exception");
  outcome.result.host = HostNameUtf8(upload_host);
  outcome.attempts = 1;
  return outcome;
}

HostOutcome TryDriveUpload(const PreparedImage& prepared) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (hosts[i].shape == UploadShape::kDrive) {
      return TryUploadHostGuarded(hosts[i], prepared);
    }
  }
  HostOutcome outcome;
  outcome.result = Failure("drive-not-configured");
  return outcome;
}

std::vector<PermanentHost> CurrentUpgradeHosts() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return PlanCacheUpgrade(g_cover_host, DriveConfiguredLocked());
}

std::string CurrentPolicyName() {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  return PolicyName(g_cover_host);
}

HostOutcome UpgradeCachedUrl(const PreparedImage& prepared) {
  const std::vector<PermanentHost> order = CurrentUpgradeHosts();
  HostOutcome outcome;
  outcome.result = Failure("no-permanent-host");
  for (size_t i = 0; i < order.size(); ++i) {
    outcome = TryDriveUpload(prepared);
    if (outcome.result.ok) {
      return outcome;
    }
    EmitLog("local-cover upgrade-host-failed host=" + outcome.result.host + " reason=" +
            outcome.result.reason);
  }
  return outcome;
}

std::string HostListToken() {
  const std::vector<UploadHost> hosts = ActiveHosts();
  std::string token;
  for (size_t i = 0; i < hosts.size(); ++i) {
    if (i != 0) {
      token += ",";
    }
    token += Utils::ToString(hosts[i].name);
  }
  return token;
}

void (*g_logger)(const std::string&) = nullptr;

void EmitLog(const std::string& line) {
  if (g_logger != nullptr) {
    g_logger(line);
  }
}

void LogDriveHostState() {
  EmitLog(std::string("local-cover drive host=") + (DriveUploadActive() ? "active" : "inactive"));
}

void LogHostSuccess(const Result& result) {
  EmitLog("local-cover published host=" + result.host + " url=" + result.url);
}

void LogHostFailure(const std::string& reason, const std::string& host_list, int attempts,
                    const PreparedImage& prepared) {
  EmitLog("local-cover failed host=" + host_list + " reason=" +
          (reason.empty() ? std::string("unknown") : reason) + " attempts=" +
          std::to_string(attempts) + " detail=" + prepared.note);
}

bool AnyHostHealthy(const bool* reachable, size_t count) {
  if (reachable == nullptr || count == 0) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (reachable[i]) {
      return true;
    }
  }
  return false;
}

size_t ConfiguredHostCount() { return ActiveHosts().size(); }

std::string ConfiguredHostName(size_t index) {
  const std::vector<UploadHost> hosts = ActiveHosts();
  return index < hosts.size() ? Utils::ToString(hosts[index].name) : std::string();
}

Result PublishException(const std::vector<unsigned char>& image_bytes, const char* what) {
  Result result = Failure("exception");
  try {
    EmitLog("local-cover failed reason=exception text=" + ExceptionText(what));
    const std::string key = Sha256Hex(image_bytes);
    CacheStore(key, result, static_cast<long long>(std::time(nullptr)));
    result.detail = "key=" + key + " attempts=0 host=exception";
  } catch (...) {
  }
  return result;
}

}  // namespace


namespace CoverPublisher {

void SetLogger(void (*logger)(const std::string& line)) {
  g_logger = logger;
}

void Configure(const std::string& cache_path) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache_path = cache_path;
  g_cache.clear();
  if (!cache_path.empty()) {
    LoadCacheLocked();
  }
}

void ConfigureAuth(const std::string& /*token*/, const std::string& /*repo*/) {
}

void ConfigureHost(const std::string& value) {
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_cover_host = ParseCoverHostPolicy(value);
}

void LogHostConfiguration() {
  HostConfigSummary summary;
  {
    std::lock_guard<std::mutex> lock(g_host_mutex);
    summary.policy = g_cover_host;
    summary.drive_usable = DriveConfiguredLocked();
    summary.drive_reason = DriveUnavailableReason(
        !g_drive_client_id.empty(), !g_drive_client_secret.empty(), !g_drive_state_path.empty());
  }
  EmitLog(FormatHostConfigLine(summary));
}

void ConfigureDrive(const std::string& client_id, const std::string& client_secret,
                    const std::string& state_path) {
  g_drive_consent_cancel.store(false);
  {
    std::lock_guard<std::mutex> session(g_drive_session_mutex);
    g_drive_access_token.clear();
    g_drive_access_expires_at = 0;
    g_drive_folder_id.clear();
  }
  g_drive_consent_failed_at.store(0);
  std::lock_guard<std::mutex> lock(g_host_mutex);
  g_drive_client_id = client_id;
  g_drive_client_secret = client_secret;
  g_drive_state_path = state_path;
}

void CancelDriveConsent() { g_drive_consent_cancel.store(true); }

Result Publish(const std::vector<unsigned char>& image_bytes, const char* mime) {
  try {
    if (image_bytes.empty()) {
      return Failure("empty-image");
    }
    const PreparedImage prepared = PrepareImageForUpload(image_bytes, mime);
    const std::string key = UploadCacheKey(prepared);
    const long long now = static_cast<long long>(std::time(nullptr));
    Result cached;
    if (CacheLookup(key, now, &cached)) {
      if (!cached.ok) {
        cached.detail = prepared.note + " key=" + key + " cache=hit";
        return cached;
      }
      const std::vector<PermanentHost> upgrade_hosts = CurrentUpgradeHosts();
      const CachedUrlAction action = PlanCachedUrl(cached.url, !upgrade_hosts.empty());
      if (action == CachedUrlAction::kUse) {
        cached.detail = prepared.note + " key=" + key + " cache=hit";
        return cached;
      }
      if (action == CachedUrlAction::kUpgradeToPermanent) {
        const HostOutcome upgrade = UpgradeCachedUrl(prepared);
        if (upgrade.result.ok) {
          Result result = upgrade.result;
          LogHostSuccess(result);
          result.detail = prepared.note + " key=" + key +
                          " cache=upgraded-from-ephemeral attempts=" +
                          std::to_string(upgrade.attempts) + " host=" + result.host;
          CacheStore(key, result, now);
          return result;
        }
        EmitLog("local-cover upgrade-failed key=" + key + " reason=" +
                upgrade.result.reason + " host=" + upgrade.result.host);
      } else {
        EmitLog("local-cover cache-upgrade-skipped key=" + key +
                " reason=no-permanent-host policy=" + CurrentPolicyName());
      }
      const HealthCheck health = HealthCheckUrl(cached.url);
      if (health.ok) {
        cached.detail = prepared.note + " key=" + key + " cache=hit-revalidated";
        return cached;
      }
      EmitLog("local-cover cache-stale key=" + key + " reason=" + health.reason);
    }

    LogDriveHostState();
    const std::vector<UploadHost> hosts = ActiveHosts();
    Result result;
    int total_attempts = 0;
    for (size_t i = 0; i < hosts.size(); ++i) {
      const HostOutcome outcome = TryUploadHostGuarded(hosts[i], prepared);
      total_attempts += outcome.attempts;
      if (!outcome.result.ok && hosts[i].shape == UploadShape::kDrive) {
        EmitLog("local-cover drive failed reason=" +
                (outcome.result.reason.empty() ? std::string("unknown") : outcome.result.reason));
      }
      if (outcome.result.ok) {
        result = outcome.result;
        break;
      }
      result = outcome.result;  // last failure names the final host
    }
    if (result.ok) {
      LogHostSuccess(result);
    } else {
      LogHostFailure(result.reason, HostListToken(), total_attempts, prepared);
    }
    result.detail = prepared.note + " key=" + key + " attempts=" + std::to_string(total_attempts) +
                    " host=" + (result.host.empty() ? HostListToken() : result.host);
    CacheStore(key, result, now);
    return result;
  } catch (const std::exception& error) {
    return PublishException(image_bytes, error.what());
  } catch (...) {
    return PublishException(image_bytes, "unknown");
  }
}

void ClearCache() {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache.clear();
  if (!g_cache_path.empty()) {
    const std::wstring wide = Utils::ToWString(g_cache_path);
    if (!wide.empty()) {
      DeleteFileW(wide.c_str());
    }
  }
}

}  // namespace CoverPublisher
