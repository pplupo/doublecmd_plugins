#include "html_probe.h"

#include <errno.h>
#include <iconv.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace HtmlProbe {

namespace {

// The window we scan for a charset declaration and for markup. The HTML
// spec's own prescan gives up after 1024 bytes, but real files routinely
// carry a licence comment or a hand-written banner ahead of <head>, and
// reading 64KB costs nothing next to spawning a renderer.
constexpr size_t kHeaderBytes = 64 * 1024;

// setHtml()'s documented ceiling is 2MB; stay under it with room for the
// UTF-8 expansion of a legacy single-byte document (worst case 2x) plus
// the injected prelude.
constexpr uint64_t kInlineCeiling = 900 * 1024;

std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)tolower(c); });
	return s;
}

bool containsAt(const std::string &hay, const char *needle, size_t from) {
	return hay.compare(from, strlen(needle), needle) == 0;
}

// Reads the charset out of `<?xml version="1.0" encoding="..."?>`,
// `<meta charset=...>` or `<meta http-equiv=... content="...; charset=...">`.
// Deliberately a scan rather than a parse: we only need the token, and a
// real tokenizer here would be a second HTML parser in a plugin that
// already links a complete one.
std::string declaredCharset(const std::string &lowerHeader) {
	auto valueAfter = [&](size_t pos) -> std::string {
		// Skip `=` and any whitespace/quote, then take the token.
		while (pos < lowerHeader.size() &&
		       (lowerHeader[pos] == ' ' || lowerHeader[pos] == '=' ||
		        lowerHeader[pos] == '\t' || lowerHeader[pos] == '"' ||
		        lowerHeader[pos] == '\''))
			pos++;
		size_t end = pos;
		while (end < lowerHeader.size() &&
		       (isalnum((unsigned char)lowerHeader[end]) ||
		        lowerHeader[end] == '-' || lowerHeader[end] == '_'))
			end++;
		return lowerHeader.substr(pos, end - pos);
	};

	size_t xmlDecl = lowerHeader.find("<?xml");
	if (xmlDecl != std::string::npos && xmlDecl < 8) {
		size_t enc = lowerHeader.find("encoding", xmlDecl);
		if (enc != std::string::npos && enc < lowerHeader.find("?>", xmlDecl))
			return valueAfter(enc + 8);
	}

	// Both <meta charset="x"> and <meta http-equiv content="...charset=x">
	// end up as the substring "charset" followed by the value, so one scan
	// covers both forms.
	size_t pos = 0;
	while ((pos = lowerHeader.find("charset", pos)) != std::string::npos) {
		std::string value = valueAfter(pos + 7);
		if (!value.empty())
			return value;
		pos += 7;
	}
	return std::string();
}

bool isValidUtf8(const std::string &bytes) {
	size_t i = 0;
	while (i < bytes.size()) {
		unsigned char c = (unsigned char)bytes[i];
		size_t extra;
		if (c < 0x80)
			extra = 0;
		else if ((c & 0xE0) == 0xC0)
			extra = 1;
		else if ((c & 0xF0) == 0xE0)
			extra = 2;
		else if ((c & 0xF8) == 0xF0)
			extra = 3;
		else
			return false;
		// A truncated sequence at the very end of the *header* is an
		// artefact of where we cut the read, not a decoding error.
		if (i + extra >= bytes.size())
			return i + extra == bytes.size() ? true : false;
		for (size_t k = 1; k <= extra; k++)
			if (((unsigned char)bytes[i + k] & 0xC0) != 0x80)
				return false;
		i += extra + 1;
	}
	return true;
}

// Any of these means someone wrote markup. Checked against a lowercased
// copy so the scan is case-insensitive without a per-character compare.
bool looksLikeMarkup(const std::string &lowerHeader) {
	static const char *kTags[] = {
	    "<!doctype", "<html", "<head", "<body",  "<meta",  "<title",
	    "<div",      "<p>",   "<p ",   "<table", "<span",  "<script",
	    "<link",     "<a ",   "<h1",   "<img",   "<style", "<ul",
	};
	for (const char *tag : kTags)
		if (lowerHeader.find(tag) != std::string::npos)
			return true;
	return false;
}

// .mht/.mhtml are RFC 2557 MIME containers, not HTML -- the markup scan
// above would reject them even though QWebEngineView renders them
// natively. They also must not go down the inline path: the engine needs
// the container, not a transcoded copy of it.
bool looksLikeMhtml(const std::string &lowerHeader) {
	return lowerHeader.find("mime-version:") != std::string::npos &&
	       lowerHeader.find("multipart/related") != std::string::npos;
}

}  // namespace

