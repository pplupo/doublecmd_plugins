#pragma once

#include <string>

/// Member-name handling shared by the libarchive walk and the ZIP central
/// directory parser.
///
/// This is not here for the sake of deduplication — it is here because the
/// two readers must agree *exactly*. The parser's per-entry packed size and
/// CRC are joined onto the scanner's entries by path, so any divergence in
/// how a name is decoded or normalised silently drops the extra columns
/// instead of failing loudly.
namespace archiveview::names {

/// True when `bytes` is well-formed UTF-8. Hand-rolled rather than pulled
/// from a toolkit, because this header is shared by the Qt and GTK builds and
/// must not depend on either.
bool isValidUtf8(const std::string &bytes);

/// Decode a member name to UTF-8 without assuming it already is UTF-8.
///
/// Treating raw bytes as UTF-8 mangles every archive whose names are in a
/// legacy codepage. libarchive's *_utf8 accessors convert properly but return
/// NULL when the name cannot be represented, so both paths are needed.
///
/// Order: declared UTF-8, then a UTF-8 validity check, then the locale's
/// codeset, then the configured fallback, then Latin-1. Latin-1 is last
/// because it round-trips losslessly — the bytes survive even when the glyphs
/// are wrong.
///
/// Deliberately no charset *detection*: it is unreliable on a short filename,
/// and 100k detections would cost more than the entire archive walk.
std::string decode(const std::string &raw, bool declaredUtf8 = false);

/// Normalise a member path for tree building: strip the "./" prefix tar
/// archives carry, collapse runs of separators so "a//b.txt" does not produce
/// empty-named tree nodes, and drop any trailing slash so "a/b/" and "a/b"
/// land on the same node.
///
/// Note what this deliberately does NOT do: it does not resolve "..", and it
/// does not strip a leading "/". A member named "../../../../etc/passwd" is
/// shown exactly as stored. Sanitising it here would hide the single most
/// useful thing this viewer can tell you about a hostile archive — and the
/// extraction path refuses such names anyway, which is where it matters.
std::string normalize(std::string path);

/// Codec applied to names that are neither valid UTF-8 nor decodable in the
/// current locale, from the NameCodec setting (e.g. "windows-1251").
///
/// A single process-wide value rather than a parameter threaded through every
/// call: both readers must agree on it for the central-directory join to line
/// up, and a viewer only ever has one archive open at a time. Set before a
/// scan starts; read from the worker thread afterwards.
void setFallbackCodec(const std::string &codec);
const std::string &fallbackCodec();

} // namespace archiveview::names
