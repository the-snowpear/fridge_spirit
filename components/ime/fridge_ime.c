// 冰箱小精灵 26 键全拼输入法核心。
// 支持 FIME1 词库：全拼前缀二分 + 词频 Top-K、在线简拼索引、选词后文本联想。
// 热路径只访问 PSRAM 中的索引与词条，不直接读 TF。

#include "fridge_ime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

// 候选池与分页上限：Top-K 控制内存与按键时延，页面可按编辑器选择 6 或 12 个候选。
// 性能约束（P4 按键热路径）：
// - 全拼/简拼扫描各最多 IME_MAX_SCAN 条，禁止全表线性扫；
// - 候选池固定 64，无动态扩容；
// - 首页用“短语配额”重排，避免再开第二份大索引。
#define IME_TOP_K 64
#define IME_MAX_SCAN 4096
#define IME_JIANPIN_MAX 12
// 选词联想：最多遍历词条数（每次点选触发一次，非按键热路径）。
#define IME_ASSOC_VISIT_LIMIT 8192
// Top-K 内至少为“拼音长于预编辑”的词保留名额，防止单字挤爆池子。
#define IME_POOL_PHRASE_MIN 24
// 候选首页（12 条）中优先插入的未完成拼音词数量。
#define IME_FIRST_PAGE_PHRASE 5

typedef struct __attribute__((packed)) {
    uint32_t pinyin_offset;
    uint32_t text_offset;
    uint16_t text_size;
    int32_t weight;
} ime_entry_t;

typedef struct {
    char pinyin[FRIDGE_IME_MAX_PINYIN + 1];
    char text[FRIDGE_IME_MAX_CANDIDATE_BYTES];
    int32_t weight;
} fallback_entry_t;

// 排序后的候选：score 越高越靠前；longer=1 表示拼音尚未打完（可联想多字词）。
typedef struct {
    const char *text;
    int32_t score;
    uint8_t longer;
} ranked_cand_t;

// 简拼倒排：jianpin 字符串按字典序排序，便于二分前缀查询。
typedef struct {
    char jianpin[IME_JIANPIN_MAX + 1];
    uint32_t entry_index;
    int32_t weight;
} jianpin_ref_t;

static const char *TAG = "fridge_ime";

// 无 TF 时的厨房高频兜底（同样走前缀/简拼逻辑）。
static const fallback_entry_t s_fallback[] = {
    // 厨房场景多字词权重抬高：在 Rime 通用词频里它们往往偏低，需靠 fallback 进首页。
    {"fanqie", "番茄", 52000}, {"fanqiejiang", "番茄酱", 40000}, {"fan", "番", 10000},
    {"qie", "茄", 9000},
    {"jidan", "鸡蛋", 52000}, {"ji", "鸡", 10000}, {"dan", "蛋", 9000},
    {"niunai", "牛奶", 52000}, {"niu", "牛", 10000}, {"nai", "奶", 9000},
    {"bocai", "菠菜", 48000}, {"bo", "菠", 9000}, {"cai", "菜", 7000},
    {"huanggua", "黄瓜", 48000}, {"huang", "黄", 9000}, {"gua", "瓜", 8000},
    {"suannai", "酸奶", 48000}, {"suan", "酸", 9000},
    {"doufu", "豆腐", 48000}, {"dou", "豆", 9000}, {"fu", "腐", 8000},
    {"pingguo", "苹果", 52000}, {"ping", "苹", 9000}, {"guo", "果", 8000},
    {"jintian", "今天", 40000}, {"jin", "今", 8000}, {"tian", "天", 7000},
    {"xihongshi", "西红柿", 52000}, {"xi", "西", 9000}, {"xigua", "西瓜", 48000},
    {"mogu", "蘑菇", 45000}, {"baicai", "白菜", 45000}, {"qincai", "芹菜", 42000},
    {"tudou", "土豆", 48000}, {"luobo", "萝卜", 45000}, {"yangcong", "洋葱", 45000},
};

