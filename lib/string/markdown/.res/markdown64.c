/*
 * markdown64.c —— Nodio 自写 x64 Markdown 渲染器（sundown 行为兼容）
 *
 * 背景：
 *   aardio 官方 lib/string/markdown 用 $"..." 内嵌一个 32 位 sundown.dll 再内存加载，
 *   x64 进程无法装载该 PE（实测 "无法加载 DLL: (内嵌的内存映像)"）。
 *   官方未随库发布 sundown 源码（.res/README.TXT 声明该组件是作者改过的 sundown，
 *   许可条款禁止脱离 aardio 使用），因此本文件是 Nodio 侧**独立实现**的等价渲染器。
 *
 * 目标：
 *   1. 导出名 / 调用约定 / 结构体布局与官方 sundown.dll 一致，
 *      使 aardio 侧只需要换 DLL 路径即可调用（见 lib/string/markdown/_.aardio）。
 *   2. 输出的 HTML 与官方 32 位 sundown 实测结果逐字节一致
 *      （基准见 docs/capability-gap.md §1.32）。
 *
 * 结构体布局（必须与 aardio 的 html_renderopt / sd_callbacks 类完全一致）：
 *   struct html_renderopt { int toc_data[3]; int flags; void *link_attributes; }
 *       toc_data 对应 aardio 的嵌套结构体 {int header_count; int current_level; int level_offset;}
 *       flags 位于偏移 12（x64 布局：0..11 toc_data，12..15 flags，16..23 link_attributes）
 *   struct sd_callbacks  26 个指针，顺序与 aardio sd_callbacks 类字段顺序逐项一致。
 *
 * 编译：MSVC x64  cl /nologo /LD /O2 /MT /GS- /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
# define MD_EXPORT __declspec(dllexport)
# define md_stricmp  _stricmp
# define md_strnicmp _strnicmp
#else
# define MD_EXPORT
# define md_stricmp  strcasecmp
# define md_strnicmp strncasecmp
#endif

#ifdef _WIN32
# define md_tolower(c) ((int)(((unsigned char)(c) >= 'A' && (unsigned char)(c) <= 'Z') ? ((unsigned char)(c) - 'A' + 'a') : (unsigned char)(c)))
#else
# define md_tolower(c) tolower((unsigned char)(c))
#endif

/* ================================================================== */
/* 基础工具                                                            */
/* ================================================================== */

static int md_isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int md_isalnum(int c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int md_isalpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ================================================================== */
/* 缓冲区                                                              */
/* ================================================================== */

struct buf {
	char  *data;
	size_t size;
	size_t asize;
};

static void buf_init(struct buf *b)
{
	b->data = NULL;
	b->size = 0;
	b->asize = 0;
}

static void buf_reserve(struct buf *b, size_t need)
{
	size_t ns;
	char  *nd;

	if (b->asize >= need + 1)
		return;

	ns = b->asize ? b->asize : 64;
	while (ns < need + 1)
		ns <<= 1;

	nd = (char *)realloc(b->data, ns);
	if (!nd) {
		fprintf(stderr, "markdown64: out of memory\n");
		exit(1);
	}
	memset(nd + b->asize, 0, ns - b->asize);
	b->data = nd;
	b->asize = ns;
}

static void buf_put(struct buf *b, const char *s, size_t n)
{
	if (!n)
		return;
	buf_reserve(b, b->size + n);
	memcpy(b->data + b->size, s, n);
	b->size += n;
	b->data[b->size] = 0;
}

static void buf_puts(struct buf *b, const char *s)
{
	buf_put(b, s, strlen(s));
}

static void buf_putc(struct buf *b, char c)
{
	buf_reserve(b, b->size + 1);
	b->data[b->size++] = c;
	b->data[b->size] = 0;
}

static void buf_reset(struct buf *b)
{
	b->size = 0;
	if (b->data)
		b->data[0] = 0;
}

/* 去掉尾部所有换行 */
static void buf_trim_eol(struct buf *b)
{
	while (b->size && b->data[b->size - 1] == '\n')
		b->size--;
	if (b->data)
		b->data[b->size] = 0;
}

/* ================================================================== */
/* 导出：buffer 家族（与官方 sundown.dll 同名同签名）                  */
/* ================================================================== */

MD_EXPORT void *bufnew(unsigned int size)
{
	struct buf *ret = (struct buf *)malloc(sizeof(struct buf));
	if (!ret)
		return NULL;
	if (size < 16)
		size = 16;
	ret->data = (char *)malloc(size);
	if (!ret->data) {
		free(ret);
		return NULL;
	}
	memset(ret->data, 0, size);
	ret->size = 0;
	ret->asize = size;
	return ret;
}

MD_EXPORT int bufgrow(void *p, unsigned int size)
{
	struct buf *b = (struct buf *)p;
	if (!b)
		return -1;
	if (b->asize >= size + 1)
		return 0;
	buf_reserve(b, size);
	return 0;
}

MD_EXPORT void bufrelease(void *p)
{
	struct buf *b = (struct buf *)p;
	if (!b)
		return;
	free(b->data);
	free(b);
}

MD_EXPORT const char *bufcstr(void *p)
{
	struct buf *b = (struct buf *)p;
	if (!b)
		return "";
	if (!b->data)
		return "";
	b->data[b->size] = 0;
	return b->data;
}

MD_EXPORT unsigned int bufsize(void *p)
{
	struct buf *b = (struct buf *)p;
	return b ? (unsigned int)b->size : 0;
}

MD_EXPORT void *bufdata(void *p)
{
	struct buf *b = (struct buf *)p;
	return b ? b->data : NULL;
}

MD_EXPORT int bufput(void *p, const void *data, unsigned int len)
{
	struct buf *b = (struct buf *)p;
	if (!b)
		return -1;
	if (len)
		buf_put(b, (const char *)data, len);
	return (int)b->size;
}

MD_EXPORT int bufputc(void *p, int c)
{
	struct buf *b = (struct buf *)p;
	if (!b)
		return -1;
	buf_putc(b, (char)c);
	return (int)b->size;
}


MD_EXPORT unsigned int bufprefix(void *p, const char *prefix)
{
	struct buf *b = (struct buf *)p;
	size_t len;

	if (!b || !b->data || !prefix)
		return 0;
	len = strlen(prefix);
	if (len > b->size)
		return 0;
	return memcmp(b->data, prefix, len) == 0 ? (unsigned int)len : 0;
}

/* ================================================================== */
/* 选项 / 回调结构体                                                   */
/* ================================================================== */

#define MKD_LIST_ORDERED   1
#define MKD_LI_BLOCK       2
#define MKD_LI_END         4
#define MKD_LI_TASK        8
#define MKD_LI_CHECKED     16

#define MKD_TABLE_ALIGN_L      1
#define MKD_TABLE_ALIGN_R      2
#define MKD_TABLE_ALIGN_CENTER 3
#define MKD_TABLE_ALIGNMASK    3
#define MKD_TABLE_HEADER       4

#define MKDA_NOT_AUTOLINK  0
#define MKDA_NORMAL        1
#define MKDA_EMAIL         2

#define MKDEXT_NO_INTRA_EMPHASIS  0x01
#define MKDEXT_TABLES             0x02
#define MKDEXT_FENCED_CODE        0x04
#define MKDEXT_AUTOLINK           0x08
#define MKDEXT_STRIKETHROUGH      0x10
#define MKDEXT_SPACE_HEADERS      0x40
#define MKDEXT_SUPERSCRIPT        0x80
#define MKDEXT_LAX_SPACING        0x100

#define HTML_SKIP_HTML     0x01
#define HTML_SKIP_STYLE    0x02
#define HTML_SKIP_IMAGES   0x04
#define HTML_SKIP_LINKS    0x08
#define HTML_EXPAND_TABS   0x10
#define HTML_SAFELINK      0x20
#define HTML_TOC           0x40
#define HTML_HARD_WRAP     0x80
#define HTML_USE_XHTML     0x100
#define HTML_ESCAPE        0x200
#define HTML_TASK_ENABLED  0x400

struct html_renderopt {
	int    toc_data[3];   /* header_count, current_level, level_offset */
	int    flags;
	void  *link_attributes;
};

struct sd_callbacks {
	void (*blockcode)(struct buf *ob, const struct buf *text, const struct buf *lang, void *opaque);
	void (*blockquote)(struct buf *ob, const struct buf *text, void *opaque);
	void (*blockhtml)(struct buf *ob, const struct buf *text, void *opaque);
	void (*header)(struct buf *ob, const struct buf *text, int level, void *opaque);
	void (*hrule)(struct buf *ob, void *opaque);
	void (*list)(struct buf *ob, const struct buf *text, int flags, void *opaque);
	void (*listitem)(struct buf *ob, const struct buf *text, int flags, void *opaque);
	void (*paragraph)(struct buf *ob, const struct buf *text, void *opaque);
	void (*table)(struct buf *ob, const struct buf *header, const struct buf *body, void *opaque);
	void (*table_row)(struct buf *ob, const struct buf *text, void *opaque);
	void (*table_cell)(struct buf *ob, const struct buf *text, int flags, void *opaque);
	int  (*utolink)(struct buf *ob, const struct buf *link, int type, void *opaque);
	int  (*codespan)(struct buf *ob, const struct buf *text, void *opaque);
	int  (*double_emphasis)(struct buf *ob, const struct buf *text, void *opaque);
	int  (*emphasis)(struct buf *ob, const struct buf *text, void *opaque);
	int  (*image)(struct buf *ob, const struct buf *link, const struct buf *title, const struct buf *alt, void *opaque);
	int  (*linebreak)(struct buf *ob, void *opaque);
	int  (*link)(struct buf *ob, const struct buf *link, const struct buf *title, const struct buf *content, void *opaque);
	int  (*raw_html_tag)(struct buf *ob, const struct buf *tag, void *opaque);
	int  (*triple_emphasis)(struct buf *ob, const struct buf *text, void *opaque);
	int  (*strikethrough)(struct buf *ob, const struct buf *text, void *opaque);
	int  (*superscript)(struct buf *ob, const struct buf *text, void *opaque);
	void (*entity)(struct buf *ob, const struct buf *entity, void *opaque);
	void (*normal_text)(struct buf *ob, const struct buf *text, void *opaque);
	void (*doc_header)(struct buf *ob, void *opaque);
	void (*doc_footer)(struct buf *ob, void *opaque);
};

struct link_ref {
	struct buf id;
	struct buf link;
	struct buf title;
};

struct sd_markdown {
	struct sd_callbacks   cb;
	struct html_renderopt *options;
	unsigned int          ext_flags;
	size_t                max_nesting;
	int                   in_link_body;
	int                   depth;
	struct link_ref      *refs;
	size_t                nrefs;
	size_t                maxrefs;
};

static void parse_block(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size);
static void parse_inline(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size);

/* ================================================================== */
/* HTML 转义                                                           */
/* ================================================================== */

/* 普通文本转义（官方 fork 行为：& < > " ' 全部转义） */
static void esc_text(struct buf *ob, const char *src, size_t len)
{
	size_t i = 0, org;
	while (i < len) {
		org = i;
		while (i < len && src[i] != '<' && src[i] != '>' && src[i] != '&'
			&& src[i] != '"' && src[i] != '\'')
			i++;
		if (i > org)
			buf_put(ob, src + org, i - org);
		if (i >= len)
			break;
		switch (src[i]) {
		case '<':  buf_puts(ob, "&lt;");   break;
		case '>':  buf_puts(ob, "&gt;");   break;
		case '&':  buf_puts(ob, "&amp;");  break;
		case '"':  buf_puts(ob, "&quot;"); break;
		case '\'': buf_puts(ob, "&#39;");  break;
		}
		i++;
	}
}

/* 常规 HTML 转义（用于 code span / title / alt） */
static void esc_html(struct buf *ob, const char *src, size_t len)
{
	size_t i = 0, org;
	while (i < len) {
		org = i;
		while (i < len && src[i] != '<' && src[i] != '>' && src[i] != '&' && src[i] != '"')
			i++;
		if (i > org)
			buf_put(ob, src + org, i - org);
		if (i >= len)
			break;
		switch (src[i]) {
		case '<':  buf_puts(ob, "&lt;");   break;
		case '>':  buf_puts(ob, "&gt;");   break;
		case '&':  buf_puts(ob, "&amp;");  break;
		case '"':  buf_puts(ob, "&quot;"); break;
		}
		i++;
	}
}

static int href_issafe(const char *src, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c == '&' || c == '\'' || c == ' ')
			return 0;
		if (c < 32 || c >= 127)
			return 0;
	}
	return 1;
}

