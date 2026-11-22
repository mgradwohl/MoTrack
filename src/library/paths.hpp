#pragma once

#include <optional>
#include <string>
#include <string_view>

/// Path and URL rules that OSP's Digital Library classes use for collection targets, HTML pages and base paths.
///
/// Each function reproduces the named `org.opensourcephysics.controls.XML` or `tools.ResourceLoader` method of
/// OSP d56207b (Tracker 6.3.5); the swingJS tip (d49c6ec) is unchanged for all of them. Paths are text: nothing
/// here touches the file system, percent-decodes or validates a URL, so the results are identical on every
/// platform. docs/library-collections.md lists the quirks with examples.
namespace motrack::library
{

/// XML.forwardSlash: replaces every backslash with a slash.
[[nodiscard]] std::string forwardSlash(std::string_view path);

/// XML.getName: the text after the last '/', or after the last '\' when there is no '/'.
[[nodiscard]] std::string_view fileName(std::string_view path) noexcept;

/// XML.getExtension: the text after the last '.', when that dot is neither the first nor the last character and
/// follows the last (forward-slashed) separator. Query text is part of the extension
/// ("ServeFile.cfm?ID=1" has extension "cfm?ID=1").
[[nodiscard]] std::optional<std::string_view> extension(std::string_view path) noexcept;

/// XML.stripExtension: forward-slashes the path, removes the file name's extension and then every trailing dot.
[[nodiscard]] std::string stripExtension(std::string_view path);

/// XML.getDirectoryPath: the forward-slashed text before the last '/', or "" when there is none.
[[nodiscard]] std::string directoryPath(std::string_view path);

/// XML.getResolvedPath(relative, base), the rule behind every absolute target and HTML path:
/// - one trailing '/' is removed from the base, and both paths are forward-slashed;
/// - a path that starts with '/' or contains ":/" anywhere is absolute and returned unchanged (URLs,
///   "C:\dir" after forward-slashing, "//server/share", "jar:file:/...");
/// - each leading "../" removes the base's last segment (a base without '/' is treated as "/base", so it
///   becomes empty), then one leading "./" is removed and "." becomes "";
/// - an empty base returns the remaining path; a base ending in '/' or a path starting with '&' (ComPADRE
///   query parameters) is concatenated directly; otherwise they are joined with '/'.
/// Spaces, '%', non-ASCII text and archive members ("a.trz!/b.html") are ordinary characters.
[[nodiscard]] std::string resolvedPath(std::string_view relative, std::string_view base);

/// The inverse that authoring needs: `absolute` relative to the directory `base`, climbing at most five levels
/// with "../", as XML.getPathRelativeTo does. Paths that are not absolute (no leading '/' and no ':') are
/// returned forward-slashed; so are paths with no common ancestor within five levels.
///
/// Deliberate difference: OSP matches the base as a plain text prefix, so "C:/lib" makes "C:/library/a.html"
/// "rary/a.html", which resolves to a missing file. MoTrack only matches whole path segments. OSP's
/// "jar!" shortcut is also omitted (it applies to class-path resources, not collections).
[[nodiscard]] std::string relativePath(std::string_view absolute, std::string_view base);

/// Where a path points, for the browser to decide what a collection may open. Classified after backslashes become
/// slashes.
enum class PathKind : unsigned char
{
    Empty,
    Relative,      ///< Resolved against a base path.
    Web,           ///< http: or https: (any case).
    FileUrl,       ///< file:
    ArchiveUrl,    ///< jar:
    OtherScheme,   ///< Any other "scheme:" such as mailto: or ftp:.
    LocalAbsolute, ///< "/path", or a drive "C:..." (also the drive-relative "C:x").
    NetworkShare,  ///< "//server/share": a UNC path, which Windows opens over SMB.
    ComPadreQuery, ///< "&name=value" parameters appended to a ComPADRE query.
};

[[nodiscard]] PathKind classifyPath(std::string_view path);

/// The result of `resolvedPath` with its provenance.
struct PathResolution
{
    std::string path;
    PathKind kind = PathKind::Empty;
    /// The result lies outside the base directory (a "../" step, or an absolute path). Common in real collections.
    bool leftBase = false;
    /// The result has another origin than the base: another scheme, host, drive or share, or none at all (a
    /// "../" chain that climbed past a URL's host leaves a relative path). A document from the web must not reach
    /// local files or shares this way.
    bool changedOrigin = false;
    bool archiveMember = false; ///< It names a member of an archive ("!/").
};

/// `resolvedPath` with the classification the Library Browser needs (slice I6): it opens local, UNC, `file:` and
/// `jar:` results of a remote collection only after confirmation, refuses origin changes it cannot explain, and
/// percent-encodes the values of ComPADRE parameters before appending them.
[[nodiscard]] PathResolution resolvePathChecked(std::string_view relative, std::string_view base);

/// ResourceLoader.isHTTP: starts with "http:" or "https:" (case-sensitive).
[[nodiscard]] bool isHttp(std::string_view path) noexcept;

/// ResourceLoader.isJarZipTrz(path, false): ends with ".jar", ".zip" or ".trz" (case-sensitive).
[[nodiscard]] bool isJarZipTrz(std::string_view path) noexcept;

/// An archive member path: Tracker names a file inside a ZIP/TRZ as "archive!/member".
struct ArchiveMember
{
    std::string_view archive; ///< Text before the first "!/".
    std::string_view member;  ///< Text after it.
};

/// Splits at the first "!/" (ResourceLoader.isJarZipTrz(path, true) tests for it).
[[nodiscard]] std::optional<ArchiveMember> splitArchiveMember(std::string_view path) noexcept;

/// ResourceLoader.getNonURIPath: removes a leading "jar:" and then "file:", removes the leading '/' of
/// "/C:/..."-style paths, then replaces "%20" with a space and '&' and '?' with '_'.
[[nodiscard]] std::string nonUriPath(std::string_view uriPath);

enum class UriPlatform : unsigned char
{
    Windows, ///< Relative paths get "file:/".
    Posix,   ///< Relative paths get "file://".
};

/// ResourceLoader.getURIPath: trims and forward-slashes, appends '/' when the path has no extension, encodes
/// spaces as "%20" for http(s) only, and adds "file:" to absolute and a platform prefix to other local paths.
[[nodiscard]] std::string uriPath(std::string_view path, UriPlatform platform);

} // namespace motrack::library