bool decodeToUtf8(const std::string &bytes, const std::string &encoding,
                  std::string &out) {
	// //TRANSLIT keeps a single unmappable byte from aborting the whole
	// document -- a stray 0x81 in a cp1252 page should cost one character,
	// not the entire render.
	iconv_t cd = iconv_open("UTF-8//TRANSLIT", encoding.c_str());
	if (cd == (iconv_t)-1)
		return false;

	std::string result;
	result.reserve(bytes.size() + bytes.size() / 2);

	char *inPtr = const_cast<char *>(bytes.data());
	size_t inLeft = bytes.size();
	std::vector<char> buffer(64 * 1024);

	while (inLeft > 0) {
		char *outPtr = buffer.data();
		size_t outLeft = buffer.size();
		size_t rc = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
		result.append(buffer.data(), buffer.size() - outLeft);
		if (rc == (size_t)-1) {
			if (errno == E2BIG)
				continue;  // buffer full, drain and keep going
			// EILSEQ/EINVAL: skip the offending byte and carry on, for
			// the same reason //TRANSLIT is set above.
			if (inLeft == 0)
				break;
			inPtr++;
			inLeft--;
			result += '?';
		}
	}

	iconv_close(cd);
	out.swap(result);
	return true;
}

Result probeFile(const std::string &path, uint64_t maxBytes,
                 const std::string &fallbackEncoding) {
	Result result;

	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		result.verdict = Verdict::Unreadable;
		result.detail = std::string("stat failed: ") + strerror(errno);
		return result;
	}
	if (!S_ISREG(st.st_mode)) {
		result.verdict = Verdict::NotHtml;
		result.detail = "not a regular file";
		return result;
	}
	result.fileSize = (uint64_t)st.st_size;
	if (result.fileSize > maxBytes) {
		result.verdict = Verdict::TooLarge;
		result.detail = "larger than the configured cap";
		return result;
	}

	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		result.verdict = Verdict::Unreadable;
		result.detail = std::string("open failed: ") + strerror(errno);
		return result;
	}
	std::string header(kHeaderBytes, '\0');
	size_t got = fread(&header[0], 1, kHeaderBytes, f);
	fclose(f);
	header.resize(got);

	if (got == 0) {
		result.verdict = Verdict::NotHtml;
		result.detail = "empty file";
		return result;
	}

	// --- BOM, which outranks every in-document declaration -------------
	bool utf16 = false;
	if (containsAt(header, "\xEF\xBB\xBF", 0)) {
		result.encoding = "UTF-8";
		result.encodingDeclared = true;
	} else if (got >= 2 && (unsigned char)header[0] == 0xFF &&
	           (unsigned char)header[1] == 0xFE) {
		result.encoding = "UTF-16LE";
		result.encodingDeclared = true;
		utf16 = true;
	} else if (got >= 2 && (unsigned char)header[0] == 0xFE &&
	           (unsigned char)header[1] == 0xFF) {
		result.encoding = "UTF-16BE";
		result.encodingDeclared = true;
		utf16 = true;
	}

	// The markup and charset scans below both need single-byte text, so a
	// UTF-16 document gets transcoded first. Order matters: the NUL-byte
	// binary check further down would reject every UTF-16 file outright.
	std::string scanText = header;
	if (utf16) {
		std::string decoded;
		if (decodeToUtf8(header, result.encoding, decoded))
			scanText = decoded;
	} else {
		// A NUL in the first block means this is not text. This is the
		// check that keeps a .html-named binary out of the renderer and
		// lets DC fall through to its own viewer.
		size_t scan = std::min<size_t>(got, 4096);
		if (memchr(header.data(), '\0', scan) != nullptr) {
			result.verdict = Verdict::NotHtml;
			result.detail = "NUL byte in first 4KB -- binary content";
			return result;
		}
	}

	const std::string lower = toLower(scanText);

	if (looksLikeMhtml(lower)) {
		result.verdict = Verdict::Renderable;
		result.delivery = Delivery::DirectFileUrl;
		result.encoding = "MHTML";
		result.encodingDeclared = true;
		return result;
	}

	if (!looksLikeMarkup(lower)) {
		result.verdict = Verdict::NotHtml;
		result.detail = "no HTML markup found in first 64KB";
		return result;
	}

	if (result.encoding.empty()) {
		std::string declared = declaredCharset(lower);
		if (!declared.empty()) {
			result.encoding = declared;
			result.encodingDeclared = true;
		} else if (isValidUtf8(header)) {
			// Undeclared but valid UTF-8: the engine would reach the same
			// conclusion, so there is nothing to correct.
			result.encoding = "UTF-8";
			result.encodingDeclared = false;
		} else {
			result.encoding = fallbackEncoding;
			result.encodingDeclared = false;
		}
	}

	result.verdict = Verdict::Renderable;
	// Inline whenever the document fits. It is not only the charset that
	// depends on this path: the theme prelude the viewer injects (see
	// buildPrelude in the Qt6 backend) can only reach a document we hand
	// over as text. Past the ceiling setHtml() cannot carry the content at
	// all, so those files render as the engine sees them, undecorated.
	result.delivery = (result.fileSize <= kInlineCeiling)
	                      ? Delivery::InlineDecoded
	                      : Delivery::DirectFileUrl;
	return result;
}

}  // namespace HtmlProbe
