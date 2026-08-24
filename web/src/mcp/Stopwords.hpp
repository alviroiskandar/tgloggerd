// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * GENERATED -- do not edit by hand.
 *
 * Indonesian and English stopwords, merged and de-duplicated, for the word
 * frequency tool. Vendored rather than fetched at runtime: a tool must not
 * depend on a third-party URL being reachable, and a word list that changes
 * under you silently changes your results.
 *
 * Sources:
 *   https://raw.githubusercontent.com/stopwords-iso/stopwords-id/refs/heads/master/stopwords-id.txt
 *   https://gist.githubusercontent.com/sebleier/554280 (NLTK English stopwords)
 *
 * 885 unique entries. Sorted, so the lookup below can binary-search.
 */
#ifndef TGLOGGERD_WEB_MCP_STOPWORDS_HPP
#define TGLOGGERD_WEB_MCP_STOPWORDS_HPP

#include <cstddef>
#include <string_view>

namespace tgweb::mcp::stopwords {

inline constexpr std::string_view kStopwords[] = {
	"a", "about", "above", "ada", "adalah", "adanya", "adapun", "after",
	"again", "against", "agak", "agaknya", "agar", "akan", "akankah", "akhir",
	"akhiri", "akhirnya", "aku", "akulah", "all", "am", "amat", "amatlah", "an",
	"and", "anda", "andalah", "antar", "antara", "antaranya", "any", "apa",
	"apaan", "apabila", "apakah", "apalagi", "apatah", "are", "artinya", "as",
	"asal", "asalkan", "at", "atas", "atau", "ataukah", "ataupun", "awal",
	"awalnya", "bagai", "bagaikan", "bagaimana", "bagaimanakah", "bagaimanapun",
	"bagi", "bagian", "bahkan", "bahwa", "bahwasanya", "baik", "bakal",
	"bakalan", "balik", "banyak", "bapak", "baru", "bawah", "be", "beberapa",
	"because", "been", "before", "begini", "beginian", "beginikah", "beginilah",
	"begitu", "begitukah", "begitulah", "begitupun", "being", "bekerja",
	"belakang", "belakangan", "below", "belum", "belumlah", "benar", "benarkah",
	"benarlah", "berada", "berakhir", "berakhirlah", "berakhirnya", "berapa",
	"berapakah", "berapalah", "berapapun", "berarti", "berawal", "berbagai",
	"berdatangan", "beri", "berikan", "berikut", "berikutnya", "berjumlah",
	"berkali-kali", "berkata", "berkehendak", "berkeinginan", "berkenaan",
	"berlainan", "berlalu", "berlangsung", "berlebihan", "bermacam",
	"bermacam-macam", "bermaksud", "bermula", "bersama", "bersama-sama",
	"bersiap", "bersiap-siap", "bertanya", "bertanya-tanya", "berturut",
	"berturut-turut", "bertutur", "berujar", "berupa", "besar", "betul",
	"betulkah", "between", "biasa", "biasanya", "bila", "bilakah", "bisa",
	"bisakah", "boleh", "bolehkah", "bolehlah", "both", "buat", "bukan",
	"bukankah", "bukanlah", "bukannya", "bulan", "bung", "but", "by", "can",
	"cara", "caranya", "cukup", "cukupkah", "cukuplah", "cuma", "dahulu",
	"dalam", "dan", "dapat", "dari", "daripada", "datang", "dekat", "demi",
	"demikian", "demikianlah", "dengan", "depan", "di", "dia", "diakhiri",
	"diakhirinya", "dialah", "diantara", "diantaranya", "diberi", "diberikan",
	"diberikannya", "dibuat", "dibuatnya", "did", "didapat", "didatangkan",
	"digunakan", "diibaratkan", "diibaratkannya", "diingat", "diingatkan",
	"diinginkan", "dijawab", "dijelaskan", "dijelaskannya", "dikarenakan",
	"dikatakan", "dikatakannya", "dikerjakan", "diketahui", "diketahuinya",
	"dikira", "dilakukan", "dilalui", "dilihat", "dimaksud", "dimaksudkan",
	"dimaksudkannya", "dimaksudnya", "diminta", "dimintai", "dimisalkan",
	"dimulai", "dimulailah", "dimulainya", "dimungkinkan", "dini", "dipastikan",
	"diperbuat", "diperbuatnya", "dipergunakan", "diperkirakan",
	"diperlihatkan", "diperlukan", "diperlukannya", "dipersoalkan",
	"dipertanyakan", "dipunyai", "diri", "dirinya", "disampaikan", "disebut",
	"disebutkan", "disebutkannya", "disini", "disinilah", "ditambahkan",
	"ditandaskan", "ditanya", "ditanyai", "ditanyakan", "ditegaskan",
	"ditujukan", "ditunjuk", "ditunjuki", "ditunjukkan", "ditunjukkannya",
	"ditunjuknya", "dituturkan", "dituturkannya", "diucapkan", "diucapkannya",
	"diungkapkan", "do", "does", "doing", "don", "dong", "down", "dua", "dulu",
	"during", "each", "empat", "enggak", "enggaknya", "entah", "entahlah",
	"few", "for", "from", "further", "guna", "gunakan", "had", "hal", "hampir",
	"hanya", "hanyalah", "hari", "harus", "haruslah", "harusnya", "has", "have",
	"having", "he", "hendak", "hendaklah", "hendaknya", "her", "here", "hers",
	"herself", "him", "himself", "hingga", "his", "how", "i", "ia", "ialah",
	"ibarat", "ibaratkan", "ibaratnya", "ibu", "if", "ikut", "in", "ingat",
	"ingat-ingat", "ingin", "inginkah", "inginkan", "ini", "inikah", "inilah",
	"into", "is", "it", "its", "itself", "itu", "itukah", "itulah", "jadi",
	"jadilah", "jadinya", "jangan", "jangankan", "janganlah", "jauh", "jawab",
	"jawaban", "jawabnya", "jelas", "jelaskan", "jelaslah", "jelasnya", "jika",
	"jikalau", "juga", "jumlah", "jumlahnya", "just", "justru", "kala", "kalau",
	"kalaulah", "kalaupun", "kalian", "kami", "kamilah", "kamu", "kamulah",
	"kan", "kapan", "kapankah", "kapanpun", "karena", "karenanya", "kasus",
	"kata", "katakan", "katakanlah", "katanya", "ke", "keadaan", "kebetulan",
	"kecil", "kedua", "keduanya", "keinginan", "kelamaan", "kelihatan",
	"kelihatannya", "kelima", "keluar", "kembali", "kemudian", "kemungkinan",
	"kemungkinannya", "kenapa", "kepada", "kepadanya", "kesampaian",
	"keseluruhan", "keseluruhannya", "keterlaluan", "ketika", "khususnya",
	"kini", "kinilah", "kira", "kira-kira", "kiranya", "kita", "kitalah", "kok",
	"kurang", "lagi", "lagian", "lah", "lain", "lainnya", "lalu", "lama",
	"lamanya", "lanjut", "lanjutnya", "lebih", "lewat", "lima", "luar", "macam",
	"maka", "makanya", "makin", "malah", "malahan", "mampu", "mampukah", "mana",
	"manakala", "manalagi", "masa", "masalah", "masalahnya", "masih",
	"masihkah", "masing", "masing-masing", "mau", "maupun", "me", "melainkan",
	"melakukan", "melalui", "melihat", "melihatnya", "memang", "memastikan",
	"memberi", "memberikan", "membuat", "memerlukan", "memihak", "meminta",
	"memintakan", "memisalkan", "memperbuat", "mempergunakan", "memperkirakan",
	"memperlihatkan", "mempersiapkan", "mempersoalkan", "mempertanyakan",
	"mempunyai", "memulai", "memungkinkan", "menaiki", "menambahkan",
	"menandaskan", "menanti", "menanti-nanti", "menantikan", "menanya",
	"menanyai", "menanyakan", "mendapat", "mendapatkan", "mendatang",
	"mendatangi", "mendatangkan", "menegaskan", "mengakhiri", "mengapa",
	"mengatakan", "mengatakannya", "mengenai", "mengerjakan", "mengetahui",
	"menggunakan", "menghendaki", "mengibaratkan", "mengibaratkannya",
	"mengingat", "mengingatkan", "menginginkan", "mengira", "mengucapkan",
	"mengucapkannya", "mengungkapkan", "menjadi", "menjawab", "menjelaskan",
	"menuju", "menunjuk", "menunjuki", "menunjukkan", "menunjuknya", "menurut",
	"menuturkan", "menyampaikan", "menyangkut", "menyatakan", "menyebutkan",
	"menyeluruh", "menyiapkan", "merasa", "mereka", "merekalah", "merupakan",
	"meski", "meskipun", "meyakini", "meyakinkan", "minta", "mirip", "misal",
	"misalkan", "misalnya", "more", "most", "mula", "mulai", "mulailah",
	"mulanya", "mungkin", "mungkinkah", "my", "myself", "nah", "naik", "namun",
	"nanti", "nantinya", "no", "nor", "not", "now", "nyaris", "nyatanya", "of",
	"off", "oleh", "olehnya", "on", "once", "only", "or", "other", "our",
	"ours", "ourselves", "out", "over", "own", "pada", "padahal", "padanya",
	"pak", "paling", "panjang", "pantas", "para", "pasti", "pastilah",
	"penting", "pentingnya", "per", "percuma", "perlu", "perlukah", "perlunya",
	"pernah", "persoalan", "pertama", "pertama-tama", "pertanyaan",
	"pertanyakan", "pihak", "pihaknya", "pukul", "pula", "pun", "punya", "rasa",
	"rasanya", "rata", "rupanya", "s", "saat", "saatnya", "saja", "sajalah",
	"saling", "sama", "sama-sama", "sambil", "same", "sampai", "sampai-sampai",
	"sampaikan", "sana", "sangat", "sangatlah", "satu", "saya", "sayalah", "se",
	"sebab", "sebabnya", "sebagai", "sebagaimana", "sebagainya", "sebagian",
	"sebaik", "sebaik-baiknya", "sebaiknya", "sebaliknya", "sebanyak",
	"sebegini", "sebegitu", "sebelum", "sebelumnya", "sebenarnya", "seberapa",
	"sebesar", "sebetulnya", "sebisanya", "sebuah", "sebut", "sebutlah",
	"sebutnya", "secara", "secukupnya", "sedang", "sedangkan", "sedemikian",
	"sedikit", "sedikitnya", "seenaknya", "segala", "segalanya", "segera",
	"seharusnya", "sehingga", "seingat", "sejak", "sejauh", "sejenak",
	"sejumlah", "sekadar", "sekadarnya", "sekali", "sekali-kali", "sekalian",
	"sekaligus", "sekalipun", "sekarang", "sekecil", "seketika", "sekiranya",
	"sekitar", "sekitarnya", "sekurang-kurangnya", "sekurangnya", "sela",
	"selagi", "selain", "selaku", "selalu", "selama", "selama-lamanya",
	"selamanya", "selanjutnya", "seluruh", "seluruhnya", "semacam", "semakin",
	"semampu", "semampunya", "semasa", "semasih", "semata", "semata-mata",
	"semaunya", "sementara", "semisal", "semisalnya", "sempat", "semua",
	"semuanya", "semula", "sendiri", "sendirian", "sendirinya", "seolah",
	"seolah-olah", "seorang", "sepanjang", "sepantasnya", "sepantasnyalah",
	"seperlunya", "seperti", "sepertinya", "sepihak", "sering", "seringnya",
	"serta", "serupa", "sesaat", "sesama", "sesampai", "sesegera", "sesekali",
	"seseorang", "sesuatu", "sesuatunya", "sesudah", "sesudahnya", "setelah",
	"setempat", "setengah", "seterusnya", "setiap", "setiba", "setibanya",
	"setidak-tidaknya", "setidaknya", "setinggi", "seusai", "sewaktu", "she",
	"should", "siap", "siapa", "siapakah", "siapapun", "sini", "sinilah", "so",
	"soal", "soalnya", "some", "suatu", "such", "sudah", "sudahkah", "sudahlah",
	"supaya", "t", "tadi", "tadinya", "tahu", "tahun", "tak", "tambah",
	"tambahnya", "tampak", "tampaknya", "tandas", "tandasnya", "tanpa", "tanya",
	"tanyakan", "tanyanya", "tapi", "tegas", "tegasnya", "telah", "tempat",
	"tengah", "tentang", "tentu", "tentulah", "tentunya", "tepat", "terakhir",
	"terasa", "terbanyak", "terdahulu", "terdapat", "terdiri", "terhadap",
	"terhadapnya", "teringat", "teringat-ingat", "terjadi", "terjadilah",
	"terjadinya", "terkira", "terlalu", "terlebih", "terlihat", "termasuk",
	"ternyata", "tersampaikan", "tersebut", "tersebutlah", "tertentu",
	"tertuju", "terus", "terutama", "tetap", "tetapi", "than", "that", "the",
	"their", "theirs", "them", "themselves", "then", "there", "these", "they",
	"this", "those", "through", "tiap", "tiba", "tiba-tiba", "tidak",
	"tidakkah", "tidaklah", "tiga", "tinggi", "to", "toh", "too", "tunjuk",
	"turut", "tutur", "tuturnya", "ucap", "ucapnya", "ujar", "ujarnya", "umum",
	"umumnya", "under", "ungkap", "ungkapnya", "until", "untuk", "up", "usah",
	"usai", "very", "waduh", "wah", "wahai", "waktu", "waktunya", "walau",
	"walaupun", "was", "we", "were", "what", "when", "where", "which", "while",
	"who", "whom", "why", "will", "with", "wong", "yaitu", "yakin", "yakni",
	"yang", "you", "your", "yours", "yourself", "yourselves",
};

inline constexpr std::size_t kStopwordCount = sizeof(kStopwords) / sizeof(kStopwords[0]);

/* True when `w` (already lowercased) is a stopword. Binary search. */
inline bool isStopword(std::string_view w)
{
	std::size_t lo = 0, hi = kStopwordCount;
	while (lo < hi) {
		const std::size_t mid = lo + (hi - lo) / 2;
		if (kStopwords[mid] < w)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < kStopwordCount && kStopwords[lo] == w;
}

} /* namespace tgweb::mcp::stopwords */

#endif /* TGLOGGERD_WEB_MCP_STOPWORDS_HPP */