static void esc_href(struct buf *ob, const char *src, size_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i = 0, org;
	char tmp[3];

	tmp[0] = '%';
	while (i < len) {
		org = i;
		while (i < len && href_issafe(src + i, 1))
			i++;
		if (i > org)
			buf_put(ob, src + org, i - org);
		if (i >= len)
			break;
		switch (src[i]) {
		case '&':
			buf_puts(ob, "&amp;");
			break;
		case '\'':
			buf_puts(ob, "&#x27;");
			break;
		case ' ':
			buf_puts(ob, "%20");
			break;
		default:
			tmp[1] = hex[((unsigned char)src[i] >> 4) & 0xF];
			tmp[2] = hex[(unsigned char)src[i] & 0xF];
			buf_put(ob, tmp, 3);
			break;
		}
		i++;
	}
}

/* ================================================================== */
/* 默认 HTML 渲染回调                                                   */
/* ================================================================== */

static void rndr_blockcode(struct buf *ob, const struct buf *text, const struct buf *lang, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;

	if (ob->size)
		buf_putc(ob, '\n');

	if (lang && lang->size) {
		size_t i = 0, org = 0;
		buf_puts(ob, "<pre><code class=\"");
		while (i < lang->size) {
			while (i < lang->size && md_isspace((unsigned char)lang->data[i]))
				i++;
			if (i >= lang->size)
				break;
			if (org < i)
				buf_put(ob, lang->data + org, i - org);
			while (i < lang->size && !md_isspace((unsigned char)lang->data[i]))
				i++;
			if (i < lang->size) {
				buf_putc(ob, ' ');
				org = i;
			}
		}
		if (org < i)
			buf_put(ob, lang->data + org, i - org);
		/* 官方 fork 额外输出 language-xxx 类名 */
		{
			const char *p = lang->data;
			size_t n = lang->size, k = 0;
			while (k < n && md_isspace((unsigned char)p[k]))
				k++;
			while (k < n && !md_isspace((unsigned char)p[k]))
				k++;
			if (k > 0) {
				buf_puts(ob, " language-");
				buf_put(ob, p, k);
			}
		}
		buf_puts(ob, "\">");
	} else {
		buf_puts(ob, "<pre><code>");
	}

	if (text)
		buf_put(ob, text->data, text->size);

	buf_puts(ob, "</code></pre>\n");
}

static void rndr_blockquote(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (ob->size)
		buf_putc(ob, '\n');
	buf_puts(ob, "<blockquote>\n");
	if (text)
		buf_put(ob, text->data, text->size);
	buf_puts(ob, "</blockquote>\n");
}

static void rndr_blockhtml(struct buf *ob, const struct buf *text, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;
	size_t org = 0, sz;

	if ((options->flags & HTML_SKIP_HTML) || (options->flags & HTML_ESCAPE))
		return;

	sz = text ? text->size : 0;
	while (sz > 0 && text->data[sz - 1] == '\n')
		sz--;
	while (org < sz && text->data[org] == '\n')
		org++;
	if (org >= sz)
		return;

	buf_put(ob, text->data + org, sz - org);
	buf_putc(ob, '\n');
}

static void rndr_header(struct buf *ob, const struct buf *text, int level, void *opaque)
{
	(void)opaque;
	if (ob->size)
		buf_putc(ob, '\n');
	buf_putc(ob, '<');
	buf_putc(ob, 'h');
	buf_putc(ob, (char)('0' + level));
	buf_putc(ob, '>');
	if (text)
		buf_put(ob, text->data, text->size);
	buf_puts(ob, "</h");
	buf_putc(ob, (char)('0' + level));
	buf_puts(ob, ">\n");
}

static void rndr_hrule(struct buf *ob, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;
	if (ob->size)
		buf_putc(ob, '\n');
	buf_puts(ob, (options->flags & HTML_USE_XHTML) ? "<hr/>\n" : "<hr>\n");
}

static void rndr_list(struct buf *ob, const struct buf *text, int flags, void *opaque)
{
	(void)opaque;
	if (ob->size)
		buf_putc(ob, '\n');
	buf_puts(ob, (flags & MKD_LIST_ORDERED) ? "<ol>\n" : "<ul>\n");
	if (text)
		buf_put(ob, text->data, text->size);
	buf_puts(ob, (flags & MKD_LIST_ORDERED) ? "</ol>\n" : "</ul>\n");
}

static void rndr_listitem(struct buf *ob, const struct buf *text, int flags, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;
	int label = 0;

	buf_puts(ob, "<li>");

	/* 任务列表：启用态且非块级列表项时，用 <label> 包裹整项（官方行为） */
	if ((flags & MKD_LI_TASK) && (options->flags & HTML_TASK_ENABLED) && !(flags & MKD_LI_BLOCK))
		label = 1;

	if (label)
		buf_puts(ob, "<label>");

	if (text) {
		size_t size = text->size;
		while (size && text->data[size - 1] == '\n')
			size--;
		buf_put(ob, text->data, size);
	}

	if (label)
		buf_puts(ob, "</label>");

	buf_puts(ob, "</li>\n");
}

static void rndr_paragraph(struct buf *ob, const struct buf *text, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;
	size_t i = 0, size;

	if (ob->size)
		buf_putc(ob, '\n');

	if (!text || !text->size)
		return;

	while (i < text->size && md_isspace((unsigned char)text->data[i]))
		i++;
	if (i == text->size)
		return;

	/* 去掉段落末尾的换行（sundown 行为） */
	size = text->size;
	while (size > i && (text->data[size - 1] == '\n' || text->data[size - 1] == '\r'))
		size--;
	if (size <= i)
		return;

	buf_puts(ob, "<p>");
	if (options->flags & HTML_HARD_WRAP) {
		size_t org;
		while (i < size) {
			org = i;
			while (i < size && text->data[i] != '\n')
				i++;
			if (i > org)
				buf_put(ob, text->data + org, i - org);
			if (i >= size - 1)
				break;
			buf_puts(ob, "<br>\n");
			i++;
		}
	} else {
		buf_put(ob, text->data + i, size - i);
	}
	buf_puts(ob, "</p>\n");
}

static void rndr_table(struct buf *ob, const struct buf *header, const struct buf *body, void *opaque)
{
	(void)opaque;
	if (ob->size)
		buf_putc(ob, '\n');
	buf_puts(ob, "<table><thead>\n");
	if (header)
		buf_put(ob, header->data, header->size);
	buf_puts(ob, "</thead><tbody>\n");
	if (body)
		buf_put(ob, body->data, body->size);
	buf_puts(ob, "</tbody></table>\n");
}

static void rndr_table_row(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	buf_puts(ob, "<tr>\n");
	if (text)
		buf_put(ob, text->data, text->size);
	buf_puts(ob, "</tr>\n");
}

static void rndr_table_cell(struct buf *ob, const struct buf *text, int flags, void *opaque)
{
	(void)opaque;
	buf_puts(ob, (flags & MKD_TABLE_HEADER) ? "<th" : "<td");
	switch (flags & MKD_TABLE_ALIGNMASK) {
	case MKD_TABLE_ALIGN_CENTER:
		buf_puts(ob, " style=\"text-align:center;\">");
		break;
	case MKD_TABLE_ALIGN_L:
		buf_puts(ob, " style=\"text-align:left;\">");
		break;
	case MKD_TABLE_ALIGN_R:
		buf_puts(ob, " style=\"text-align:right;\">");
		break;
	default:
		buf_putc(ob, '>');
		break;
	}
	if (text)
		buf_put(ob, text->data, text->size);
	buf_puts(ob, (flags & MKD_TABLE_HEADER) ? "</th>\n" : "</td>\n");
}

static int rndr_autolink(struct buf *ob, const struct buf *link, int type, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;
	int is_www;

	if (!link || !link->size)
		return 0;

	if ((options->flags & HTML_SAFELINK) && !href_issafe(link->data, link->size)
		&& type != MKDA_NOT_AUTOLINK)
		return 0;

	is_www = (link->size >= 4 && md_strnicmp(link->data, "www.", 4) == 0);

	buf_puts(ob, "<a href=\"");
	if (type == MKDA_EMAIL)
		buf_puts(ob, "mailto:");
	else if (is_www)
		buf_puts(ob, "http://");
	esc_href(ob, link->data, link->size);
	buf_puts(ob, "\">");

	if (link->size >= 7 && strncmp(link->data, "mailto:", 7) == 0)
		esc_html(ob, link->data + 7, link->size - 7);
	else
		esc_html(ob, link->data, link->size);

	buf_puts(ob, "</a>");
	return 1;
}

static int rndr_codespan(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	buf_puts(ob, "<code>");
	if (text)
		esc_html(ob, text->data, text->size);
	buf_puts(ob, "</code>");
	return 1;
}

static int rndr_double_emphasis(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (!text || !text->size)
		return 0;
	buf_puts(ob, "<strong>");
	buf_put(ob, text->data, text->size);
	buf_puts(ob, "</strong>");
	return 1;
}

static int rndr_emphasis(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (!text || !text->size)
		return 0;
	buf_puts(ob, "<em>");
	buf_put(ob, text->data, text->size);
	buf_puts(ob, "</em>");
	return 1;
}