// 合法普通话音节表（无调、小写），用于把连续拼音切成音节并提取简拼首字母。
// 按长度优先的贪心切分依赖“最长匹配”，表本身按字典序便于二分存在性检查。
static const char *const s_syllables[] = {
    "a", "ai", "an", "ang", "ao",
    "ba", "bai", "ban", "bang", "bao", "bei", "ben", "beng", "bi", "bian", "biao", "bie", "bin", "bing", "bo", "bu",
    "ca", "cai", "can", "cang", "cao", "ce", "cei", "cen", "ceng", "cha", "chai", "chan", "chang", "chao", "che",
    "chen", "cheng", "chi", "chong", "chou", "chu", "chua", "chuai", "chuan", "chuang", "chui", "chun", "chuo",
    "ci", "cong", "cou", "cu", "cuan", "cui", "cun", "cuo",
    "da", "dai", "dan", "dang", "dao", "de", "dei", "den", "deng", "di", "dia", "dian", "diao", "die", "ding",
    "diu", "dong", "dou", "du", "duan", "dui", "dun", "duo",
    "e", "ei", "en", "eng", "er",
    "fa", "fan", "fang", "fei", "fen", "feng", "fo", "fou", "fu",
    "ga", "gai", "gan", "gang", "gao", "ge", "gei", "gen", "geng", "gong", "gou", "gu", "gua", "guai", "guan",
    "guang", "gui", "gun", "guo",
    "ha", "hai", "han", "hang", "hao", "he", "hei", "hen", "heng", "hong", "hou", "hu", "hua", "huai", "huan",
    "huang", "hui", "hun", "huo",
    "ji", "jia", "jian", "jiang", "jiao", "jie", "jin", "jing", "jiong", "jiu", "ju", "juan", "jue", "jun",
    "ka", "kai", "kan", "kang", "kao", "ke", "ken", "keng", "kong", "kou", "ku", "kua", "kuai", "kuan", "kuang",
    "kui", "kun", "kuo",
    "la", "lai", "lan", "lang", "lao", "le", "lei", "leng", "li", "lia", "lian", "liang", "liao", "lie", "lin",
    "ling", "liu", "lo", "long", "lou", "lu", "luan", "lue", "lun", "luo", "lv",
    "ma", "mai", "man", "mang", "mao", "me", "mei", "men", "meng", "mi", "mian", "miao", "mie", "min", "ming",
    "miu", "mo", "mou", "mu",
    "na", "nai", "nan", "nang", "nao", "ne", "nei", "nen", "neng", "ni", "nian", "niang", "niao", "nie", "nin",
    "ning", "niu", "nong", "nou", "nu", "nuan", "nue", "nun", "nuo", "nv",
    "o", "ou",
    "pa", "pai", "pan", "pang", "pao", "pei", "pen", "peng", "pi", "pian", "piao", "pie", "pin", "ping", "po",
    "pou", "pu",
    "qi", "qia", "qian", "qiang", "qiao", "qie", "qin", "qing", "qiong", "qiu", "qu", "quan", "que", "qun",
    "ran", "rang", "rao", "re", "ren", "reng", "ri", "rong", "rou", "ru", "rua", "ruan", "rui", "run", "ruo",
    "sa", "sai", "san", "sang", "sao", "se", "sen", "seng", "sha", "shai", "shan", "shang", "shao", "she",
    "shei", "shen", "sheng", "shi", "shou", "shu", "shua", "shuai", "shuan", "shuang", "shui", "shun", "shuo",
    "si", "song", "sou", "su", "suan", "sui", "sun", "suo",
    "ta", "tai", "tan", "tang", "tao", "te", "tei", "teng", "ti", "tian", "tiao", "tie", "ting", "tong", "tou",
    "tu", "tuan", "tui", "tun", "tuo",
    "wa", "wai", "wan", "wang", "wei", "wen", "weng", "wo", "wu",
    "xi", "xia", "xian", "xiang", "xiao", "xie", "xin", "xing", "xiong", "xiu", "xu", "xuan", "xue", "xun",
    "ya", "yan", "yang", "yao", "ye", "yi", "yin", "ying", "yo", "yong", "you", "yu", "yuan", "yue", "yun",
    "za", "zai", "zan", "zang", "zao", "ze", "zei", "zen", "zeng", "zha", "zhai", "zhan", "zhang", "zhao",
    "zhe", "zhei", "zhen", "zheng", "zhi", "zhong", "zhou", "zhu", "zhua", "zhuai", "zhuan", "zhuang", "zhui",
    "zhun", "zhuo", "zi", "zong", "zou", "zu", "zuan", "zui", "zun", "zuo",
};

static ime_entry_t *s_entries;
static size_t s_entry_count;
static char *s_blob;
static size_t s_blob_size;
static lv_obj_t *s_textarea;
static fridge_ime_snapshot_t s_snapshot;

static ranked_cand_t s_ranked[IME_TOP_K];
static size_t s_ranked_count;
static size_t s_candidate_page_size = FRIDGE_IME_MAX_CANDIDATES;

static jianpin_ref_t *s_jianpin_refs;
static size_t s_jianpin_count;

