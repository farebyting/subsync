#include <string>
#include <vector>

#include "harness.h"
#include "subtitle.h"
#include "textutil.h"

using namespace subsync;

namespace {

bool identity_srt(const std::string& in) {
    std::vector<Cue> cues;
    if (!parse_srt(in, cues) || cues.empty()) return false;
    std::vector<ms> s, e;
    for (const Cue& c : cues) { s.push_back(c.start); e.push_back(c.end); }
    return write_srt(in, s, e) == in;
}

bool identity_vtt(const std::string& in) {
    std::vector<Cue> cues;
    if (!parse_vtt(in, cues) || cues.empty()) return false;
    std::vector<ms> s, e;
    for (const Cue& c : cues) { s.push_back(c.start); e.push_back(c.end); }
    return write_vtt(in, s, e) == in;
}

bool identity_ass(const std::string& in) {
    std::vector<Cue> cues;
    if (!parse_ass(in, cues) || cues.empty()) return false;
    std::vector<ms> s, e;
    for (const Cue& c : cues) { s.push_back(c.start); e.push_back(c.end); }
    return write_ass(in, s, e) == in;
}

} // namespace

int main() {
    // ---- SRT ----
    const std::string srt_lf =
        "1\n"
        "00:00:01,000 --> 00:00:03,500\n"
        "Hello there.\n"
        "\n"
        "2\n"
        "00:00:04,000 --> 00:00:06,250\n"
        "General Kenobi!\n"
        "<i>You are a bold one.</i>\n"
        "\n";
    {
        std::vector<Cue> cues;
        t::ok(parse_srt(srt_lf, cues), "srt parses");
        t::eq_i(static_cast<long long>(cues.size()), 2, "srt cue count");
        t::eq_i(cues[0].start, 1000, "srt first start");
        t::eq_i(cues[0].end, 3500, "srt first end");
        t::eq_i(cues[1].start, 4000, "srt second start");
        t::ok(cues[1].text.find("General Kenobi!") == 0, "srt multi-line text");
        t::ok(identity_srt(srt_lf), "srt identity round trip");
    }
    // CRLF must survive.
    {
        std::string crlf;
        for (char c : srt_lf) { if (c == '\n') crlf += '\r'; crlf += c; }
        t::ok(identity_srt(crlf), "srt CRLF round trip");
    }
    // Shifted write.
    {
        std::vector<Cue> cues;
        parse_srt(srt_lf, cues);
        std::vector<ms> s{2000, 5000}, e{4500, 7250};
        const std::string out = write_srt(srt_lf, s, e);
        std::vector<Cue> again;
        parse_srt(out, again);
        t::eq_i(again[0].start, 2000, "srt shifted start");
        t::eq_i(again[1].end, 7250, "srt shifted end");
    }

    // ---- VTT ----
    const std::string vtt =
        "WEBVTT\n"
        "\n"
        "00:00:01.000 --> 00:00:03.500\n"
        "Hello there.\n"
        "\n"
        "cue-two\n"
        "00:00:04.000 --> 00:00:06.250 align:start\n"
        "General Kenobi!\n";
    {
        std::vector<Cue> cues;
        t::ok(parse_vtt(vtt, cues), "vtt parses");
        t::eq_i(static_cast<long long>(cues.size()), 2, "vtt cue count");
        t::eq_i(cues[1].start, 4000, "vtt second start");
        t::ok(identity_vtt(vtt), "vtt identity round trip");
    }
    // MM:SS shape (no hours) must be preserved.
    {
        const std::string mmss =
            "WEBVTT\n"
            "\n"
            "01:02.500 --> 01:05.000\n"
            "Short form.\n";
        std::vector<Cue> cues;
        t::ok(parse_vtt(mmss, cues), "vtt mm:ss parses");
        t::eq_i(cues[0].start, 62500, "vtt mm:ss value");
        t::ok(identity_vtt(mmss), "vtt mm:ss round trip");
    }

    // ---- ASS ----
    const std::string ass =
        "[Script Info]\n"
        "Title: Demo\n"
        "ScriptType: v4.00+\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize\n"
        "Style: Default,Arial,20\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:03.50,Default,,0,0,0,,Hello there.\n"
        "Dialogue: 0,0:00:04.00,0:00:06.25,Default,,0,0,0,,{\\an8}General Kenobi!\n";
    {
        std::vector<Cue> cues;
        t::ok(parse_ass(ass, cues), "ass parses");
        t::eq_i(static_cast<long long>(cues.size()), 2, "ass event count");
        t::eq_i(cues[0].start, 1000, "ass first start");
        t::eq_i(cues[1].end, 6250, "ass second end");
        t::ok(cues[1].text.find("General Kenobi!") != std::string::npos, "ass text field");
        t::ok(identity_ass(ass), "ass identity round trip");
    }
    // Non-default Format: field order must be honoured.
    {
        const std::string reordered =
            "[Events]\n"
            "Format: Start, End, Text\n"
            "Dialogue: 0:00:02.00,0:00:04.00,Reordered fields.\n";
        std::vector<Cue> cues;
        t::ok(parse_ass(reordered, cues), "ass reordered format parses");
        t::eq_i(cues[0].start, 2000, "ass reordered start");
        t::eq_i(cues[0].end, 4000, "ass reordered end");
        t::ok(cues[0].text == "Reordered fields.", "ass reordered text");
        t::ok(identity_ass(reordered), "ass reordered round trip");
    }
    // A comma inside the Text field must not shift fields.
    {
        const std::string comma =
            "[Events]\n"
            "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
            "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,One, two, three.\n";
        std::vector<Cue> cues;
        parse_ass(comma, cues);
        t::eq_i(static_cast<long long>(cues.size()), 1, "ass comma cue count");
        t::ok(cues[0].text == "One, two, three.", "ass text keeps commas");
        t::ok(identity_ass(comma), "ass comma round trip");
    }

    // ---- MicroDVD ----
    const std::string sub =
        "{100}{200}Hello there.|Second line.\n"
        "{250}{400}General Kenobi!\n";
    {
        std::vector<Cue> cues;
        t::ok(parse_sub(sub, cues, 25.0), "microdvd parses");
        t::eq_i(cues[0].start, 4000, "microdvd frame->ms");
        t::eq_i(cues[1].start, 10000, "microdvd second start");
        t::ok(cues[0].text.find('\n') != std::string::npos, "microdvd | is a line break");
        t::ok(write_sub(sub, {4000, 10000}, {8000, 16000}, 25.0) == sub, "microdvd identity");
        t::ok(!parse_sub(sub, cues, 0.0), "microdvd refuses without fps");
    }

    // ---- time helpers ----
    {
        ms v = 0;
        t::ok(parse_srt_time("01:02:03,004", &v) && v == 3723004, "srt time parse");
        t::ok(parse_vtt_time("62:03.500", &v) && v == 3723500, "vtt mm:ss time parse");
        t::ok(parse_ass_time("1:02:03.45", &v) && v == 3723450, "ass time parse");
        t::ok(format_srt_time(3723004) == "01:02:03,004", "srt time format");
        t::ok(format_ass_time(3723450) == "1:02:03.45", "ass time format");
        t::ok(!parse_srt_time("nonsense", &v), "bad time rejected");
    }

    // ---- junk filtering ----
    {
        t::ok(is_junk_cue("[door slams]"), "bracketed sfx is junk");
        t::ok(is_junk_cue("(sighs)"), "parenthesised sfx is junk");
        t::ok(is_junk_cue("\xE2\x99\xAA"), "music note is junk");
        t::ok(is_junk_cue("Subtitles by Somebody"), "credit line is junk");
        t::ok(is_junk_cue("www.example.com"), "url is junk");
        t::ok(!is_junk_cue("Hello there."), "dialogue is not junk");
        t::ok(strip_markup("<i>Hello</i>") == "Hello", "markup stripped");
        t::ok(strip_markup("- Hello there.") == "Hello there.", "leading dash stripped");
    }

    return t::done("test_formats");
}