static int rndr_triple_emphasis(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (!text || !text->size)
		return 0;
	buf_puts(ob, "<strong><em>");
	buf_put(ob, text->data, text->size);
	buf_puts(ob, "</em></strong>");
	return 1;
}

static int rndr_strikethrough(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (!text || !text->size)
		return 0;
	buf_puts(ob, "<del>");
	buf_put(ob, text->data, text->size);
	buf_puts(ob, "</del>");
	return 1;
}

static int rndr_superscript(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (!text || !text->size)
		return 0;
	buf_puts(ob, "<sup>");
	buf_put(ob, text->data, text->size);
	buf_puts(ob, "</sup>");
	return 1;
}

static int rndr_image(struct buf *ob, const struct buf *link, const struct buf *title,
	const struct buf *alt, void *opaque)
{
	(void)opaque;
	if (!link || !link->size)
		return 0;
	buf_puts(ob, "<img src=\"");
	esc_href(ob, link->data, link->size);
	buf_puts(ob, "\" alt=\"");
	if (alt && alt->size)
		esc_html(ob, alt->data, alt->size);
	if (title && title->size) {
		buf_puts(ob, "\" title=\"");
		esc_html(ob, title->data, title->size);
	}
	buf_puts(ob, "\">");
	return 1;
}

static int rndr_linebreak(struct buf *ob, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;
	buf_puts(ob, (options->flags & HTML_USE_XHTML) ? "<br/>\n" : "<br>\n");
	return 1;
}

static int rndr_link(struct buf *ob, const struct buf *link, const struct buf *title,
	const struct buf *content, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;

	if (link && (options->flags & HTML_SAFELINK) && !href_issafe(link->data, link->size)) {
		buf_put(ob, link->data, link->size);
		return 1;
	}

	buf_puts(ob, "<a href=\"");
	if (link && link->size)
		esc_href(ob, link->data, link->size);
	if (title && title->size) {
		buf_puts(ob, "\" title=\"");
		esc_html(ob, title->data, title->size);
	}
	buf_puts(ob, "\">");
	if (content && content->size)
		buf_put(ob, content->data, content->size);
	buf_puts(ob, "</a>");
	return 1;
}

static int rndr_raw_html(struct buf *ob, const struct buf *tag, void *opaque)
{
	struct html_renderopt *options = (struct html_renderopt *)opaque;

	if ((options->flags & HTML_SKIP_HTML) || (options->flags & HTML_ESCAPE))
		return 0;

	if (tag && tag->size)
		buf_put(ob, tag->data, tag->size);
	return 1;
}

static void rndr_entity(struct buf *ob, const struct buf *entity, void *opaque)
{
	(void)opaque;
	if (entity && entity->size)
		buf_put(ob, entity->data, entity->size);
}

static void rndr_normal_text(struct buf *ob, const struct buf *text, void *opaque)
{
	(void)opaque;
	if (text && text->size)
		esc_text(ob, text->data, text->size);
}

static void rndr_doc_header(struct buf *ob, void *opaque)
{
	(void)opaque;
	(void)ob;
}

static void rndr_doc_footer(struct buf *ob, void *opaque)
{
	(void)opaque;
	(void)ob;
}

static const struct sd_callbacks cb_default = {
	rndr_blockcode,
	rndr_blockquote,
	rndr_blockhtml,
	rndr_header,
	rndr_hrule,
	rndr_list,
	rndr_listitem,
	rndr_paragraph,
	rndr_table,
	rndr_table_row,
	rndr_table_cell,
	rndr_autolink,
	rndr_codespan,
	rndr_double_emphasis,
	rndr_emphasis,
	rndr_image,
	rndr_linebreak,
	rndr_link,
	rndr_raw_html,
	rndr_triple_emphasis,
	rndr_strikethrough,
	rndr_superscript,
	rndr_entity,
	rndr_normal_text,
	rndr_doc_header,
	rndr_doc_footer
};

/* ================================================================== */
/* 链接引用表                                                          */
/* ================================================================== */

static struct link_ref *find_link_ref(struct sd_markdown *rndr, const char *id, size_t id_len)
{
	size_t i;
	if (!id_len)
		return NULL;
	for (i = 0; i < rndr->nrefs; i++) {
		struct link_ref *r = &rndr->refs[i];
		if (r->id.size == id_len && memcmp(r->id.data, id, id_len) == 0)
			return r;
	}
	return NULL;
}

static void add_link_ref(struct sd_markdown *rndr, const char *id, size_t id_len,
	const char *link, size_t link_len, const char *title, size_t title_len)
{
	struct link_ref *r;

	if (find_link_ref(rndr, id, id_len))
		return;

	if (rndr->nrefs == rndr->maxrefs) {
		size_t n = rndr->maxrefs ? rndr->maxrefs * 2 : 16;
		struct link_ref *nr = (struct link_ref *)realloc(rndr->refs, n * sizeof(struct link_ref));
		if (!nr)
			return;
		rndr->refs = nr;
		rndr->maxrefs = n;
	}

	r = &rndr->refs[rndr->nrefs++];
	buf_init(&r->id);
	buf_init(&r->link);
	buf_init(&r->title);
	buf_put(&r->id, id, id_len);
	buf_put(&r->link, link, link_len);
	buf_put(&r->title, title, title_len);
}

/* ================================================================== */
/* 块级解析辅助                                                        */
/* ================================================================== */

/* 空行（含行尾换行）返回长度，否则 0 */
static size_t is_empty(const char *data, size_t size)
{
	size_t i;
	for (i = 0; i < size && data[i] != '\n'; i++)
		if (data[i] != ' ')
			return 0;
	return i + 1;
}

/* 水平分隔线 */
static int is_hrule(const char *data, size_t size)
{
	size_t i = 0, n = 0;
	char c;

	if (size < 3)
		return 0;
	if (data[0] == ' ') {
		i++;
		if (data[1] == ' ') {
			i++;
			if (data[2] == ' ')
				i++;
		}
	}
	if (i + 2 >= size || (data[i] != '*' && data[i] != '-' && data[i] != '_'))
		return 0;
	c = data[i];
	while (i < size && data[i] != '\n') {
		if (data[i] == c)
			n++;
		else if (data[i] != ' ')
			return 0;
		i++;
	}
	return n >= 3;
}

/* setext 下划线：=== 为 1 级，--- 为 2 级，不是下划线返回 0 */
static int is_headerline(const char *data, size_t size)
{
	size_t i = 0;

	if (size < 1)
		return 0;

	/* 1 级标题下划线 */
	if (data[i] == '=') {
		for (i = 1; i < size && data[i] == '='; i++)
			;
		while (i < size && data[i] == ' ')
			i++;
		return (i >= size || data[i] == '\n') ? 1 : 0;
	}

	/* 2 级标题下划线 */
	if (data[i] == '-') {
		for (i = 1; i < size && data[i] == '-'; i++)
			;
		while (i < size && data[i] == ' ')
			i++;
		return (i >= size || data[i] == '\n') ? 2 : 0;
	}

	return 0;
}

/* 下一行是否是 setext 下划线（用于区分列表项与标题下划线） */
static int is_next_headerline(const char *data, size_t size)
{
	size_t i = 0;

	while (i < size && data[i] != '\n')
		i++;

	if (++i >= size)
		return 0;

	return is_headerline(data + i, size - i);
}

/* ATX 标题起始，返回级别（0 表示不是标题） */
static int is_atxheader(struct sd_markdown *rndr, const char *data, size_t size)
{
	size_t level = 0;

	while (level < size && level < 6 && data[level] == '#')
		level++;
	if (!level)
		return 0;
	if (level < size && data[level] != ' ') {
		/* 没有空格时，只有在未启用 SPACE_HEADERS 时才算标题 */
		if (rndr->ext_flags & MKDEXT_SPACE_HEADERS)
			return 0;
	}
	return (int)level;
}

/* 引用块前缀 */
static size_t prefix_quote(const char *data, size_t size)
{
	size_t i = 0;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == '>') {
		if (i + 1 < size && data[i + 1] == ' ')
			return i + 2;
		return i + 1;
	}
	return 0;
}

/* 缩进代码前缀 */
static size_t prefix_code(const char *data, size_t size)
{
	if (size > 3 && data[0] == ' ' && data[1] == ' ' && data[2] == ' ' && data[3] == ' ')
		return 4;
	return 0;
}

/* 无序列表前缀 */
static size_t prefix_uli(const char *data, size_t size)
{
	size_t i = 0;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == ' ') i++;
	if (i + 1 >= size)
		return 0;
	if ((data[i] != '*' && data[i] != '+' && data[i] != '-') || data[i + 1] != ' ')
		return 0;
	if (is_next_headerline(data + i, size - i))
		return 0;
	return i + 2;
}

/* 有序列表前缀 */
static size_t prefix_oli(const char *data, size_t size)
{
	size_t i = 0;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == ' ') i++;
	if (i < size && data[i] == ' ') i++;
	if (i >= size || !md_isalnum(data[i]))
		return 0;
	while (i < size && md_isalnum(data[i]))
		i++;
	if (i + 1 >= size || data[i] != '.' || data[i + 1] != ' ')
		return 0;
	if (is_next_headerline(data + i, size - i))
		return 0;
	return i + 2;
}

/* 围栏代码块起始 */
static size_t is_codefence(const char *data, size_t size, struct buf *syntax)
{
	size_t i = 0, syn_len = 0;
	char c;

	if (size < 3)
		return 0;
	c = data[0];
	if (c != '~' && c != '`')
		return 0;

	i = 1;
	while (i < size && data[i] == c)
		i++;
	if (i < 3)
		return 0;

	if (syntax) {
		size_t start;
		while (i < size && data[i] == ' ')
			i++;
		start = i;
		while (i < size && !md_isspace((unsigned char)data[i])) {
			i++;
			syn_len++;
		}
		buf_put(syntax, data + start, syn_len);
	}

	while (i < size && data[i] != '\n') {
		if (!md_isspace((unsigned char)data[i]))
			return 0;
		i++;
	}
	return i + 1;
}

/* ================================================================== */
/* 块级解析                                                            */
/* ================================================================== */

static size_t parse_atxheader(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t level = 0, i, end, skip;
	struct buf work;

	while (level < size && level < 6 && data[level] == '#')
		level++;

	for (i = level; i < size && data[i] == ' '; i++);

	if ((rndr->ext_flags & MKDEXT_SPACE_HEADERS) && i == level)
		return 0;

	for (end = i; end < size && data[end] != '\n'; end++)
		;
	skip = end;

	while (end && data[end - 1] == '#')
		end--;
	while (end && data[end - 1] == ' ')
		end--;

	if (level > 6)
		return 0;

	buf_init(&work);
	parse_inline(&work, rndr, data + i, end - i);
	if (rndr->cb.header)
		rndr->cb.header(ob, &work, (int)level, rndr->options);
	else if (work.size)
		buf_put(ob, work.data, work.size);
	free(work.data);

	return skip;
}