static char s_last_commit[FRIDGE_IME_MAX_CANDIDATE_BYTES];

static void *ime_psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static const char *entry_pinyin(size_t index)
{
    return s_blob + s_entries[index].pinyin_offset;
}

static const char *entry_text(size_t index)
{
    return s_blob + s_entries[index].text_offset;
}

static int syllable_cmp(const void *key, const void *elem)
{
    return strcmp((const char *)key, *(const char *const *)elem);
}

static bool is_valid_syllable(const char *s, size_t len)
{
    if (len == 0 || len > 6) {
        return false;
    }
    char tmp[8];
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    // 词库把 ü 写作 v（如 lv/nv）。
    return bsearch(tmp, s_syllables, sizeof(s_syllables) / sizeof(s_syllables[0]),
                   sizeof(s_syllables[0]), syllable_cmp) != NULL;
}

// 连续拼音 → 简拼首字母串。贪心最长音节切分；切不动时前进 1 字母保底。
static bool pinyin_to_jianpin(const char *pinyin, char *out, size_t out_size)
{
    if (!pinyin || !out || out_size < 2) {
        return false;
    }
    size_t out_len = 0;
    size_t i = 0;
    size_t n = strlen(pinyin);
    while (i < n && out_len + 1 < out_size) {
        size_t best = 0;
        size_t max_try = n - i;
        if (max_try > 6) {
            max_try = 6;
        }
        for (size_t len = max_try; len >= 1; len--) {
            if (is_valid_syllable(pinyin + i, len)) {
                best = len;
                break;
            }
        }
        if (best == 0) {
            best = 1;
        }
        out[out_len++] = pinyin[i];
        i += best;
    }
    out[out_len] = '\0';
    return out_len > 0;
}

static int jianpin_ref_cmp(const void *a, const void *b)
{
    const jianpin_ref_t *ra = (const jianpin_ref_t *)a;
    const jianpin_ref_t *rb = (const jianpin_ref_t *)b;
    int c = strcmp(ra->jianpin, rb->jianpin);
    if (c != 0) {
        return c;
    }
    if (ra->weight != rb->weight) {
        return (rb->weight > ra->weight) ? 1 : -1;
    }
    return (ra->entry_index > rb->entry_index) - (ra->entry_index < rb->entry_index);
}

static void reset_page_candidates(void)
{
    s_snapshot.candidate_count = 0;
    memset(s_snapshot.candidates, 0, sizeof(s_snapshot.candidates));
}

static size_t ranked_count_longer(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_ranked_count; i++) {
        if (s_ranked[i].longer) {
            n++;
        }
    }
    return n;
}

// 在固定 Top-K 内插入：score 更大者保留；并为 longer 词保留最低名额。
// 同文案若再次命中（词典+厨房 fallback）只抬分、不占双槽。复杂度 O(K)。
static void ranked_insert(const char *text, int32_t score, bool longer)
{
    if (!text || !text[0]) {
        return;
    }
    for (size_t i = 0; i < s_ranked_count; i++) {
        if (strcmp(s_ranked[i].text, text) != 0) {
            continue;
        }
        if (score > s_ranked[i].score) {
            s_ranked[i].score = score;
        }
        // longer 一旦为真就保留，避免简拼/前缀标记被精确路径冲掉。
        if (longer) {
            s_ranked[i].longer = 1U;
        }
        return;
    }
    if (s_ranked_count < IME_TOP_K) {
        s_ranked[s_ranked_count].text = text;
        s_ranked[s_ranked_count].score = score;
        s_ranked[s_ranked_count].longer = longer ? 1U : 0U;
        s_ranked_count++;
        return;
    }

    // 找可替换槽：优先同类型最低分；若 longer 名额不足，允许挤掉最低分的非 longer。
    size_t longer_n = ranked_count_longer();
    size_t victim = IME_TOP_K;
    int32_t victim_score = INT32_MAX;

    for (size_t i = 0; i < s_ranked_count; i++) {
        bool same_kind = (s_ranked[i].longer != 0U) == longer;
        bool can_evict_exact_for_phrase =
            longer && s_ranked[i].longer == 0U && longer_n < IME_POOL_PHRASE_MIN;
        if (!same_kind && !can_evict_exact_for_phrase) {
            continue;
        }
        if (s_ranked[i].score < victim_score) {
            victim_score = s_ranked[i].score;
            victim = i;
        }
    }

    // 若名额策略下找不到同类 victim，回退到全局最低分（且不破坏 longer 保底）。
    if (victim >= s_ranked_count) {
        for (size_t i = 0; i < s_ranked_count; i++) {
            if (!longer && s_ranked[i].longer != 0U && longer_n <= IME_POOL_PHRASE_MIN) {
                continue;
            }
            if (s_ranked[i].score < victim_score) {
                victim_score = s_ranked[i].score;
                victim = i;
            }
        }
    }

    if (victim >= s_ranked_count || score <= victim_score) {
        return;
    }
    s_ranked[victim].text = text;
    s_ranked[victim].score = score;
    s_ranked[victim].longer = longer ? 1U : 0U;
}

