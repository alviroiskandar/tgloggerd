// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_MCP_NOISEWORDS_HPP
#define TGLOGGERD_WEB_MCP_NOISEWORDS_HPP

#include <string_view>
#include <unordered_set>

/*
 * Corpus noise, as opposed to the grammatical stopwords in Stopwords.hpp.
 *
 * The vendored Indonesian and English stopword lists are built from formal
 * prose, and formal prose is not what a Telegram group produces. They contain
 * "tidak" but not "gak", "yang" but not "yg", and nothing at all for "wkwk",
 * "lol" or the Date:/Subject: lines of quoted mail. Left alone, the top of a
 * ranking fills with those instead of with what the group talks about --
 * measured on this archive, a month of one group ranked "aja", "gak", "nya",
 * "udah", "sih", "wkwk", "pake" and "kalo" in its top ten.
 *
 * Deliberately a SEPARATE, hand-maintained file rather than an addition to
 * Stopwords.hpp: that one is generated from upstream lists and regenerating it
 * would silently drop anything added here. Grouped by why each word is noise,
 * because the judgement is what a future reader will want to argue with.
 *
 * A hash set, not a sorted array, precisely because it is hand-maintained: an
 * out-of-order insertion in a binary-searched array fails silently and the word
 * simply stops being filtered. Here order cannot matter.
 *
 * The line to hold is TOPICAL vs NOT. Words that could name a subject stay out
 * of this list even when they are frequent -- "net", "io", "id" and "co" are
 * omitted for exactly that reason: they read as noise in a chat corpus and as
 * kernel subsystems in this one. Callers who disagree with a specific call have
 * telegram_popular_words's `exclude` and `include_stopwords` arguments.
 */
namespace tgweb::mcp::noisewords {

/* True when `w` (already lowercased and elongation-collapsed) is noise. */
inline bool isNoiseWord(std::string_view w)
{
	static const std::unordered_set<std::string_view> kNoise = {
		/* Indonesian negation, affirmation and their chat spellings. */
		"bener", "beneran", "bukannya", "emang", "emangnya", "emg",
		"enggak", "ga", "gak", "gakpapa", "gapapa", "gk", "gpp",
		"iya", "iyaa", "iyah", "iye", "kaga", "kagak", "ngak", "ngga",
		"nggak", "oke", "okelah", "okey", "siap", "sip", "udahlah",
		"yaa", "yah", "yasudah", "yaudah", "yoi", "yup",

		/* Indonesian discourse particles -- they attach to any sentence and mean nothing alone. */
		"aja", "ajah", "dah", "deh", "doang", "doank", "dong", "donk",
		"ealah", "kah", "kan", "kek", "kok", "lah", "lho", "loh",
		"mah", "nih", "ny", "nya", "sih", "toh", "tuh", "ya",
		"yaelah", "yalah",

		/* Indonesian colloquial pronouns and address forms. */
		"abang", "ane", "bang", "bro", "bund", "bunda", "doi", "elo",
		"elu", "ente", "gan", "gua", "gue", "gw", "kak", "kakak",
		"lo", "lu", "mas", "mba", "mbak", "mimin", "min", "om", "sis",
		"tante",

		/* Indonesian colloquial spellings of words the formal list already covers. */
		"abis", "apaan", "banget", "belom", "bentar", "bgt", "bikin",
		"blm", "bngt", "bntr", "dgn", "dgr", "dikit", "dll", "dmn",
		"drpd", "dsb", "dst", "gimana", "gini", "gitu", "gmn", "gmna",
		"gt", "intinya", "jd", "jg", "kalo", "karna", "kaya", "kayak",
		"kayanya", "klo", "klu", "knapa", "knp", "krn", "liat",
		"makanya", "mirip", "ngapain", "ngerti", "ntar", "paham",
		"pake", "pokoknya", "sebenernya", "skrg", "soalnya", "spt",
		"sy", "tadi", "tau", "tdk", "tp", "tpi", "trs", "trus", "tsb",
		"udah", "udh", "utk", "yg",

		/* Interjections and evaluations -- reaction, not subject. */
		"aduh", "anjay", "anjir", "anjrit", "astaga", "awok",
		"awokawok", "buset", "ehh", "gokil", "hadeh", "haduh", "haha",
		"hehe", "hihi", "hoho", "keren", "mantap", "mantul",
		"masyaallah", "njir", "ohh", "parah", "serius", "waduh",
		"wah", "weh", "wih", "wkwk", "wkwkwk", "xixi",

		/* English internet shorthand. */
		"afaik", "afk", "aka", "asap", "atm", "brb", "btw", "etc",
		"ez", "fyi", "gg", "gj", "gotcha", "idc", "idk", "iirc",
		"ikr", "ily", "imho", "imma", "imo", "irl", "lel", "lmao",
		"lmfao", "lmk", "lol", "ngl", "nvm", "omfg", "omg", "rofl",
		"smh", "tbh", "tldr", "ttyl", "wdym", "wtf", "wth", "xd",
		"xdd",

		/* English chat fillers, backchannels and interjections. */
		"aah", "actually", "ahh", "aight", "aint", "alright",
		"anyway", "anyways", "basically", "bruh", "coz", "crap",
		"cuz", "damn", "dang", "dude", "dunno", "fck", "fuck",
		"fucking", "gimme", "gonna", "gotta", "guys", "hello", "hey",
		"hmm", "huh", "kay", "kinda", "lemme", "like", "literally",
		"mate", "meh", "nah", "nop", "nope", "okay", "okie", "ooh",
		"oops", "pls", "plz", "probs", "prolly", "really", "rly",
		"seriously", "shit", "sorry", "sorta", "srsly", "sure",
		"thank", "thanks", "tho", "thru", "thx", "tks", "tysm", "ugh",
		"uhh", "umm", "wanna", "well", "wow", "yall", "yea", "yeah",
		"yep",

		/* Quoted-mail and forwarded-message scaffolding. */
		"bcc", "cc", "cheers", "date", "dear", "forwarded", "fw",
		"fwd", "regards", "replied", "reply", "sent", "sincerely",
		"subject", "unsubscribe", "wrote",

		/* Calendar vocabulary -- when something happened, never what it was about. */
		"apr", "april", "aug", "august", "bulan", "dec", "december",
		"detik", "feb", "february", "fri", "friday", "gmt", "hari",
		"jam", "jan", "january", "jul", "july", "jumat", "jun",
		"june", "kamis", "malam", "mar", "march", "may", "menit",
		"minggu", "mon", "monday", "nov", "november", "oct",
		"october", "pagi", "rabu", "sabtu", "sat", "saturday",
		"selasa", "senin", "sep", "sept", "september", "siang",
		"sore", "sun", "sunday", "tahun", "thu", "thur", "thurs",
		"thursday", "tue", "tues", "tuesday", "utc", "wed",
		"wednesday", "wib", "wita",

		/* Leftovers of pasted links and markup that survive URL stripping. */
		"com", "gif", "htm", "html", "http", "https", "jpeg", "jpg",
		"org", "pdf", "php", "png", "svg", "www",
	};
	return kNoise.count(w) != 0;
}

} /* namespace tgweb::mcp::noisewords */

#endif /* TGLOGGERD_WEB_MCP_NOISEWORDS_HPP */