static size_t parse_fencedcode(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t beg, end;
	struct buf work, lang;

	buf_init(&work);
	buf_init(&lang);

	beg = is_codefence(data, size, &lang);
	if (!beg) {
		free(work.data);
		free(lang.data);
		return 0;
	}

	while (beg < size) {
		size_t fence_end;
		struct buf fence_trail;

		buf_init(&fence_trail);
		fence_end = is_codefence(data + beg, size - beg, &fence_trail);
		if (fence_end != 0 && fence_trail.size == 0) {
			beg += fence_end;
			free(fence_trail.data);
			break;
		}
		free(fence_trail.data);

		for (end = beg + 1; end < size && data[end - 1] != '\n'; end++)
			;
		if (beg < end) {
			if (is_empty(data + beg, end - beg))
				buf_putc(&work, '\n');
			else
				buf_put(&work, data + beg, end - beg);
		}
		beg = end;
	}

	if (work.size && work.data[work.size - 1] != '\n')
		buf_putc(&work, '\n');

	if (rndr->cb.blockcode)
		rndr->cb.blockcode(ob, &work, lang.size ? &lang : NULL, rndr->options);

	free(work.data);
	free(lang.data);
	return beg;
}

static size_t parse_blockcode(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t beg = 0, end, pre;
	struct buf work;

	buf_init(&work);

	while (beg < size) {
		for (end = beg + 1; end < size && data[end - 1] != '\n'; end++)
			;
		pre = prefix_code(data + beg, end - beg);
		if (pre)
			beg += pre;
		else if (!is_empty(data + beg, end - beg))
			break;

		if (beg < end) {
			if (is_empty(data + beg, end - beg))
				buf_putc(&work, '\n');
			else
				buf_put(&work, data + beg, end - beg);
		}
		beg = end;
	}

	buf_trim_eol(&work);
	buf_putc(&work, '\n');

	if (rndr->cb.blockcode)
		rndr->cb.blockcode(ob, &work, NULL, rndr->options);

	free(work.data);
	return beg;
}

static size_t parse_blockquote(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t beg = 0, end = 0, pre;
	struct buf out, work;

	buf_init(&out);
	buf_init(&work);

	while (beg < size) {
		for (end = beg + 1; end < size && data[end - 1] != '\n'; end++)
			;

		pre = prefix_quote(data + beg, end - beg);
		if (pre)
			beg += pre;
		else if (is_empty(data + beg, end - beg) &&
			(end >= size || (prefix_quote(data + end, size - end) == 0 &&
				!is_empty(data + end, size - end))))
			break;

		if (beg < end)
			buf_put(&work, data + beg, end - beg);
		beg = end;
	}

	parse_block(&out, rndr, work.data ? work.data : (char *)"", work.size);
	if (rndr->cb.blockquote)
		rndr->cb.blockquote(ob, &out, rndr->options);

	free(out.data);
	free(work.data);
	return end;
}

static void parse_table_row(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size,
	int *columns, int ncols, int header)
{
	size_t i = 0, col;
	struct buf row;

	buf_init(&row);

	if (i < size && data[i] == '|')
		i++;

	for (col = 0; col < (size_t)ncols && i < size; col++) {
		size_t cell_start, cell_end;
		struct buf cell;

		while (i < size && md_isspace((unsigned char)data[i]))
			i++;

		cell_start = i;
		while (i < size && data[i] != '|')
			i++;
		cell_end = i ? i - 1 : 0;

		while (cell_end > cell_start && md_isspace((unsigned char)data[cell_end]))
			cell_end--;

		buf_init(&cell);
		parse_inline(&cell, rndr, data + cell_start, (cell_end >= cell_start) ? cell_end - cell_start + 1 : 0);
		if (rndr->cb.table_cell)
			rndr->cb.table_cell(&row, &cell, columns[col] | (header ? MKD_TABLE_HEADER : 0), rndr->options);
		free(cell.data);

		i++;
	}

	for (; col < (size_t)ncols; col++) {
		struct buf empty;
		buf_init(&empty);
		if (rndr->cb.table_cell)
			rndr->cb.table_cell(&row, &empty, columns[col] | (header ? MKD_TABLE_HEADER : 0), rndr->options);
		free(empty.data);
	}

	if (rndr->cb.table_row)
		rndr->cb.table_row(ob, &row, rndr->options);

	free(row.data);
}

static size_t parse_table(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t i = 0, header_end, under_end, pipes = 0;
	int columns[64];
	int ncols = 0;
	size_t col;
	struct buf header_work, body_work;

	/* 表头行：统计列数 */
	while (i < size && data[i] != '\n') {
		if (data[i] == '|')
			pipes++;
		i++;
	}
	if (i == size || pipes == 0)
		return 0;

	header_end = i;
	while (header_end > 0 && md_isspace((unsigned char)data[header_end - 1]))
		header_end--;

	ncols = (int)pipes + 1;
	if (data[0] == '|')
		ncols--;
	if (header_end && data[header_end - 1] == '|')
		ncols--;

	if (ncols > 64)
		ncols = 64;
	if (ncols <= 0)
		return 0;
	pipes = (size_t)ncols;

	for (col = 0; col < pipes; col++)
		columns[col] = 0;

	/* 分隔行 */
	i++;
	if (i < size && data[i] == '|')
		i++;

	under_end = i;
	while (under_end < size && data[under_end] != '\n')
		under_end++;

	for (col = 0; col < pipes && i < under_end; col++) {
		size_t dashes = 0;

		while (i < under_end && data[i] == ' ')
			i++;

		if (i < under_end && data[i] == ':') {
			i++;
			columns[col] |= MKD_TABLE_ALIGN_L;
			dashes++;
		}
		while (i < under_end && data[i] == '-') {
			i++;
			dashes++;
		}
		if (i < under_end && data[i] == ':') {
			i++;
			columns[col] |= MKD_TABLE_ALIGN_R;
			dashes++;
		}
		while (i < under_end && data[i] == ' ')
			i++;

		if (i < under_end && data[i] != '|')
			break;
		if (dashes < 3)
			break;
		i++;
	}

	if (col < pipes)
		return 0;

	/* 正文行从分隔行之后开始 */
	i = under_end;
	if (i < size)
		i++;

	buf_init(&header_work);
	buf_init(&body_work);

	parse_table_row(&header_work, rndr, data, header_end, columns, ncols, 1);

	while (i < size) {
		size_t row_start = i;
		size_t p = 0;

		while (i < size && data[i] != '\n') {
			if (data[i] == '|')
				p++;
			i++;
		}
		if (p == 0 || i == size) {
			i = row_start;
			break;
		}
		parse_table_row(&body_work, rndr, data + row_start, i - row_start, columns, ncols, 0);
		i++;
	}

	if (rndr->cb.table)
		rndr->cb.table(ob, &header_work, &body_work, rndr->options);

	free(header_work.data);
	free(body_work.data);
	return i;
}

static size_t parse_listitem(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size, int *flags)
{
	struct buf work, inter;
	size_t beg = 0, end, pre, sublist = 0, orgpre = 0, i;
	int in_empty = 0, has_inside_empty = 0, in_fence = 0;
	int task = 0, checked = 0;

	while (orgpre < 3 && orgpre < size && data[orgpre] == ' ')
		orgpre++;

	beg = prefix_uli(data, size);
	if (!beg)
		beg = prefix_oli(data, size);
	if (!beg)
		return 0;

	end = beg;
	while (end < size && data[end - 1] != '\n')
		end++;

	buf_init(&work);
	buf_init(&inter);

	buf_put(&work, data + beg, end - beg);
	beg = end;

	/* 任务列表标记：[ ] / [x]
	   官方行为：把标记替换成复选框 HTML 并作为内容的一部分参与行内解析，
	   因此松散列表时复选框落在 <p> 内部。
	   必须在首行入缓冲后立刻判定（此时缓冲里只有本项的首行内容）。 */
	if (work.size >= 4 && work.data[0] == '[' && work.data[2] == ']' && work.data[3] == ' ') {
		char c = work.data[1];
		if (c == ' ' || c == 'x' || c == 'X') {
			const char *html;
			struct buf nw;

			task = 1;
			checked = (c == 'x' || c == 'X') ? 1 : 0;

			if (rndr->options->flags & HTML_TASK_ENABLED)
				html = checked ? "<input type=\"checkbox\" checked>" : "<input type=\"checkbox\">";
			else
				html = checked ? "<input type=\"checkbox\" checked disabled>" : "<input type=\"checkbox\" disabled>";

			buf_init(&nw);
			buf_puts(&nw, html);
			buf_put(&nw, work.data + 3, work.size - 3);
			free(work.data);
			work = nw;
		}
	}

	while (beg < size) {
		size_t has_next_uli = 0, has_next_oli = 0;

		end++;
		while (end < size && data[end - 1] != '\n')
			end++;

		if (is_empty(data + beg, end - beg)) {
			in_empty = 1;
			beg = end;
			continue;
		}

		i = 0;
		while (i < 4 && beg + i < end && data[beg + i] == ' ')
			i++;
		pre = i;

		if (rndr->ext_flags & MKDEXT_FENCED_CODE) {
			struct buf tmp;
			buf_init(&tmp);
			if (is_codefence(data + beg + i, end - beg - i, &tmp) != 0)
				in_fence = !in_fence;
			free(tmp.data);
		}

		if (!in_fence) {
			has_next_uli = prefix_uli(data + beg + i, end - beg - i);
			has_next_oli = prefix_oli(data + beg + i, end - beg - i);
		}

		if (in_empty && (((*flags & MKD_LIST_ORDERED) && has_next_uli) ||
			(!(*flags & MKD_LIST_ORDERED) && has_next_oli))) {
			*flags |= MKD_LI_END;
			break;
		}

		if ((has_next_uli && !is_hrule(data + beg + i, end - beg - i)) || has_next_oli) {
			if (in_empty)
				has_inside_empty = 1;

			if (pre == orgpre)
				break;

			if (!sublist)
				sublist = work.size;
		}
		else if (in_empty && pre == 0) {
			*flags |= MKD_LI_END;
			break;
		}
		else if (in_empty) {
			buf_putc(&work, '\n');
			has_inside_empty = 1;
		}

		in_empty = 0;
		buf_put(&work, data + beg + i, end - beg - i);
		beg = end;
	}

	/* 任务列表标记已在首行入缓冲时处理（见上） */

	if (has_inside_empty)
		*flags |= MKD_LI_BLOCK;

	if (*flags & MKD_LI_BLOCK) {
		if (sublist && sublist < work.size) {
			parse_block(&inter, rndr, work.data, sublist);
			parse_block(&inter, rndr, work.data + sublist, work.size - sublist);
		}
		else
			parse_block(&inter, rndr, work.data, work.size);
	}
	else {
		if (sublist && sublist < work.size) {
			parse_inline(&inter, rndr, work.data, sublist);
			parse_block(&inter, rndr, work.data + sublist, work.size - sublist);
		}
		else
			parse_inline(&inter, rndr, work.data, work.size);
	}

	if (task) {
		*flags |= MKD_LI_TASK;
		if (checked)
			*flags |= MKD_LI_CHECKED;
	}

	if (rndr->cb.listitem)
		rndr->cb.listitem(ob, &inter, *flags, rndr->options);

	free(work.data);
	free(inter.data);
	return beg;
}