static int ranked_score_cmp(const void *a, const void *b)
{
    const ranked_cand_t *ra = (const ranked_cand_t *)a;
    const ranked_cand_t *rb = (const ranked_cand_t *)b;
    if (ra->score != rb->score) {
        return (rb->score > ra->score) ? 1 : -1;
    }
    // 同分时 longer 词略优先，便于短输入出多字。
    if (ra->longer != rb->longer) {
        return (int)rb->longer - (int)ra->longer;
    }
    return strcmp(ra->text, rb->text);
}

static void ranked_sort(void)
{
    if (s_ranked_count > 1) {
        qsort(s_ranked, s_ranked_count, sizeof(s_ranked[0]), ranked_score_cmp);
    }
}

// 首页短语配额重排：在已按 score 排序的池上做稳定分流，O(K)。
// 不改变 Top-K 集合，只调整展示顺序，避免短音节单字占满第一页。
static void reorder_with_first_page_quota(void)
{
    if (s_ranked_count <= 1) {
        return;
    }

    ranked_cand_t exact[IME_TOP_K];
    ranked_cand_t phrase[IME_TOP_K];
    size_t ne = 0;
    size_t np = 0;
    for (size_t i = 0; i < s_ranked_count; i++) {
        if (s_ranked[i].longer) {
            phrase[np++] = s_ranked[i];
        } else {
            exact[ne++] = s_ranked[i];
        }
    }
    if (np == 0 || ne == 0) {
        return;
    }

    ranked_cand_t out[IME_TOP_K];
    size_t out_n = 0;
    size_t ei = 0;
    size_t pi = 0;

    size_t phrase_first = IME_FIRST_PAGE_PHRASE;
    if (phrase_first > np) {
        phrase_first = np;
    }
    size_t exact_first = FRIDGE_IME_MAX_CANDIDATES - phrase_first;
    if (exact_first > ne) {
        exact_first = ne;
    }
    // 首页名额若一侧不足，用另一侧补满到 12。
    size_t first_total = phrase_first + exact_first;
    if (first_total < FRIDGE_IME_MAX_CANDIDATES) {
        size_t need = FRIDGE_IME_MAX_CANDIDATES - first_total;
        size_t add_p = np - phrase_first;
        if (add_p > need) {
            add_p = need;
        }
        phrase_first += add_p;
        need -= add_p;
        size_t add_e = ne - exact_first;
        if (add_e > need) {
            add_e = need;
        }
        exact_first += add_e;
    }

    // 首页交错：词、字、词、字… 更接近手机候选条。
    size_t p_left = phrase_first;
    size_t e_left = exact_first;
    while (out_n < s_ranked_count && (p_left > 0 || e_left > 0)) {
        if (p_left > 0 && pi < np) {
            out[out_n++] = phrase[pi++];
            p_left--;
        }
        if (e_left > 0 && ei < ne && out_n < s_ranked_count) {
            out[out_n++] = exact[ei++];
            e_left--;
        }
        if ((p_left == 0 || pi >= np) && (e_left == 0 || ei >= ne)) {
            break;
        }
    }
    while (pi < np && out_n < s_ranked_count) {
        out[out_n++] = phrase[pi++];
    }
    while (ei < ne && out_n < s_ranked_count) {
        out[out_n++] = exact[ei++];
    }

    memcpy(s_ranked, out, out_n * sizeof(out[0]));
    s_ranked_count = out_n;
}

