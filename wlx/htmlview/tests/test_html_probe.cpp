// Checkpoints for the accept/decline decision. Every case here is one the
// reference QTextBrowser implementation gets wrong by returning a valid
// window handle unconditionally -- if any of these regress, DC's fallback
// to its own viewer stops working and the user gets an empty pane.

#include "core/html_probe.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static int g_failures = 0;

static std::string writeTemp(const std::string &name, const std::string &bytes) {
	std::string path = std::string("/tmp/htmlview_test_") + name;
	std::ofstream out(path, std::ios::binary);
	out.write(bytes.data(), (std::streamsize)bytes.size());
	out.close();
	return path;
}

static const char *verdictName(HtmlProbe::Verdict v) {
	switch (v) {
	case HtmlProbe::Verdict::Renderable: return "Renderable";
	case HtmlProbe::Verdict::NotHtml: return "NotHtml";
	case HtmlProbe::Verdict::Unreadable: return "Unreadable";
	case HtmlProbe::Verdict::TooLarge: return "TooLarge";
	}
	return "?";
}

static void expectVerdict(const char *what, const std::string &path,
                          HtmlProbe::Verdict expected,
                          uint64_t maxBytes = 32 * 1024 * 1024) {
	HtmlProbe::Result r =
	    HtmlProbe::probeFile(path, maxBytes, "windows-1252");
	if (r.verdict != expected) {
		printf("FAIL %-28s expected %s, got %s (%s)\n", what,
		       verdictName(expected), verdictName(r.verdict), r.detail.c_str());
		g_failures++;
	} else {
		printf("ok   %-28s %s\n", what, verdictName(r.verdict));
	}
}

static void expectEncoding(const char *what, const std::string &path,
                           const std::string &expected, bool expectDeclared) {
	HtmlProbe::Result r =
	    HtmlProbe::probeFile(path, 32 * 1024 * 1024, "windows-1252");
	if (r.encoding != expected || r.encodingDeclared != expectDeclared) {
		printf("FAIL %-28s expected %s(declared=%d), got %s(declared=%d)\n",
		       what, expected.c_str(), (int)expectDeclared, r.encoding.c_str(),
		       (int)r.encodingDeclared);
		g_failures++;
	} else {
		printf("ok   %-28s %s (declared=%d)\n", what, r.encoding.c_str(),
		       (int)r.encodingDeclared);
	}
}

int main() {
	// --- what we must decline ----------------------------------------
	expectVerdict("missing file", "/tmp/htmlview_test_does_not_exist",
	              HtmlProbe::Verdict::Unreadable);

	expectVerdict("empty file", writeTemp("empty.html", ""),
	              HtmlProbe::Verdict::NotHtml);

	// A JPEG that someone renamed. The reference plugin renders this as an
	// empty pane; we hand it back to DC.
	std::string jpeg("\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01", 15);
	jpeg.append(2000, '\x7F');
	expectVerdict("binary named .html", writeTemp("binary.html", jpeg),
	              HtmlProbe::Verdict::NotHtml);

	expectVerdict("plain text, no markup",
	              writeTemp("plain.html", "just some notes, nothing marked up"),
	              HtmlProbe::Verdict::NotHtml);

	expectVerdict("oversized", writeTemp("big.html", std::string(4096, 'x') +
	                                                     "<html><body>hi"),
	              HtmlProbe::Verdict::TooLarge, 1024);

	// --- what we must accept -----------------------------------------
	expectVerdict("plain html",
	              writeTemp("basic.html", "<!doctype html><html><body>hi"),
	              HtmlProbe::Verdict::Renderable);

	expectVerdict("fragment without <html>",
	              writeTemp("fragment.html", "<div class=x>content</div>"),
	              HtmlProbe::Verdict::Renderable);

	// RFC 2557 container: not HTML by markup, still renderable.
	expectVerdict("mhtml archive",
	              writeTemp("archive.mht",
	                        "From: <Saved by Blink>\r\nMIME-Version: 1.0\r\n"
	                        "Content-Type: multipart/related; boundary=\"x\"\r\n"),
	              HtmlProbe::Verdict::Renderable);

	// --- charset resolution ------------------------------------------
	expectEncoding("declared meta charset",
	               writeTemp("meta.html",
	                         "<html><head><meta charset=\"iso-8859-7\">"
	                         "</head><body>hi"),
	               "iso-8859-7", true);

	expectEncoding("declared http-equiv",
	               writeTemp("equiv.html",
	                         "<html><head><meta http-equiv=\"Content-Type\" "
	                         "content=\"text/html; charset=windows-1251\">"
	                         "</head><body>hi"),
	               "windows-1251", true);

	expectEncoding("xhtml xml declaration",
	               writeTemp("xml.xhtml",
	                         "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	                         "<html><body>hi"),
	               "utf-8", true);

	expectEncoding("utf-8 BOM outranks meta",
	               writeTemp("bom.html",
	                         "\xEF\xBB\xBF<html><head><meta charset=\"latin1\">"
	                         "</head><body>hi"),
	               "UTF-8", true);

	// A UTF-16 document is full of NUL bytes; the binary check must not
	// fire before the BOM is read.
	std::string utf16("\xFF\xFE", 2);
	for (const char *p = "<html><body>hi"; *p; ++p) {
		utf16 += *p;
		utf16 += '\0';
	}
	expectVerdict("utf-16le is not binary", writeTemp("utf16.html", utf16),
	              HtmlProbe::Verdict::Renderable);
	expectEncoding("utf-16le BOM", writeTemp("utf16.html", utf16), "UTF-16LE",
	               true);

	// Undeclared and valid UTF-8: the engine would agree, so we record
	// that we guessed rather than claiming the document said so.
	expectEncoding("undeclared valid utf-8",
	               writeTemp("undeclared_utf8.html",
	                         "<html><body>caf\xC3\xA9 na\xC3\xAFve"),
	               "UTF-8", false);

	// Undeclared and *not* valid UTF-8 -- the legacy page that renders as
	// mojibake when Chromium applies its UTF-8 default.
	expectEncoding("undeclared legacy bytes",
	               writeTemp("undeclared_cp1252.html",
	                         "<html><body>caf\xE9 na\xEFve"),
	               "windows-1252", false);

	// --- transcode ----------------------------------------------------
	std::string decoded;
	if (!HtmlProbe::decodeToUtf8("caf\xE9", "windows-1252", decoded) ||
	    decoded != "caf\xC3\xA9") {
		printf("FAIL %-28s got \"%s\"\n", "cp1252 -> utf-8", decoded.c_str());
		g_failures++;
	} else {
		printf("ok   %-28s %s\n", "cp1252 -> utf-8", decoded.c_str());
	}

	if (HtmlProbe::decodeToUtf8("x", "not-a-real-charset", decoded)) {
		printf("FAIL %-28s unknown charset reported success\n",
		       "unknown charset declines");
		g_failures++;
	} else {
		printf("ok   %-28s declined\n", "unknown charset declines");
	}

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
	       g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