static size_t parse_list(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size, int flags)
{
	struct buf work;
	size_t i = 0, j;

	buf_init(&work);

	while (i < size) {
		j = parse_listitem(&work, rndr, data + i, size - i, &flags);
		i += j;
		if (!j || (flags & MKD_LI_END))
			break;
	}

	if (rndr->cb.list)
		rndr->cb.list(ob, &work, flags, rndr->options);

	free(work.data);
	return i;
}

/* 段落的内联渲染；返回消耗的字节数 */
static size_t parse_paragraph(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t i = 0, end = 0;
	int level = 0;
	struct buf work;

	buf_init(&work);

	while (i < size) {
		for (end = i + 1; end < size && data[end - 1] != '\n'; end++)
			;

		if (is_empty(data + i, size - i))
			break;

		if ((level = is_headerline(data + i, size - i)) != 0)
			break;

		if (is_atxheader(rndr, data + i, size - i) ||
			is_hrule(data + i, size - i) ||
			prefix_quote(data + i, size - i)) {
			end = i;
			break;
		}

		if (rndr->ext_flags & MKDEXT_LAX_SPACING) {
			if (prefix_uli(data + i, size - i) || prefix_oli(data + i, size - i)) {
				end = i;
				break;
			}
		}

		i = end;
	}

	if (!level) {
		/* 段落正文必须先做行内解析（强调/链接/转义都在这一步） */
		struct buf inline_work;
		buf_init(&inline_work);
		parse_inline(&inline_work, rndr, data, i);
		if (rndr->cb.paragraph)
			rndr->cb.paragraph(ob, &inline_work, rndr->options);
		free(inline_work.data);
		free(work.data);
		return end ? end : size;
	}

	/* setext 标题：work 内容为标题文本 */
	{
		size_t beg = 0, tlen;
		tlen = i;
		while (tlen > 0 && data[tlen - 1] == '\n')
			tlen--;
		beg = 0;
		while (beg < tlen && md_isspace((unsigned char)data[beg]))
			beg++;

		buf_init(&work);
		if (tlen > beg) {
			struct buf inline_work;
			buf_init(&inline_work);
			parse_inline(&inline_work, rndr, data + beg, tlen - beg);
			if (rndr->cb.header)
				rndr->cb.header(ob, &inline_work, level, rndr->options);
			else if (inline_work.size)
				buf_put(ob, inline_work.data, inline_work.size);
			free(inline_work.data);
		}
		free(work.data);
	}

	return end;
}

/* HTML 块级标签 */
static const char *block_tags[] = {
	"p", "dl", "div", "math", "table", "ul", "del", "form", "blockquote",
	"figure", "ol", "fieldset", "h1", "h2", "h3", "h4", "h5", "h6",
	"pre", "script", "style", "iframe", "ins", "address", "article",
	"aside", "audio", "canvas", "figcaption", "footer", "header",
	"hgroup", "output", "section", "video", "noscript", "main",
	"nav", "details", "summary", "center", "dir", "menu", "isindex",
	NULL
};

static const char *find_block_tag(const char *data, size_t size)
{
	size_t i, j;
	for (i = 0; i < size; i++) {
		for (j = 0; block_tags[j]; j++) {
			size_t tl = strlen(block_tags[j]);
			if (tl <= size - i && md_strnicmp(data + i, block_tags[j], tl) == 0) {
				if (i + tl >= size)
					return block_tags[j];
				if (data[i + tl] == '>' || data[i + tl] == ' ' || data[i + tl] == '\n' ||
					data[i + tl] == '\t' || data[i + tl] == '/')
					return block_tags[j];
			}
		}
	}
	return NULL;
}

/* 查找 HTML 块的结束位置。
   start_of_line=1 时只认行首出现的结束标签（上游的第一轮查找），
   0 时不限制位置（第二轮查找）。 */
static size_t htmlblock_end(const char *curtag, struct sd_markdown *rndr, char *data, size_t size, int start_of_line)
{
	size_t tag_size = strlen(curtag);
	size_t i = 1, end_tag;
	int block_lines = 0;

	(void)rndr;

	while (i < size) {
		i++;
		while (i < size && !(data[i - 1] == '<' && data[i] == '/')) {
			if (data[i] == '\n')
				block_lines++;
			i++;
		}

		if (start_of_line && block_lines == 0)
			continue;

		if (i + 1 + tag_size >= size)
			break;

		end_tag = 0;
		if (data[i + 1] == '/')
			end_tag = i + 2;
		else if (data[i] == '/')
			end_tag = i + 1;

		if (end_tag && md_strnicmp(data + end_tag, curtag, tag_size) == 0 &&
			data[end_tag + tag_size] == '>')
			return end_tag + tag_size + 1;
	}

	return 0;
}

static size_t parse_htmlblock(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t i = 1, tag_end;
	const char *tag;
	struct buf work;

	if (size < 2 || data[0] != '<')
		return 0;

	while (i < size && data[i] != '>' && data[i] != ' ' && data[i] != '\n')
		i++;

	tag = (i < size) ? find_block_tag(data + 1, i - 1) : NULL;
	if (!tag) {
		/* HTML 注释 */
		if (size > 5 && data[1] == '!' && data[2] == '-' && data[3] == '-') {
			size_t k = 5;
			while (k < size && !(data[k - 2] == '-' && data[k - 1] == '-' && data[k] == '>'))
				k++;
			k++;
			if (k <= size) {
				size_t j = is_empty(data + k, size - k);
				if (j) {
					buf_init(&work);
					buf_put(&work, data, k + j);
					if (rndr->cb.blockhtml)
						rndr->cb.blockhtml(ob, &work, rndr->options);
					free(work.data);
					return k + j;
				}
			}
		}
		return 0;
	}

	tag_end = htmlblock_end(tag, rndr, data, size, 1);
	if (!tag_end && strcmp(tag, "ins") != 0 && strcmp(tag, "del") != 0)
		tag_end = htmlblock_end(tag, rndr, data, size, 0);
	if (!tag_end)
		return 0;

	buf_init(&work);
	buf_put(&work, data, tag_end);
	if (rndr->cb.blockhtml)
		rndr->cb.blockhtml(ob, &work, rndr->options);
	free(work.data);
	return tag_end;
}

static void parse_block(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t beg = 0;

	if (rndr->depth++ > (int)rndr->max_nesting) {
		rndr->depth--;
		return;
	}

	while (beg < size) {
		char *txt = data + beg;
		size_t end = size - beg;
		size_t i;

		if (is_atxheader(rndr, txt, end)) {
			beg += parse_atxheader(ob, rndr, txt, end);
			continue;
		}

		if (txt[0] == '<' && rndr->cb.blockhtml) {
			i = parse_htmlblock(ob, rndr, txt, end);
			if (i) {
				beg += i;
				continue;
			}
		}

		i = is_empty(txt, end);
		if (i) {
			beg += i;
			continue;
		}

		if (is_hrule(txt, end)) {
			if (rndr->cb.hrule)
				rndr->cb.hrule(ob, rndr->options);
			while (beg < size && data[beg] != '\n')
				beg++;
			beg++;
			continue;
		}

		if (rndr->ext_flags & MKDEXT_FENCED_CODE) {
			i = parse_fencedcode(ob, rndr, txt, end);
			if (i) {
				beg += i;
				continue;
			}
		}

		if (rndr->ext_flags & MKDEXT_TABLES) {
			i = parse_table(ob, rndr, txt, end);
			if (i) {
				beg += i;
				continue;
			}
		}

		if (prefix_quote(txt, end)) {
			beg += parse_blockquote(ob, rndr, txt, end);
			continue;
		}

		if (prefix_code(txt, end)) {
			beg += parse_blockcode(ob, rndr, txt, end);
			continue;
		}

		if (prefix_uli(txt, end)) {
			beg += parse_list(ob, rndr, txt, end, 0);
			continue;
		}

		if (prefix_oli(txt, end)) {
			beg += parse_list(ob, rndr, txt, end, MKD_LIST_ORDERED);
			continue;
		}

		i = parse_paragraph(ob, rndr, txt, end);
		beg += i ? i : end;
	}

	rndr->depth--;
}

/* ================================================================== */
/* 行内解析                                                            */
/* ================================================================== */

/* 去掉 URL 中的反斜杠转义 */
static void unscape_text(struct buf *ob, const char *src, size_t len)
{
	size_t i = 0, org;
	while (i < len) {
		org = i;
		while (i < len && src[i] != '\\')
			i++;
		if (i > org)
			buf_put(ob, src + org, i - org);
		if (i + 1 >= len)
			break;
		buf_putc(ob, src[i + 1]);
		i += 2;
	}
}

/* 查找下一个强调标记 */
static size_t find_emph_char(const char *data, size_t size, char c)
{
	size_t i = 1;

	while (i < size) {
		while (i < size && data[i] != c && data[i] != '`' && data[i] != '[')
			i++;

		if (i == size)
			return 0;

		if (data[i] == c)
			return i;

		if (data[i] == '`') {
			size_t span_nb = 0, bt;
			size_t tmp_i = 0;
			while (i < size && data[i] == '`') {
				i++;
				span_nb++;
			}
			if (i >= size)
				return 0;
			bt = 0;
			while (i < size && bt < span_nb) {
				if (!tmp_i && data[i] == c)
					tmp_i = i;
				if (data[i] == '`')
					bt++;
				else
					bt = 0;
				i++;
			}
			if (i >= size)
				return tmp_i;
		}
		else {
			size_t tmp_i = 0;
			char cc;
			i++;
			while (i < size && data[i] != ']') {
				if (!tmp_i && data[i] == c)
					tmp_i = i;
				i++;
			}
			i++;
			while (i < size && (data[i] == ' ' || data[i] == '\n'))
				i++;
			if (i >= size)
				return tmp_i;
			switch (data[i]) {
			case '[': cc = ']'; break;
			case '(': cc = ')'; break;
			default:
				if (tmp_i)
					return tmp_i;
				continue;
			}
			while (i < size && data[i] != cc) {
				if (!tmp_i && data[i] == c)
					tmp_i = i;
				i++;
			}
			if (i >= size)
				return tmp_i;
			i++;
		}
	}
	return 0;
}