// 全拼贴合度：词频为主；取消“精确匹配 +50 万”的断层，让未打完的多字词能进池。
// longer 名额 + 首页配额负责展示优先级，这里只负责同类内排序。
static int32_t full_pinyin_score(const char *py, const char *preedit, int32_t weight, size_t text_bytes)
{
    size_t pre_len = strlen(preedit);
    size_t py_len = strlen(py);
    int32_t score = weight;
    if (py_len == pre_len) {
        // 精确音节：常用字靠词频；小幅精确加成，避免压死全部多字词。
        score += 30000;
        if (text_bytes >= 6) {
            score += 8000;
        }
    } else if (py_len > pre_len) {
        score += 15000;
        // 多字词（UTF-8 ≥2 汉字约 ≥6 字节）在部分输入时加分。
        if (text_bytes >= 6) {
            score += 45000;
        } else if (text_bytes >= 3) {
            score += 8000;
        }
        // 输入越长、剩余拼音越短，越接近用户目标词。
        score += (int32_t)(pre_len * 3500);
        size_t remain = py_len - pre_len;
        if (remain <= 5) {
            score += (int32_t)((6 - remain) * 2500);
        }
    }
    return score;
}

static int32_t jianpin_score(int32_t weight, size_t jianpin_len, size_t pre_len, size_t text_bytes)
{
    // 简拼本身就是“未打完”的快捷方式，略抬高多字词。
    int32_t score = weight + 25000;
    if (jianpin_len == pre_len) {
        score += 40000;
    }
    if (text_bytes >= 6) {
        score += 35000;
    }
    score += (int32_t)(pre_len * 2500);
    return score;
}

