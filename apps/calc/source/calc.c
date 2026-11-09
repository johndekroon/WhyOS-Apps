// SPDX-License-Identifier: MIT
/*
 * calc — a calculator for the badge (sdapps/README.md), on the WhyOS SDK
 * (sdk/README.md): an entry screen. You type a sum, it shows the result
 * as you type, Enter (or =) works it out and puts it on the roll above.
 * Starting with + * / ^ % or ! goes on from the last result; an x after a
 * number is "times". The sums themselves are sums.c.
 */
#include "whyos.h"
#include "sums.h"

static char bases[96];

static const char *preview(const char *line, char *res, int size)
{
	double v;

	bases[0] = 0;
	if (!line[0]) {		/* the last result, in all three bases */
		if (sums_have_ans)
			fmt_bases(sums_ans, bases);
		fmt_num(sums_have_ans ? sums_ans : 0, res);
		return 0;
	}
	if (calc(line, &v))
		return "";
	fmt_bases(v, bases);
	wos_copy(res, "= ", size);
	fmt_num(v, res + 2);
	return 0;
}

static const char *evaluate(const char *line, char *res, int size)
{
	char log[WOS_LOG_LEN] = "";
	const char *why = calc(line, &sums_ans);

	(void)size;
	if (why)
		return why;
	sums_have_ans = 1;
	fmt_num(sums_ans, res);
	wos_add(wos_add(wos_add(log, line, sizeof(log)), " = ", sizeof(log)), res, sizeof(log));
	wos_log(log);
	return 0;
}

/* what a key types: = works the sum out, x after a number is times, an
 * operator first goes on from the last result */
static const char *typed(const char *line, int cur, int ch)
{
	static char s[8];
	int n = 0, digit_before = cur && line[cur - 1] >= '0' && line[cur - 1] <= '9';

	if (ch == '=')
		return "\n";
	if (ch == 'x' && digit_before && !(line[cur - 1] == '0' && (cur == 1 || !strchr("0123456789.", line[cur - 2]))))
		ch = '*';
	if (!strchr("0123456789.+-*/^()%! ", ch) && !(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z'))
		return "";
	if (!line[0] && sums_have_ans && strchr("+*/^%!", ch))
		n = (int)strlen(wos_copy(s, "ans", sizeof(s)));
	s[n] = (char)ch;
	s[n + 1] = 0;
	return s;
}

static void angle(void)
{
	sums_rad = !strcmp(wos_get("angle", 0), "rad");
}

static void toggle(int arg)
{
	(void)arg;
	wos_set("angle", sums_rad ? "deg" : "rad");
	angle();
}

static void update(wos_screen *s)
{
	s->title_right = sums_rad ? "RAD" : "DEG";
}

static const wos_action keys[] = {
	{ WOS_KEY_ENTER, "work out", "Work the sum out (= does too)" },
	{ WOS_KEY_TAB, "deg/rad", "Degrees or radians", toggle },
	{ 0 }
};

static const wos_setting settings[] = {
	{ "angle", "Angle unit", "For sin, cos, tan and their inverses. Tab switches it too.", WOS_CHOICE, "deg",
	  "deg=Degrees|rad=Radians", .changed = angle },
	{ 0 }
};

int main(void)
{
	static const wos_app app = {
		.name = "Calculator", .settings = settings,
		.about = "Type a sum and press Enter. Also: ^ % ! mod pi e ans sqrt ln log sin cos tan. "
			 "+ * / or ^ first goes on from the last result.",
	};
	static wos_screen roll = {
		.kind = WOS_ENTRY, .actions = keys, .evaluate = evaluate, .preview = preview, .typed = typed,
		.update = update, .placeholder = "Type a sum", .detail = bases, .right = 1,
	};

	angle();
	return wos_run(&app, &roll);
}