static size_t parse_emph1(struct buf *ob, struct sd_markdown *rndr, const char *data,
	size_t size, char c, size_t offset)
{
	size_t i = 0, len;
	struct buf work;
	int r;

	if (!rndr->cb.emphasis)
		return 0;

	if (size > 1 && data[0] == c && data[1] == c)
		i = 1;

	while (i < size) {
		len = find_emph_char(data + i, size - i, c);
		if (!len)
			return 0;
		i += len;
		if (i >= size)
			return 0;

		if (data[i] == c && !md_isspace((unsigned char)data[i - 1])) {
			if ((rndr->ext_flags & MKDEXT_NO_INTRA_EMPHASIS)) {
				if (offset + i + 1 < size && md_isalnum((unsigned char)data[i + 1]))
					continue;
			}
			buf_init(&work);
			parse_inline(&work, rndr, (char *)data, i);
			r = rndr->cb.emphasis(ob, &work, rndr->options);
			free(work.data);
			return r ? i + 1 : 0;
		}
	}
	return 0;
}

static size_t parse_emph2(struct buf *ob, struct sd_markdown *rndr, const char *data,
	size_t size, char c)
{
	size_t i = 0, len;
	struct buf work;
	int r;

	if (!rndr->cb.double_emphasis)
		return 0;

	while (i < size) {
		len = find_emph_char(data + i, size - i, c);
		if (!len)
			return 0;
		i += len;

		if (i + 1 < size && data[i] == c && data[i + 1] == c && i && !md_isspace((unsigned char)data[i - 1])) {
			buf_init(&work);
			parse_inline(&work, rndr, (char *)data, i);
			r = rndr->cb.double_emphasis(ob, &work, rndr->options);
			free(work.data);
			return r ? i + 2 : 0;
		}
		i++;
	}
	return 0;
}

static size_t parse_emph3(struct buf *ob, struct sd_markdown *rndr, const char *data,
	size_t size, char c)
{
	size_t i = 0, len;
	int r;

	while (i < size) {
		len = find_emph_char(data + i, size - i, c);
		if (!len)
			return 0;
		i += len;

		if (data[i] != c || md_isspace((unsigned char)data[i - 1]))
			continue;

		if (i + 2 < size && data[i + 1] == c && data[i + 2] == c && rndr->cb.triple_emphasis) {
			struct buf work;
			buf_init(&work);
			parse_inline(&work, rndr, (char *)data, i);
			r = rndr->cb.triple_emphasis(ob, &work, rndr->options);
			free(work.data);
			return r ? i + 3 : 0;
		}
		else if (i + 1 < size && data[i + 1] == c) {
			len = parse_emph1(ob, rndr, data - 2, size + 2, c, 0);
			if (!len)
				return 0;
			return len - 2;
		}
		else {
			len = parse_emph2(ob, rndr, data - 1, size + 1, c);
			if (!len)
				return 0;
			return len - 1;
		}
	}
	return 0;
}

static size_t char_emphasis(struct buf *ob, struct sd_markdown *rndr, char *data,
	size_t offset, size_t size)
{
	char c = data[0];
	size_t ret;

	if (rndr->ext_flags & MKDEXT_NO_INTRA_EMPHASIS) {
		if (offset > 0 && !md_isspace((unsigned char)data[-1]) && data[-1] != '>' && data[-1] != '(')
			return 0;
	}

	if (size > 2 && data[1] != c) {
		if (c == '~' || c == '^' || md_isspace((unsigned char)data[1]) ||
			(ret = parse_emph1(ob, rndr, data + 1, size - 1, c, offset + 1)) == 0)
			return 0;
		return ret + 1;
	}

	if (size > 3 && data[1] == c && data[2] != c) {
		if (md_isspace((unsigned char)data[2]) ||
			(ret = parse_emph2(ob, rndr, data + 2, size - 2, c)) == 0)
			return 0;
		return ret + 2;
	}

	if (size > 4 && data[1] == c && data[2] == c && data[3] != c) {
		if (c == '~' || c == '^' || md_isspace((unsigned char)data[3]) ||
			(ret = parse_emph3(ob, rndr, data + 3, size - 3, c)) == 0)
			return 0;
		return ret + 3;
	}

	return 0;
}

static size_t char_codespan(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	size_t end, nb = 0, i, f_begin, f_end;
	struct buf work;

	(void)offset;

	while (nb < size && data[nb] == '`')
		nb++;

	i = 0;
	for (end = nb; end < size && i < nb; end++) {
		if (data[end] == '`')
			i++;
		else
			i = 0;
	}

	if (i < nb && end >= size)
		return 0;

	f_begin = nb;
	while (f_begin < end && data[f_begin] == ' ')
		f_begin++;

	f_end = end - nb;
	while (f_end > nb && data[f_end - 1] == ' ')
		f_end--;

	buf_init(&work);
	if (f_begin < f_end) {
		buf_put(&work, data + f_begin, f_end - f_begin);
		if (!rndr->cb.codespan || !rndr->cb.codespan(ob, &work, rndr->options)) {
			free(work.data);
			return 0;
		}
	}
	else {
		if (!rndr->cb.codespan || !rndr->cb.codespan(ob, &work, rndr->options)) {
			free(work.data);
			return 0;
		}
	}
	free(work.data);
	return end;
}

static size_t char_escape(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	static const char *escape_chars = "\\`*_{}[]()#+-.!:|&<>^~";
	struct buf work;

	(void)offset;

	buf_init(&work);
	if (size > 1) {
		if (strchr(escape_chars, data[1]) == NULL) {
			free(work.data);
			return 0;
		}
		if (rndr->cb.normal_text) {
			/* 输出被转义的字符本身（去掉反斜杠） */
			buf_put(&work, data + 1, 1);
			rndr->cb.normal_text(ob, &work, rndr->options);
		}
		else {
			buf_putc(ob, data[1]);
		}
	}
	else if (size == 1) {
		buf_putc(ob, data[0]);
	}
	free(work.data);
	/* 反斜杠 + 被转义的字符，共 2 个字节 */
	return 2;
}

static size_t char_entity(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	size_t end = 1;
	struct buf work;

	(void)offset;

	if (end < size && data[end] == '#')
		end++;
	while (end < size && md_isalnum((unsigned char)data[end]))
		end++;
	if (end < size && data[end] == ';')
		end++;
	else
		return 0;

	buf_init(&work);
	buf_put(&work, data, end);
	if (rndr->cb.entity)
		rndr->cb.entity(ob, &work, rndr->options);
	else
		buf_put(ob, data, end);
	free(work.data);
	return end;
}

static size_t char_linebreak(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	(void)size;
	if (offset < 2 || data[-1] != ' ' || data[-2] != ' ')
		return 0;

	while (ob->size && ob->data[ob->size - 1] == ' ')
		ob->size--;
	if (ob->data)
		ob->data[ob->size] = 0;

	return (rndr->cb.linebreak && rndr->cb.linebreak(ob, rndr->options)) ? 1 : 0;
}

/* 判定 <...>：返回标签总长度，autolink 类型写入 *autolink，
   自动链接正文范围写入 body_b / body_e */
static size_t tag_length(const char *data, size_t size, int *autolink,
	size_t *body_b, size_t *body_e)
{
	size_t i, j;

	*autolink = MKDA_NOT_AUTOLINK;
	*body_b = *body_e = 0;

	/* 合法标签至少 3 个字符 */
	if (size < 3 || data[0] != '<')
		return 0;

	i = (data[1] == '/') ? 2 : 1;
	if (!md_isalnum((unsigned char)data[i]))
		return 0;

	while (i < size && (md_isalnum((unsigned char)data[i]) || data[i] == '.' ||
		data[i] == '+' || data[i] == '-'))
		i++;

	/* <user@host> 形式的邮件 */
	if (i > 1 && i < size && data[i] == '@') {
		size_t nb = 0;
		j = i;
		while (j < size) {
			char c = data[j];
			if (md_isalnum((unsigned char)c)) {
				j++;
				continue;
			}
			if (c == '@') {
				nb++;
				j++;
				continue;
			}
			if (c == '-' || c == '.' || c == '_') {
				j++;
				continue;
			}
			break;
		}
		if (nb == 1 && j < size && data[j] == '>' && j > i + 1) {
			*autolink = MKDA_EMAIL;
			*body_b = 1;
			*body_e = j;
			return j + 1;
		}
	}

	/* <http://...> 形式的链接 */
	if (i > 2 && i < size && data[i] == ':') {
		*autolink = MKDA_NORMAL;
		i++;
		j = i;
		while (i < size) {
			if (data[i] == '\\')
				i += 2;
			else if (data[i] == '>' || data[i] == '\'' || data[i] == '"' ||
				data[i] == ' ' || data[i] == '\n')
				break;
			else
				i++;
		}
		if (i < size && i > j && data[i] == '>') {
			*body_b = 1;
			*body_e = i;
			return i + 1;
		}
		*autolink = MKDA_NOT_AUTOLINK;
	}

	/* 普通标签：找 '>' */
	while (i < size && data[i] != '>')
		i++;
	if (i >= size)
		return 0;
	return i + 1;
}

/* <...> 自动链接或原始 HTML 标签 */
static size_t char_langle_tag(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	size_t end, body_b = 0, body_e = 0;
	int altype = MKDA_NOT_AUTOLINK;
	struct buf work;

	(void)offset;

	end = tag_length(data, size, &altype, &body_b, &body_e);
	if (!end)
		return 0;

	if (altype != MKDA_NOT_AUTOLINK && rndr->cb.utolink) {
		struct buf link;
		buf_init(&link);
		buf_put(&link, data + body_b, body_e - body_b);
		rndr->cb.utolink(ob, &link, altype, rndr->options);
		free(link.data);
		return end;
	}

	if (rndr->cb.raw_html_tag) {
		buf_init(&work);
		buf_put(&work, data, end);
		rndr->cb.raw_html_tag(ob, &work, rndr->options);
		free(work.data);
		return end;
	}

	return 0;
}

/* 裸链接：网址（www. / http:// ）与裸邮箱。
   触发字符：':'（网址 scheme 的冒号）、'w'（www.）、'@'（邮箱）。
   网址/邮箱的起始部分可能已经作为普通文本写进 ob，需要回退。 */
