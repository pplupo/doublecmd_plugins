// Toolkit-agnostic "should we claim this file, and how is it encoded?"
// pass, deliberately kept free of Qt so a future GTK3/WebKitGTK backend
// can share it verbatim (same core/qt6/gtk3 split markdownview uses).
//
// The whole point of this file is that ListLoad gets to say "no". A WLX
// that always returns a valid handle makes Double Commander's fallback to
// the next lister -- or to its own internal viewer -- unreachable, so a
// binary named .html shows an empty pane forever. Everything here exists
// to produce an honest verdict before a window is ever created.

#pragma once

#include <cstdint>
#include <string>

namespace HtmlProbe {

enum class Verdict {
	Renderable,  // parses as HTML (or MHTML) -- hand it to the engine
	NotHtml,     // binary, or no markup at all -- let DC fall through
	Unreadable,  // stat/open failed
	TooLarge,    // past the configured cap; DC's internal viewer handles
	             // huge files without blocking a UI thread, we would not
};

// How the document should reach the engine. Rendering a file:// URL
// directly is what a browser does and is always correct, but it hands
// charset guessing to Chromium -- whose answer for an undeclared legacy
// page is "UTF-8", i.e. mojibake. Inline lets us decode with the encoding
// resolved below and inject our own <meta charset>, at the cost of a size
// ceiling (QWebEnginePage::setHtml tops out around 2MB).
enum class Delivery {
	InlineDecoded,  // read + transcode + setHtml() with a file:// base URL
	DirectFileUrl,  // load() the file:// URL and let the engine do it
};

struct Result {
	Verdict verdict = Verdict::Unreadable;
	Delivery delivery = Delivery::DirectFileUrl;
	// IANA name, meaningful only when verdict == Renderable.
	std::string encoding;
	// False means we inferred the encoding rather than read it off the
	// document -- the case where handing the file straight to the engine
	// would produce a different (usually wrong) answer than we did.
	bool encodingDeclared = false;
	uint64_t fileSize = 0;
	// Short human-readable reason, for the stderr note on rejection.
	std::string detail;
};

// `fallbackEncoding` is used only when the document declares nothing and
// the bytes are not valid UTF-8 -- there is no universally right answer
// there, so it is a setting rather than a hardcoded guess.
Result probeFile(const std::string &path, uint64_t maxBytes,
                 const std::string &fallbackEncoding);

// iconv-backed transcode. Returns false (leaving `out` untouched) if the
// encoding is unknown to iconv; callers fall back to DirectFileUrl.
bool decodeToUtf8(const std::string &bytes, const std::string &encoding,
                  std::string &out);

}  // namespace HtmlProbe