// 在按拼音排序的主表上找第一个 pinyin >= key 的下标。
static size_t pinyin_lower_bound(const char *key)
{
    size_t lo = 0;
    size_t hi = s_entry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(entry_pinyin(mid), key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void collect_full_pinyin_prefix(const char *preedit)
{
    if (!s_entries || s_entry_count == 0 || !preedit[0]) {
        return;
    }
    size_t pre_len = strlen(preedit);
    size_t start = pinyin_lower_bound(preedit);
    size_t scanned = 0;
    for (size_t i = start; i < s_entry_count && scanned < IME_MAX_SCAN; i++, scanned++) {
        const char *py = entry_pinyin(i);
        if (strncmp(py, preedit, pre_len) != 0) {
            break;
        }
        const char *text = entry_text(i);
        size_t py_len = strlen(py);
        bool longer = py_len > pre_len;
        ranked_insert(text,
                      full_pinyin_score(py, preedit, s_entries[i].weight, s_entries[i].text_size),
                      longer);
    }
}

static size_t jianpin_lower_bound(const char *key)
{
    size_t lo = 0;
    size_t hi = s_jianpin_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(s_jianpin_refs[mid].jianpin, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void collect_jianpin_prefix(const char *preedit)
{
    if (!s_jianpin_refs || s_jianpin_count == 0 || !preedit[0]) {
        return;
    }
    // 单字母简拼噪声极大，只在长度 ≥2 时启用。
    size_t pre_len = strlen(preedit);
    if (pre_len < 2) {
        return;
    }
    size_t start = jianpin_lower_bound(preedit);
    size_t scanned = 0;
    for (size_t i = start; i < s_jianpin_count && scanned < IME_MAX_SCAN; i++, scanned++) {
        const char *jp = s_jianpin_refs[i].jianpin;
        if (strncmp(jp, preedit, pre_len) != 0) {
            break;
        }
        uint32_t idx = s_jianpin_refs[i].entry_index;
        if (idx >= s_entry_count) {
            continue;
        }
        const char *text = entry_text(idx);
        size_t jp_len = strlen(jp);
        // 简拼命中一律视为“未打完”，参与首页短语配额。
        ranked_insert(text,
                      jianpin_score(s_jianpin_refs[i].weight, jp_len, pre_len, s_entries[idx].text_size),
                      true);
    }
}

static void collect_fallback(const char *preedit, bool allow_jianpin)
{
    size_t pre_len = strlen(preedit);
    if (pre_len == 0) {
        return;
    }
    for (size_t i = 0; i < sizeof(s_fallback) / sizeof(s_fallback[0]); i++) {
        if (strncmp(s_fallback[i].pinyin, preedit, pre_len) == 0) {
            size_t py_len = strlen(s_fallback[i].pinyin);
            ranked_insert(s_fallback[i].text,
                          full_pinyin_score(s_fallback[i].pinyin, preedit, s_fallback[i].weight,
                                           strlen(s_fallback[i].text)),
                          py_len > pre_len);
        }
        if (allow_jianpin && pre_len >= 2) {
            char jp[IME_JIANPIN_MAX + 1];
            if (pinyin_to_jianpin(s_fallback[i].pinyin, jp, sizeof(jp)) &&
                strncmp(jp, preedit, pre_len) == 0) {
                ranked_insert(s_fallback[i].text,
                              jianpin_score(s_fallback[i].weight, strlen(jp), pre_len,
                                           strlen(s_fallback[i].text)),
                              true);
            }
        }
    }
}

// 选词后：找“以刚上屏文本为前缀的更长词”，剩余部分作为联想候选展示。
// 性能：只遍历 IME_ASSOC_VISIT_LIMIT 条（步进访问），不建文本索引，避免额外 PSRAM。
// 词表按拼音排序，故覆盖不完全；厨房 fallback 先注入保证常用续写。
static void collect_association(const char *committed)
{
    s_ranked_count = 0;
    if (!committed || !committed[0]) {
        return;
    }
    size_t prefix_len = strlen(committed);

    // 先注入内置厨房续写，常数规模，保证番茄酱/西红柿等不依赖全表命中。
    for (size_t i = 0; i < sizeof(s_fallback) / sizeof(s_fallback[0]); i++) {
        size_t text_len = strlen(s_fallback[i].text);
        if (text_len <= prefix_len) {
            continue;
        }
        if (strncmp(s_fallback[i].text, committed, prefix_len) != 0) {
            continue;
        }
        ranked_insert(s_fallback[i].text + prefix_len, s_fallback[i].weight + 80000, true);
    }

    if (s_entries && s_entry_count > 0) {
        size_t visit = 0;
        size_t step = 1;
        // 词条很多时做稀疏抽样，把最坏访问控制在固定上限附近。
        if (s_entry_count > IME_ASSOC_VISIT_LIMIT) {
            step = s_entry_count / IME_ASSOC_VISIT_LIMIT;
            if (step < 1) {
                step = 1;
            }
        }
        for (size_t i = 0; i < s_entry_count && visit < IME_ASSOC_VISIT_LIMIT; i += step, visit++) {
            const char *text = entry_text(i);
            size_t text_len = s_entries[i].text_size;
            if (text_len <= prefix_len) {
                continue;
            }
            if (strncmp(text, committed, prefix_len) != 0) {
                continue;
            }
            const char *suffix = text + prefix_len;
            if (suffix[0] == '\0') {
                continue;
            }
            ranked_insert(suffix, s_entries[i].weight + 50000 + (int32_t)text_len, true);
        }
    }
    ranked_sort();
}

static void publish_ranked_page(void)
{
    reset_page_candidates();
    s_snapshot.candidate_total = s_ranked_count;
    s_snapshot.candidate_page_count =
        s_ranked_count == 0 ? 0 :
        (s_ranked_count + s_candidate_page_size - 1) / s_candidate_page_size;
    if (s_snapshot.candidate_page_count == 0) {
        s_snapshot.candidate_page = 0;
        return;
    }
    if (s_snapshot.candidate_page >= s_snapshot.candidate_page_count) {
        s_snapshot.candidate_page = s_snapshot.candidate_page_count - 1;
    }
    size_t first = s_snapshot.candidate_page * s_candidate_page_size;
    for (size_t i = first; i < s_ranked_count && i < first + s_candidate_page_size; i++) {
        strlcpy(s_snapshot.candidates[s_snapshot.candidate_count++], s_ranked[i].text,
                sizeof(s_snapshot.candidates[0]));
    }
}

static void collect_candidates(void)
{
    s_snapshot.association_mode = false;
    s_ranked_count = 0;
    if (!s_snapshot.preedit[0]) {
        // 空拼音：若刚上屏过词，展示联想；否则清空候选。
        if (s_last_commit[0]) {
            s_snapshot.association_mode = true;
            collect_association(s_last_commit);
            publish_ranked_page();
            return;
        }
        s_snapshot.candidate_total = 0;
        s_snapshot.candidate_page = 0;
        s_snapshot.candidate_page_count = 0;
        reset_page_candidates();
        return;
    }

    // 新输入打断联想上下文。
    s_last_commit[0] = '\0';
    // 厨房 fallback 先写入：同文案后续词典命中只会抬分，保证场景词优先占池。
    collect_fallback(s_snapshot.preedit, true);
    collect_full_pinyin_prefix(s_snapshot.preedit);
    collect_jianpin_prefix(s_snapshot.preedit);
    ranked_sort();
    // 仅对“有未完成拼音词”时做首页配额，精确匹配已打完时保持纯词频序。
    if (ranked_count_longer() > 0) {
        reorder_with_first_page_quota();
    }
    publish_ranked_page();
}

static esp_err_t build_jianpin_index(void)
{
    free(s_jianpin_refs);
    s_jianpin_refs = NULL;
    s_jianpin_count = 0;
    if (!s_entries || s_entry_count == 0) {
        return ESP_OK;
    }

    jianpin_ref_t *refs = ime_psram_malloc(s_entry_count * sizeof(jianpin_ref_t));
    if (!refs) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    for (size_t i = 0; i < s_entry_count; i++) {
        char jp[IME_JIANPIN_MAX + 1];
        if (!pinyin_to_jianpin(entry_pinyin(i), jp, sizeof(jp))) {
            continue;
        }
        // 单音节词简拼只有 1 字母，查询阶段也基本不用，仍保留便于 fq 类多音节。
        strlcpy(refs[count].jianpin, jp, sizeof(refs[count].jianpin));
        refs[count].entry_index = (uint32_t)i;
        refs[count].weight = s_entries[i].weight;
        count++;
    }
    if (count == 0) {
        free(refs);
        return ESP_OK;
    }
    qsort(refs, count, sizeof(refs[0]), jianpin_ref_cmp);
    s_jianpin_refs = refs;
    s_jianpin_count = count;
    ESP_LOGI(TAG, "jianpin index ready: %u refs", (unsigned)count);
    return ESP_OK;
}

static esp_err_t load_dictionary(void)
{
    FILE *file = fopen("/sdcard/fridge/assets/fontpack/v1/ime/pinyin_simp.bin", "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long file_size = ftell(file);
    if (file_size < 12 || file_size > (4 * 1024 * 1024)) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(file);

    uint8_t *data = ime_psram_malloc((size_t)file_size);
    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);

    if (memcmp(data, "FIME1", 5) != 0) {
        free(data);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t count = 0;
    memcpy(&count, data + 8, sizeof(count));
    if (count == 0 || count > 100000) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    if (count > (SIZE_MAX - 12U) / sizeof(ime_entry_t)) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t table_size = (size_t)count * sizeof(ime_entry_t);
    if (12 + table_size > (size_t)file_size) {
        free(data);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t blob_offset = 12 + table_size;
    size_t blob_size = (size_t)file_size - blob_offset;
    for (uint32_t i = 0; i < count; i++) {
        const ime_entry_t *entry = (const ime_entry_t *)(data + 12 + i * sizeof(*entry));
        if (entry->pinyin_offset >= blob_size || entry->text_offset >= blob_size ||
            entry->text_size == 0 || entry->text_size > blob_size - entry->text_offset ||
            memchr(data + blob_offset + entry->pinyin_offset, '\0',
                   blob_size - entry->pinyin_offset) == NULL ||
            memchr(data + blob_offset + entry->text_offset, '\0',
                   blob_size - entry->text_offset) == NULL) {
            free(data);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    s_entries = ime_psram_malloc(table_size);
    if (!s_entries) {
        free(data);
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_entries, data + 12, table_size);
    s_blob_size = blob_size;
    s_blob = ime_psram_malloc(s_blob_size + 1);
    if (!s_blob) {
        free(s_entries);
        s_entries = NULL;
        free(data);
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_blob, data + 12 + table_size, s_blob_size);
    s_blob[s_blob_size] = '\0';
    free(data);
    s_entry_count = count;
    s_snapshot.dictionary_loaded = true;

    esp_err_t jp_ret = build_jianpin_index();
    if (jp_ret != ESP_OK) {
        ESP_LOGW(TAG, "jianpin index skipped: %s", esp_err_to_name(jp_ret));
    }
    return ESP_OK;
}

// 延迟加载 TF 词库。该接口只应由 LVGL/UI 任务调用，避免与按键候选查询并发改写索引。
// 成功后保留当前预编辑内容并重新计算候选，使用户无需清空已经输入的拼音。
esp_err_t fridge_ime_try_load_tf_dictionary(void)
{
    if (s_snapshot.dictionary_loaded) {
        return ESP_OK;
    }

    esp_err_t ret = load_dictionary();
    if (ret == ESP_OK) {
        snprintf(s_snapshot.last_error, sizeof(s_snapshot.last_error),
                 "全拼+简拼+联想 | 词条%u 简拼%u", (unsigned)s_entry_count,
                 (unsigned)s_jianpin_count);
        collect_candidates();
        ESP_LOGI(TAG, "TF dictionary ready entries=%u jianpin=%u", (unsigned)s_entry_count,
                 (unsigned)s_jianpin_count);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        strlcpy(s_snapshot.last_error, "等待 TF 词库，暂用内置厨房词",
                sizeof(s_snapshot.last_error));
    } else {
        snprintf(s_snapshot.last_error, sizeof(s_snapshot.last_error), "词库加载失败: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t fridge_ime_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    free(s_entries);
    s_entries = NULL;
    free(s_blob);
    s_blob = NULL;
    free(s_jianpin_refs);
    s_jianpin_refs = NULL;
    s_entry_count = 0;
    s_blob_size = 0;
    s_jianpin_count = 0;
    s_ranked_count = 0;
    s_candidate_page_size = FRIDGE_IME_MAX_CANDIDATES;
    s_last_commit[0] = '\0';

    esp_err_t ret = fridge_ime_try_load_tf_dictionary();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "%s", s_snapshot.last_error);
    }
    return ESP_OK;
}

esp_err_t fridge_ime_attach_textarea(lv_obj_t *textarea)
{
    if (!textarea) {
        return ESP_ERR_INVALID_ARG;
    }
    s_textarea = textarea;
    return ESP_OK;
}

esp_err_t fridge_ime_set_candidate_page_size(size_t page_size)
{
    if (page_size == 0 || page_size > FRIDGE_IME_MAX_CANDIDATES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_candidate_page_size == page_size) {
        return ESP_OK;
    }
    // 切换编辑器时从新页面的第一页开始，避免沿用另一种页大小下的页码。
    s_candidate_page_size = page_size;
    s_snapshot.candidate_page = 0;
    publish_ranked_page();
    return ESP_OK;
}

void fridge_ime_feed_letter(char letter)
{
    if (!isalpha((unsigned char)letter)) {
        return;
    }
    size_t len = strlen(s_snapshot.preedit);
    if (len >= FRIDGE_IME_MAX_PINYIN) {
        return;
    }
    letter = (char)tolower((unsigned char)letter);
    s_snapshot.preedit[len] = letter;
    s_snapshot.preedit[len + 1] = '\0';
    s_snapshot.candidate_page = 0;
    collect_candidates();
}

void fridge_ime_backspace(void)
{
    size_t len = strlen(s_snapshot.preedit);
    if (len > 0) {
        s_snapshot.preedit[len - 1] = '\0';
        s_snapshot.candidate_page = 0;
        collect_candidates();
    } else if (s_snapshot.association_mode) {
        // 联想状态下退格：取消联想，回到空候选。
        s_last_commit[0] = '\0';
        s_snapshot.association_mode = false;
        s_ranked_count = 0;
        s_snapshot.candidate_total = 0;
        s_snapshot.candidate_page = 0;
        s_snapshot.candidate_page_count = 0;
        reset_page_candidates();
    } else if (s_textarea) {
        lv_textarea_delete_char(s_textarea);
    }
}

void fridge_ime_clear_preedit(void)
{
    s_snapshot.preedit[0] = '\0';
    s_snapshot.candidate_page = 0;
    s_snapshot.candidate_total = 0;
    s_snapshot.candidate_page_count = 0;
    s_snapshot.association_mode = false;
    s_last_commit[0] = '\0';
    s_ranked_count = 0;
    reset_page_candidates();
}

esp_err_t fridge_ime_select_candidate(size_t index)
{
    if (!s_textarea || index >= s_snapshot.candidate_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *chosen = s_snapshot.candidates[index];
    char committed[FRIDGE_IME_MAX_CANDIDATE_BYTES];

    if (s_snapshot.association_mode && s_last_commit[0]) {
        // 联想项是后缀：上屏后缀，完整词=上次提交+后缀，继续深层联想。
        lv_textarea_add_text(s_textarea, chosen);
        // 用两段 strlcpy/strlcat 避免长词拼接触发 format-truncation。
        strlcpy(committed, s_last_commit, sizeof(committed));
        strlcat(committed, chosen, sizeof(committed));
    } else {
        lv_textarea_add_text(s_textarea, chosen);
        strlcpy(committed, chosen, sizeof(committed));
    }

    s_snapshot.preedit[0] = '\0';
    s_snapshot.candidate_page = 0;
    strlcpy(s_last_commit, committed, sizeof(s_last_commit));
    collect_candidates();
    return ESP_OK;
}

void fridge_ime_get_snapshot(fridge_ime_snapshot_t *out)
{
    if (out) {
        *out = s_snapshot;
    }
}

esp_err_t fridge_ime_next_candidate_page(void)
{
    if (s_snapshot.candidate_page + 1 >= s_snapshot.candidate_page_count) {
        return ESP_ERR_NOT_FOUND;
    }
    s_snapshot.candidate_page++;
    publish_ranked_page();
    return ESP_OK;
}

esp_err_t fridge_ime_previous_candidate_page(void)
{
    if (s_snapshot.candidate_page == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_snapshot.candidate_page--;
    publish_ranked_page();
    return ESP_OK;
}