static size_t char_autolink(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	size_t j;
	struct buf link;

	if (!rndr->cb.utolink || rndr->in_link_body)
		return 0;

	/* ---- www.xxx ---- */
	if (data[0] == 'w' && size >= 4 && md_strnicmp(data, "www.", 4) == 0) {
		if (offset > 0) {
			char p = data[-1];
			if (md_isalnum((unsigned char)p) || p == '/' || p == '.' || p == '@' ||
				p == ':' || p == '-' || p == '_')
				return 0;
		}
		j = 4;
		while (j < size && !md_isspace((unsigned char)data[j]) && data[j] != '>' &&
			data[j] != '"' && data[j] != '\'' && data[j] != '<' &&
			data[j] != ',' && data[j] != ';' && data[j] != ')' && data[j] != ']')
			j++;
		while (j > 4 && (data[j - 1] == '.' || data[j - 1] == ',' || data[j - 1] == ';' ||
			data[j - 1] == ':' || data[j - 1] == '!' || data[j - 1] == '?'))
			j--;
		if (j <= 4)
			return 0;

		buf_init(&link);
		buf_put(&link, data, j);
		/* www. 的 href 由渲染回调补 http:// 前缀，显示文本保持 www. 原样 */
		rndr->cb.utolink(ob, &link, MKDA_NORMAL, rndr->options);
		free(link.data);
		return j;
	}

	/* ---- http:// https:// ftp:// （触发字符是 ':'） ---- */
	if (data[0] == ':') {
		int wordlen = 0;
		size_t rewind;
		char word[8];

		while (wordlen < (int)offset && md_isalnum((unsigned char)data[-1 - wordlen])) {
			wordlen++;
			if (wordlen >= (int)sizeof(word))
				return 0;
		}
		if (wordlen == 0)
			return 0;

		memcpy(word, data - wordlen, (size_t)wordlen);
		word[wordlen] = 0;
		{
			int q;
			for (q = 0; q < wordlen; q++)
				word[q] = (char)md_tolower(word[q]);
		}
		if (strcmp(word, "http") != 0 && strcmp(word, "https") != 0 && strcmp(word, "ftp") != 0)
			return 0;

		if (wordlen < (int)offset) {
			char p = data[-1 - wordlen];
			if (md_isalnum((unsigned char)p) || p == '/' || p == '.' || p == '@' ||
				p == ':' || p == '-' || p == '_')
				return 0;
		}
		if (size < 3 || data[1] != '/' || data[2] != '/')
			return 0;

		j = 3;
		while (j < size && !md_isspace((unsigned char)data[j]) && data[j] != '>' &&
			data[j] != '"' && data[j] != '\'' && data[j] != '<' &&
			data[j] != ',' && data[j] != ';' && data[j] != ')' && data[j] != ']')
			j++;
		while (j > 3 && (data[j - 1] == '.' || data[j - 1] == ',' || data[j - 1] == ';' ||
			data[j - 1] == ':' || data[j - 1] == '!' || data[j - 1] == '?'))
			j--;
		if (j <= 3)
			return 0;

		rewind = (size_t)wordlen;
		if (rewind <= ob->size)
			ob->size -= rewind;
		else
			ob->size = 0;
		if (ob->data)
			ob->data[ob->size] = 0;

		buf_init(&link);
		buf_put(&link, data - wordlen, (size_t)wordlen + j);
		rndr->cb.utolink(ob, &link, MKDA_NORMAL, rndr->options);
		free(link.data);
		return j;
	}

	/* ---- 裸邮箱（触发字符是 '@'） ---- */
	if (data[0] == '@') {
		int rewind = 0;
		int dots = 0;

		while (rewind < (int)offset) {
			char p = data[-1 - rewind];
			if (md_isalnum((unsigned char)p) || p == '.' || p == '+' || p == '-' || p == '_')
				rewind++;
			else
				break;
		}
		if (rewind == 0)
			return 0;

		j = 1;
		while (j < size) {
			char c = data[j];
			if (md_isalnum((unsigned char)c)) {
				j++;
				continue;
			}
			if (c == '.') {
				dots++;
				j++;
				continue;
			}
			if (c == '-' || c == '_') {
				j++;
				continue;
			}
			break;
		}
		while (j > 1 && (data[j - 1] == '.' || data[j - 1] == '-' || data[j - 1] == '_'))
			j--;
		if (dots == 0 || j < 2)
			return 0;

		if ((size_t)rewind <= ob->size)
			ob->size -= (size_t)rewind;
		else
			ob->size = 0;
		if (ob->data)
			ob->data[ob->size] = 0;

		buf_init(&link);
		buf_put(&link, data - rewind, (size_t)rewind + j);
		rndr->cb.utolink(ob, &link, MKDA_EMAIL, rndr->options);
		free(link.data);
		return j;
	}

	return 0;
}

static size_t char_link(struct buf *ob, struct sd_markdown *rndr, char *data, size_t offset, size_t size)
{
	int is_img = (offset && data[-1] == '!');
	int level;
	size_t i = 1, txt_e, link_b = 0, link_e = 0, title_b = 0, title_e = 0;
	struct buf content, link, title, u_link;
	int have_link = 0, have_title = 0;
	int text_has_nl = 0, ret = 0;
	int in_title = 0, qtype = 0;

	buf_init(&content);
	buf_init(&link);
	buf_init(&title);
	buf_init(&u_link);

	if ((is_img && !rndr->cb.image) || (!is_img && !rndr->cb.link))
		return 0;

	for (level = 1; i < size; i++) {
		if (data[i] == '\n')
			text_has_nl = 1;
		else if (data[i - 1] == '\\')
			continue;
		else if (data[i] == '[')
			level++;
		else if (data[i] == ']') {
			level--;
			if (level <= 0)
				break;
		}
	}

	if (i >= size)
		return 0;

	txt_e = i;
	i++;

	while (i < size && md_isspace((unsigned char)data[i]))
		i++;

	if (i < size && data[i] == '(') {
		i++;
		while (i < size && md_isspace((unsigned char)data[i]))
			i++;

		link_b = i;
		level = 1;

		while (i < size) {
			if (data[i] == '\\')
				i += 2;
			else if (data[i] == '(')
				level++;
			else if (data[i] == ')') {
				level--;
				if (level <= 0)
					break;
			}
			else if (i >= 1 && md_isspace((unsigned char)data[i - 1]) &&
				(data[i] == '\'' || data[i] == '"'))
				break;
			i++;
		}

		if (i >= size)
			return 0;
		link_e = i;

		if (data[i] == '\'' || data[i] == '"') {
			qtype = data[i];
			in_title = 1;
			i++;
			title_b = i;

			while (i < size) {
				if (data[i] == '\\')
					i += 2;
				else if (data[i] == qtype) {
					in_title = 0;
					i++;
				}
				else if ((data[i] == ')') && !in_title)
					break;
				else
					i++;
			}

			if (i >= size)
				return 0;

			title_e = i - 1;
			while (title_e > title_b && md_isspace((unsigned char)data[title_e]))
				title_e--;

			if (data[title_e] != '\'' && data[title_e] != '"') {
				title_b = title_e = 0;
				link_e = i;
			}
		}

		while (link_e > link_b && md_isspace((unsigned char)data[link_e - 1]))
			link_e--;

		if (link_b < size && data[link_b] == '<')
			link_b++;
		if (link_e > link_b && data[link_e - 1] == '>')
			link_e--;

		if (link_e > link_b) {
			buf_put(&link, data + link_b, link_e - link_b);
			have_link = 1;
		}
		if (title_e > title_b) {
			buf_put(&title, data + title_b, title_e - title_b);
			have_title = 1;
		}
		i++;
	}
	else if (i < size && data[i] == '[') {
		struct buf id;
		struct link_ref *lr;

		buf_init(&id);
		i++;
		link_b = i;
		while (i < size && data[i] != ']')
			i++;
		if (i >= size) {
			free(id.data);
			return 0;
		}
		link_e = i;

		if (link_b == link_e) {
			if (text_has_nl) {
				size_t j;
				for (j = 1; j < txt_e; j++) {
					if (data[j] != '\n')
						buf_putc(&id, data[j]);
					else if (data[j - 1] != ' ')
						buf_putc(&id, ' ');
				}
			}
			else {
				buf_put(&id, data + 1, txt_e - 1);
			}
		}
		else {
			buf_put(&id, data + link_b, link_e - link_b);
		}

		lr = find_link_ref(rndr, id.data, id.size);
		free(id.data);
		if (!lr)
			return 0;

		buf_put(&link, lr->link.data, lr->link.size);
		have_link = lr->link.size > 0;
		buf_put(&title, lr->title.data, lr->title.size);
		have_title = lr->title.size > 0;
		i++;
	}
	else {
		struct buf id;
		struct link_ref *lr;

		buf_init(&id);
		if (text_has_nl) {
			size_t j;
			for (j = 1; j < txt_e; j++) {
				if (data[j] != '\n')
					buf_putc(&id, data[j]);
				else if (data[j - 1] != ' ')
					buf_putc(&id, ' ');
			}
		}
		else {
			buf_put(&id, data + 1, txt_e - 1);
		}

		lr = find_link_ref(rndr, id.data, id.size);
		free(id.data);
		if (!lr)
			return 0;

		buf_put(&link, lr->link.data, lr->link.size);
		have_link = lr->link.size > 0;
		buf_put(&title, lr->title.data, lr->title.size);
		have_title = lr->title.size > 0;
		i = txt_e + 1;
	}

	if (txt_e > 1) {
		if (is_img) {
			buf_put(&content, data + 1, txt_e - 1);
		}
		else {
			rndr->in_link_body = 1;
			parse_inline(&content, rndr, data + 1, txt_e - 1);
			rndr->in_link_body = 0;
		}
	}

	if (have_link) {
		unscape_text(&u_link, link.data, link.size);
	}

	if (is_img) {
		if (ob->size && ob->data[ob->size - 1] == '!') {
			ob->size--;
			if (ob->data)
				ob->data[ob->size] = 0;
		}
		ret = rndr->cb.image(ob, have_link ? &u_link : NULL, have_title ? &title : NULL,
			content.size ? &content : NULL, rndr->options);
	}
	else {
		ret = rndr->cb.link(ob, have_link ? &u_link : NULL, have_title ? &title : NULL,
			content.size ? &content : NULL, rndr->options);
	}

	free(content.data);
	free(link.data);
	free(title.data);
	free(u_link.data);
	return ret ? i : 0;
}

static size_t char_strikethrough(struct buf *ob, struct sd_markdown *rndr, char *data,
	size_t offset, size_t size)
{
	size_t i;
	struct buf work;
	int r;

	(void)offset;

	if (rndr->ext_flags & MKDEXT_STRIKETHROUGH) {
		if (size > 2 && data[1] == '~') {
			i = 2;
			while (i < size && data[i] != '~')
				i++;
			if (i + 1 < size && data[i + 1] == '~') {
				buf_init(&work);
				parse_inline(&work, rndr, data + 2, i - 2);
				r = rndr->cb.strikethrough ? rndr->cb.strikethrough(ob, &work, rndr->options) : 0;
				free(work.data);
				return r ? i + 2 : 0;
			}
		}
	}
	return 0;
}

static size_t char_superscript(struct buf *ob, struct sd_markdown *rndr, char *data,
	size_t offset, size_t size)
{
	size_t i;
	struct buf work;
	int r;

	(void)offset;

	if (rndr->ext_flags & MKDEXT_SUPERSCRIPT) {
		if (size > 2 && data[1] == '^') {
			i = 2;
			while (i < size && data[i] != '^')
				i++;
			if (i + 1 < size && data[i + 1] == '^') {
				buf_init(&work);
				parse_inline(&work, rndr, data + 2, i - 2);
				r = rndr->cb.superscript ? rndr->cb.superscript(ob, &work, rndr->options) : 0;
				free(work.data);
				return r ? i + 2 : 0;
			}
		}
	}
	return 0;
}

static void parse_inline(struct buf *ob, struct sd_markdown *rndr, char *data, size_t size)
{
	size_t i = 0, end = 0;
	int action;
	struct buf work;

	if (rndr->depth++ > (int)rndr->max_nesting) {
		rndr->depth--;
		return;
	}

	buf_init(&work);

	while (i < size) {
		action = 0;
		while (end < size) {
			char c = data[end];
			if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '~' || c == '^' ||
				c == '[' || c == '<' || c == '&' || c == '\n' ||
				c == ':' || c == '@' || c == 'w')
			{
				action = 1;
				break;
			}
			end++;
		}

		if (end > i) {
			if (rndr->cb.normal_text) {
				buf_put(&work, data + i, end - i);
				rndr->cb.normal_text(ob, &work, rndr->options);
			}
			else
				buf_put(ob, data + i, end - i);
			buf_reset(&work);
		}

		if (end >= size)
			break;
		i = end;

		{
			char c = data[i];
			size_t used = 0;

			switch (c) {
			case '\\':
				used = char_escape(ob, rndr, data + i, i, size - i);
				break;
			case '`':
				used = char_codespan(ob, rndr, data + i, i, size - i);
				break;
			case '*':
			case '_':
				used = char_emphasis(ob, rndr, data + i, i, size - i);
				break;
			case '~':
				used = char_strikethrough(ob, rndr, data + i, i, size - i);
				break;
			case '^':
				used = char_superscript(ob, rndr, data + i, i, size - i);
				break;
			case '[':
				used = char_link(ob, rndr, data + i, i, size - i);
				break;
			case '<':
				used = char_langle_tag(ob, rndr, data + i, i, size - i);
				break;
			case '&':
				used = char_entity(ob, rndr, data + i, i, size - i);
				break;
			case '\n':
				used = char_linebreak(ob, rndr, data + i, i, size - i);
				break;
			case ':':
			case '@':
			case 'w':
				used = char_autolink(ob, rndr, data + i, i, size - i);
				break;
			}

			if (!used) {
				/* 未处理的字符按普通文本输出 */
				buf_putc(&work, c);
				if (rndr->cb.normal_text) {
					rndr->cb.normal_text(ob, &work, rndr->options);
				}
				else
					buf_putc(ob, c);
				buf_reset(&work);
				used = 1;
			}

			i += used;
			end = i;
		}
	}

	free(work.data);
	rndr->depth--;
}

/* ================================================================== */
/* 前置：链接引用定义 / Front Matter                                    */
/* ================================================================== */

/* 判断一行是否是链接引用定义，返回消耗长度；否则 0 */
static size_t parse_ref_line(struct sd_markdown *rndr, const char *line, size_t len)
{
	size_t i = 0, id_b, id_e, link_b, link_e, title_b = 0, title_e = 0;
	char q = 0;

	if (len < 4 || line[0] != '[')
		return 0;

	i = 1;
	id_b = i;
	while (i < len && line[i] != ']' && line[i] != '\n')
		i++;
	if (i >= len || line[i] != ']')
		return 0;
	id_e = i;
	i++;

	if (i >= len || line[i] != ':')
		return 0;
	i++;
	while (i < len && line[i] == ' ')
		i++;
	if (i >= len)
		return 0;

	link_b = i;
	while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n')
		i++;
	link_e = i;
	if (link_e == link_b)
		return 0;

	/* 可选 title */
	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	if (i < len && (line[i] == '"' || line[i] == '\'')) {
		q = line[i];
		i++;
		title_b = i;
		while (i < len && line[i] != q)
			i++;
		if (i >= len)
			return 0;
		title_e = i;
		i++;
		while (i < len && (line[i] == ' ' || line[i] == '\t'))
			i++;
	}
	if (i != len)
		return 0;

	add_link_ref(rndr, line + id_b, id_e - id_b, line + link_b, link_e - link_b,
		title_b ? line + title_b : NULL, title_b ? title_e - title_b : 0);
	return len;
}

/* Front Matter：--- ... --- 开头 */
static size_t skip_front_matter(const char *data, size_t size)
{
	size_t i = 0, line_end;

	if (size < 4)
		return 0;
	if (data[0] != '-' || data[1] != '-' || data[2] != '-')
		return 0;
	if (data[3] != '\n')
		return 0;

	i = 4;
	while (i < size) {
		line_end = i;
		while (line_end < size && data[line_end] != '\n')
			line_end++;
		/* 结束标记 --- 或 ... */
		if ((data[i] == '-' && line_end - i >= 3) || (data[i] == '.' && line_end - i >= 3)) {
			size_t k = i;
			while (k < line_end && (data[k] == '-' || data[k] == '.'))
				k++;
			if (k == line_end)
				return (line_end < size) ? line_end + 1 : line_end;
		}
		i = (line_end < size) ? line_end + 1 : line_end;
	}
	return 0;
}

/* ================================================================== */
/* 导出：md 家族                                                        */
/* ================================================================== */

MD_EXPORT void *sd_markdown_new(unsigned int extensions, unsigned int max_nesting,
	const struct sd_callbacks *callbacks, void *opaque)
{
	struct sd_markdown *md = (struct sd_markdown *)calloc(1, sizeof(struct sd_markdown));
	if (!md)
		return NULL;

	if (callbacks)
		memcpy(&md->cb, callbacks, sizeof(struct sd_callbacks));
	md->ext_flags = extensions;
	md->max_nesting = max_nesting ? max_nesting : 16;
	md->options = (struct html_renderopt *)opaque;
	md->in_link_body = 0;
	md->depth = 0;
	md->refs = NULL;
	md->nrefs = 0;
	md->maxrefs = 0;
	return md;
}

MD_EXPORT void sd_markdown_free(void *p)
{
	struct sd_markdown *md = (struct sd_markdown *)p;
	size_t i;
	if (!md)
		return;
	for (i = 0; i < md->nrefs; i++) {
		free(md->refs[i].id.data);
		free(md->refs[i].link.data);
		free(md->refs[i].title.data);
	}
	free(md->refs);
	free(md);
}

MD_EXPORT int sd_markdown_render(struct buf *ob, char *document, unsigned int doc_size, void *mdp)
{
	struct sd_markdown *md = (struct sd_markdown *)mdp;
	struct buf text;
	size_t beg, end;

	if (!ob || !md)
		return 0;

	buf_init(&text);

	/* 第一遍：收集链接引用、归一化行尾 */
	beg = 0;
	while (beg < doc_size) {
		end = beg;
		while (end < doc_size && document[end] != '\n')
			end++;

		/* 去掉行尾 \r */
		{
			size_t e = end;
			if (e > beg && document[e - 1] == '\r')
				e--;
			if (!parse_ref_line(md, document + beg, e - beg)) {
				buf_put(&text, document + beg, e - beg);
				buf_putc(&text, '\n');
			}
		}

		beg = (end < doc_size) ? end + 1 : doc_size;
	}

	/* Front Matter */
	{
		size_t skip = skip_front_matter(text.data ? text.data : "", text.size);
		if (skip) {
			memmove(text.data, text.data + skip, text.size - skip);
			text.size -= skip;
			text.data[text.size] = 0;
		}
	}

	if (md->cb.doc_header)
		md->cb.doc_header(ob, md->options);

	if (text.size) {
		if (text.data[text.size - 1] != '\n')
			buf_putc(&text, '\n');
		parse_block(ob, md, text.data, text.size);
	}

	if (md->cb.doc_footer)
		md->cb.doc_footer(ob, md->options);

	free(text.data);
	return 1;
}

MD_EXPORT void sd_version(int *major, int *minor, int *patch)
{
	if (major) *major = 1;
	if (minor) *minor = 0;
	if (patch) *patch = 0;
}

/* ================================================================== */
/* 导出：sdhtml 家族                                                   */
/* ================================================================== */

MD_EXPORT int sdhtml_renderer(struct sd_callbacks *callbacks, struct html_renderopt *options,
	unsigned int render_flags)
{
	if (!callbacks)
		return 0;
	memcpy(callbacks, &cb_default, sizeof(cb_default));
	if (options) {
		memset(options, 0, sizeof(struct html_renderopt));
		options->flags = (int)render_flags;
	}
	return 1;
}

MD_EXPORT int sdhtml_toc_renderer(struct sd_callbacks *callbacks, struct html_renderopt *options,
	unsigned int render_flags)
{
	return sdhtml_renderer(callbacks, options, render_flags);
}

MD_EXPORT int sdhtml_is_tag(const char *tag_data, unsigned int tag_size, const char *tagname)
{
	size_t i;
	int closed = 0;

	if (tag_size < 3 || tag_data[0] != '<')
		return 0;

	i = 1;
	if (tag_data[i] == '/') {
		closed = 1;
		i++;
	}

	for (; i < tag_size; ++i, ++tagname) {
		if (*tagname == 0)
			break;
		if (tag_data[i] != *tagname)
			return 0;
	}

	if (i == tag_size)
		return 0;
	if (closed)
		return 2;
	if (tag_data[i] == '>' || tag_data[i] == ' ')
		return 1;
	return 0;
}

/* smartypants：把 ASCII 引号/破折号转成 HTML 实体 */
MD_EXPORT int sdhtml_smartypants(struct buf *ob, const char *text, unsigned int size)
{
	size_t i = 0;
	int in_single = 0, in_double = 0;

	if (!ob)
		return 0;

	for (i = 0; i < size; i++) {
		char c = text[i];
		switch (c) {
		case '"':
			if (in_double) {
				buf_puts(ob, "&rdquo;");
				in_double = 0;
			}
			else {
				buf_puts(ob, "&ldquo;");
				in_double = 1;
			}
			break;
		case '\'':
			if (in_single) {
				buf_puts(ob, "&rsquo;");
				in_single = 0;
			}
			else {
				buf_puts(ob, "&lsquo;");
				in_single = 1;
			}
			break;
		case '-':
			if (i + 1 < size && text[i + 1] == '-') {
				buf_puts(ob, "&mdash;");
				i++;
			}
			else
				buf_putc(ob, c);
			break;
		case '.':
			if (i + 2 < size && text[i + 1] == '.' && text[i + 2] == '.') {
				buf_puts(ob, "&hellip;");
				i += 2;
			}
			else
				buf_putc(ob, c);
			break;
		default:
			buf_putc(ob, c);
			break;
		}
	}
	return 1;
}
