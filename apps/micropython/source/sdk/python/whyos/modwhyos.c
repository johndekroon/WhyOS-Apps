// SPDX-License-Identifier: MIT
/*
 * The WhyOS SDK in MicroPython: the module "whyos", a user C module of the
 * store's MicroPython (sdapps/build.sh micropython). The same screens,
 * actions and settings as in C (sdk/README.md), described in Python:
 *
 *	import whyos
 *	light = whyos.Colour(keys_ms=2000)
 *	light.action(whyos.SECOND, "Red", "Red light", lambda: setattr(light, "colour", 0xFF0000))
 *	whyos.run("Flashlight", light, keep_on=True)
 *
 * The SDK runs the loop and draws in C; Python only describes the screens
 * and handles their actions, so a key costs the C drawing plus a call into
 * Python. An exception in a callback is printed to stderr (the app's log)
 * and the app goes on. sdk/python/README.md is the guide.
 */
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mphal.h"

#include "whyos.h"

#define PY_ACTIONS	16		/* per screen */
#define PY_SLOTS	128		/* actions of all screens */
#define PY_SETTINGS	16

typedef struct {
	mp_obj_base_t base;
	wos_screen s;
	mp_obj_t attrs;			/* dict: the Python values behind s's fields */
	mp_obj_t keep;			/* list: the callables and strings of the actions */
	mp_obj_t group[3];		/* the arrows' functions: ARROWS, LEFTRIGHT, UPDOWN */
	wos_action acts[PY_ACTIONS + 1];
	wos_action menu[PY_ACTIONS + 1];
	wos_setting fields[PY_SETTINGS + 1];
} py_screen_t;

static const mp_obj_type_t py_screen_type;

/* callables by slot: the actions of every screen (also kept in its list) */
static mp_obj_t py_slot[PY_SLOTS];
static int py_n_slot;

static mp_obj_t py_app_tick, py_app_quit;
static wos_setting py_settings[PY_SETTINGS + 1];

MP_REGISTER_ROOT_POINTER(mp_obj_t whyos_keep);
MP_REGISTER_ROOT_POINTER(mp_obj_t whyos_pushed);
MP_REGISTER_ROOT_POINTER(mp_obj_t whyos_pop_fn);

static py_screen_t *py_of(wos_screen *s)
{
	return (py_screen_t *)((char *)s - offsetof(py_screen_t, s));
}

/* str(x) of an exception into buf */
static const char *py_message(mp_obj_t exc, char *buf, int size)
{
	vstr_t v;
	mp_print_t p;

	vstr_init_print(&v, 64, &p);
	mp_obj_print_helper(&p, exc, PRINT_STR);
	wos_copy(buf, vstr_null_terminated_str(&v), size);
	vstr_clear(&v);
	return buf;
}

/*
 * fn(args) from C; MP_OBJ_NULL when it raised, the exception printed to
 * stderr and its message (if msg) in msg. SystemExit ends the app.
 */
static mp_obj_t py_call(mp_obj_t fn, size_t n, const mp_obj_t *args, char *msg, int size)
{
	nlr_buf_t nlr;
	mp_obj_t r;

	if (fn == MP_OBJ_NULL || fn == mp_const_none)
		return mp_const_none;
	if (nlr_push(&nlr) == 0) {
		r = mp_call_function_n_kw(fn, n, 0, args);
		nlr_pop();
		return r;
	}
	if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(MP_OBJ_FROM_PTR(nlr.ret_val))),
				    MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
		wos_quit();
		return MP_OBJ_NULL;
	}
	if (msg)
		py_message(MP_OBJ_FROM_PTR(nlr.ret_val), msg, size);
	else
		mp_obj_print_exception(&mp_stderr_print, MP_OBJ_FROM_PTR(nlr.ret_val));
	return MP_OBJ_NULL;
}

static const char *py_str(mp_obj_t o)
{
	return o == MP_OBJ_NULL || o == mp_const_none ? 0 : mp_obj_str_get_str(o);
}

/* ---- the trampolines: the SDK's C callbacks into Python ------------------ */

static void py_action(int slot)
{
	py_call(py_slot[slot], 0, 0, 0, 0);
}

static void py_group(int k, int step)
{
	mp_obj_t a = mp_obj_new_int(step);

	py_call(py_of(wos_top())->group[k], 1, &a, 0, 0);
}

static void py_arrows(int step) { py_group(0, step); }
static void py_leftright(int step) { py_group(1, step); }
static void py_updown(int step) { py_group(2, step); }

/* the square key's menu: the item chosen (the pop-up's focus) on the row */
static mp_obj_t py_attr_get(py_screen_t *o, qstr q);

static void py_menu_action(int row)
{
	mp_obj_t menu = py_attr_get(py_of(wos_top()), MP_QSTR_menu), a = mp_obj_new_int(row);
	size_t n, k;
	mp_obj_t *items, *pair;

	mp_obj_get_array(menu, &n, &items);
	if (wos.pop_focus < 0 || (size_t)wos.pop_focus >= n)
		return;
	mp_obj_get_array(items[wos.pop_focus], &k, &pair);	/* (word, fn(row)) */
	if (k > 1)
		py_call(pair[1], 1, &a, 0, 0);
}

static mp_obj_t py_attr_get(py_screen_t *o, qstr q)
{
	mp_map_elem_t *e = mp_map_lookup(mp_obj_dict_get_map(o->attrs), MP_OBJ_NEW_QSTR(q), MP_MAP_LOOKUP);

	return e ? e->value : MP_OBJ_NULL;
}

static void py_row(int i, wos_row *r)
{
	py_screen_t *o = py_of(wos_top());
	mp_obj_t rows = py_attr_get(o, MP_QSTR_rows), item;
	size_t n, k;
	mp_obj_t *items, *parts;

	if (rows == MP_OBJ_NULL)
		return;
	mp_obj_get_array(rows, &n, &items);
	if (i < 0 || (size_t)i >= n)
		return;
	item = items[i];
	r->kind = py_attr_get(o, MP_QSTR_choose) ? WOS_ROW_OPEN : WOS_ROW_PLAIN;
	if (mp_obj_is_str(item)) {
		r->label = mp_obj_str_get_str(item);
		return;
	}
	mp_obj_get_array(item, &k, &parts);	/* (label, value, sub) */
	r->label = k > 0 ? py_str(parts[0]) : "";
	r->value = k > 1 ? py_str(parts[1]) : 0;
	r->sub = k > 2 ? py_str(parts[2]) : 0;
}

static void py_choose(int i)
{
	mp_obj_t a = mp_obj_new_int(i);

	py_call(py_attr_get(py_of(wos_top()), MP_QSTR_choose), 1, &a, 0, 0);
}

static void py_update(wos_screen *s)
{
	py_screen_t *o = py_of(s);
	mp_obj_t rows = py_attr_get(o, MP_QSTR_rows);

	py_call(py_attr_get(o, MP_QSTR_update), 0, 0, 0, 0);
	/* the fields from their Python values, which the app may have changed */
	if (rows != MP_OBJ_NULL)
		s->n = (int)mp_obj_get_int(mp_obj_len(rows));
}

/* the line's result, or what is wrong: the function's return value, or the
 * message of what it raised */
static char py_err[96];

static const char *py_evaluate(const char *line, char *res, int size)
{
	mp_obj_t a = mp_obj_new_str(line, strlen(line)), r;

	r = py_call(py_attr_get(py_of(wos_top()), MP_QSTR_evaluate), 1, &a, py_err, sizeof(py_err));
	if (r == MP_OBJ_NULL)
		return py_err[0] ? py_err : "Something went wrong";
	if (r != mp_const_none)
		wos_copy(res, py_message(r, py_err, sizeof(py_err)), size);
	return 0;
}

static const char *py_preview(const char *line, char *res, int size)
{
	mp_obj_t a = mp_obj_new_str(line, strlen(line)), r;
	char err[8];

	r = py_call(py_attr_get(py_of(wos_top()), MP_QSTR_preview), 1, &a, err, sizeof(err));
	if (r == MP_OBJ_NULL || r == mp_const_none)
		return "";
	wos_copy(res, py_message(r, py_err, sizeof(py_err)), size);
	return 0;
}

static void py_abort(void)
{
	py_call(py_attr_get(py_of(wos_top()), MP_QSTR_abort), 0, 0, 0, 0);
}

static void py_tick(void)
{
	py_call(py_app_tick, 0, 0, 0, 0);
}

static void py_quit(void)
{
	py_call(py_app_quit, 0, 0, 0, 0);
}

/* ---- Screen --------------------------------------------------------------- */

static void py_schema(mp_obj_t list, wos_setting *out);

/* the C fields from a Python value set on the screen */
static void py_field(py_screen_t *o, qstr q, mp_obj_t v)
{
	wos_screen *s = &o->s;

	switch (q) {
	case MP_QSTR_title: s->title = py_str(v); break;
	case MP_QSTR_eyebrow: s->eyebrow = py_str(v); break;
	case MP_QSTR_about: s->about = py_str(v); break;
	case MP_QSTR_title_right: s->title_right = py_str(v); break;
	case MP_QSTR_esc: s->esc = py_str(v); break;
	case MP_QSTR_placeholder: s->placeholder = py_str(v); break;
	case MP_QSTR_detail: s->detail = py_str(v); break;
	case MP_QSTR_colour: s->colour = (uint32_t)mp_obj_get_int_truncated(v); break;
	case MP_QSTR_brightness: s->brightness = (int)mp_obj_get_int(v); break;
	case MP_QSTR_keys_ms: s->keys_ms = (int)mp_obj_get_int(v); break;
	case MP_QSTR_right: s->right = mp_obj_is_true(v); break;
	case MP_QSTR_focus: s->focus = (int)mp_obj_get_int(v); break;
	case MP_QSTR_dirty: s->dirty = mp_obj_is_true(v); break;
	case MP_QSTR_busy: s->busy = py_str(v); break;
	case MP_QSTR_text: {		/* an editor's text: a buffer of max bytes the SDK edits in place */
		mp_obj_t max = py_attr_get(o, MP_QSTR_max);
		size_t n = max != MP_OBJ_NULL ? (size_t)mp_obj_get_int(max) : 1000;
		const char *t = mp_obj_str_get_str(v);
		mp_obj_t buf = mp_obj_new_bytearray_by_ref(n + 1, m_new0(char, n + 1));
		mp_buffer_info_t bi;

		mp_get_buffer_raise(buf, &bi, MP_BUFFER_WRITE);
		wos_copy(bi.buf, t, (int)n + 1);
		s->text = bi.buf;
		s->max = (int)n + 1;
		mp_obj_dict_store(o->attrs, MP_OBJ_NEW_QSTR(MP_QSTR__buf), buf);
		return;
	}
	case MP_QSTR_max: break;
	case MP_QSTR_progress: s->progress = (int)mp_obj_get_int(v); break;
	case MP_QSTR_abort: s->abort = v == mp_const_none ? 0 : py_abort; break;
	case MP_QSTR_rows: s->n = (int)mp_obj_get_int(mp_obj_len(v)); s->row = py_row; break;
	case MP_QSTR_choose: s->choose = v == mp_const_none ? 0 : py_choose; break;
	case MP_QSTR_evaluate: s->evaluate = v == mp_const_none ? 0 : py_evaluate; break;
	case MP_QSTR_preview: s->preview = v == mp_const_none ? 0 : py_preview; break;
	case MP_QSTR_update: break;
	case MP_QSTR_fields:
		py_schema(v, o->fields);
		s->fields = v == mp_const_none ? 0 : o->fields;
		break;
	case MP_QSTR_menu: {
		size_t n;
		mp_obj_t *items;

		mp_obj_get_array(v, &n, &items);
		memset(o->menu, 0, sizeof(o->menu));
		for (size_t i = 0; i < n && i < PY_ACTIONS; i++) {
			size_t k;
			mp_obj_t *pair;

			mp_obj_get_array(items[i], &k, &pair);	/* (word, fn(row)) */
			o->menu[i] = (wos_action){ WOS_KEY_ENTER, py_str(pair[0]), 0, py_menu_action, (int)i };
		}
		s->menu = n ? o->menu : 0;
		break;
	}
	default:
		mp_raise_msg_varg(&mp_type_AttributeError, MP_ERROR_TEXT("Screen has no field '%q'"), q);
	}
	mp_obj_dict_store(o->attrs, MP_OBJ_NEW_QSTR(q), v);
}

static mp_obj_t py_screen_make(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
	py_screen_t *o = mp_obj_malloc(py_screen_t, type);
	mp_map_t kw;

	mp_arg_check_num(n_args, n_kw, 1, 1, true);
	memset(&o->s, 0, sizeof(o->s));
	o->s.kind = (int)mp_obj_get_int(args[0]);
	o->s.update = py_update;
	o->s.progress = -1;
	o->attrs = mp_obj_new_dict(0);
	o->keep = mp_obj_new_list(0, 0);
	o->group[0] = o->group[1] = o->group[2] = mp_const_none;
	memset(o->acts, 0, sizeof(o->acts));
	o->s.actions = o->acts;
	if (o->s.kind == WOS_COLOUR)
		o->s.colour = 0xFFFFFF;
	mp_map_init_fixed_table(&kw, n_kw, args + n_args);
	/* the text last: an editor's buffer takes its size from max */
	for (int text = 0; text < 2; text++)
		for (size_t i = 0; i < kw.alloc; i++)
			if (mp_map_slot_is_filled(&kw, i) &&
			    (mp_obj_str_get_qstr(kw.table[i].key) == MP_QSTR_text) == text)
				py_field(o, mp_obj_str_get_qstr(kw.table[i].key), kw.table[i].value);
	return MP_OBJ_FROM_PTR(o);
}

static void py_screen_attr(mp_obj_t self, qstr q, mp_obj_t *dest)
{
	py_screen_t *o = MP_OBJ_TO_PTR(self);

	if (dest[0] == MP_OBJ_NULL) {		/* load */
		mp_obj_t v = py_attr_get(o, q);

		if (q == MP_QSTR_focus)
			dest[0] = mp_obj_new_int(o->s.focus);
		else if (q == MP_QSTR_text && o->s.text)
			dest[0] = mp_obj_new_str(o->s.text, strlen(o->s.text));
		else if (q == MP_QSTR_dirty)
			dest[0] = mp_obj_new_bool(o->s.dirty);
		else if (v != MP_OBJ_NULL)
			dest[0] = v;
		else if (q == MP_QSTR_colour || q == MP_QSTR_brightness || q == MP_QSTR_keys_ms)
			dest[0] = mp_obj_new_int(q == MP_QSTR_colour ? (mp_int_t)o->s.colour :
						 q == MP_QSTR_brightness ? o->s.brightness : o->s.keys_ms);
		else
			dest[1] = MP_OBJ_SENTINEL;	/* the methods */
		return;
	}
	if (dest[1] != MP_OBJ_NULL) {		/* store */
		py_field(o, q, dest[1]);
		dest[0] = MP_OBJ_NULL;
	}
}

/* screen.action(key, word, help, fn): fn() on the key; for the arrow
 * groups fn(step) with -1 or +1; word None: only in the help */
static mp_obj_t py_screen_action(size_t n_args, const mp_obj_t *args)
{
	py_screen_t *o = MP_OBJ_TO_PTR(args[0]);
	int key = (int)mp_obj_get_int(args[1]), n = 0;
	mp_obj_t fn = n_args > 4 ? args[4] : mp_const_none;

	while (o->acts[n].key)
		n++;
	if (n == PY_ACTIONS || py_n_slot == PY_SLOTS)
		mp_raise_ValueError(MP_ERROR_TEXT("too many actions"));
	mp_obj_list_append(o->keep, mp_obj_new_tuple(n_args - 1, args + 1));
	o->acts[n] = (wos_action){ key, py_str(args[2]), n_args > 3 ? py_str(args[3]) : 0, 0, 0 };
	if (fn != mp_const_none) {
		if (key == WOS_KEY_ARROWS || key == WOS_KEY_LEFTRIGHT || key == WOS_KEY_UPDOWN) {
			o->group[-key - 1] = fn;
			o->acts[n].fn = key == WOS_KEY_ARROWS ? py_arrows : key == WOS_KEY_LEFTRIGHT ? py_leftright : py_updown;
		} else {
			py_slot[py_n_slot] = fn;
			o->acts[n].fn = py_action;
			o->acts[n].arg = py_n_slot++;
		}
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_screen_action_obj, 3, 5, py_screen_action);

static const mp_rom_map_elem_t py_screen_locals[] = {
	{ MP_ROM_QSTR(MP_QSTR_action), MP_ROM_PTR(&py_screen_action_obj) },
};
static MP_DEFINE_CONST_DICT(py_screen_locals_dict, py_screen_locals);

static MP_DEFINE_CONST_OBJ_TYPE(py_screen_type, MP_QSTR_Screen, MP_TYPE_FLAG_NONE,
				make_new, py_screen_make, attr, py_screen_attr, locals_dict, &py_screen_locals_dict);

/* whyos.Colour(**fields), List, Entry: a Screen of that kind */
static mp_obj_t py_kind(int kind, size_t n_args, const mp_obj_t *args, mp_map_t *kw)
{
	mp_obj_t a[1 + 2 * 16];
	size_t n = 0;

	if (n_args)
		mp_raise_TypeError(MP_ERROR_TEXT("give the fields by name"));
	a[0] = mp_obj_new_int(kind);
	for (size_t i = 0; i < kw->alloc && n < 16; i++)
		if (mp_map_slot_is_filled(kw, i)) {
			a[1 + 2 * n] = kw->table[i].key;
			a[2 + 2 * n] = kw->table[i].value;
			n++;
		}
	return py_screen_make(&py_screen_type, 1, n, a);
}

static mp_obj_t py_colour(size_t n, const mp_obj_t *a, mp_map_t *kw) { return py_kind(WOS_COLOUR, n, a, kw); }
static mp_obj_t py_list(size_t n, const mp_obj_t *a, mp_map_t *kw) { return py_kind(WOS_LIST, n, a, kw); }
static mp_obj_t py_entry(size_t n, const mp_obj_t *a, mp_map_t *kw) { return py_kind(WOS_ENTRY, n, a, kw); }
static mp_obj_t py_editor(size_t n, const mp_obj_t *a, mp_map_t *kw) { return py_kind(WOS_EDITOR, n, a, kw); }
static MP_DEFINE_CONST_FUN_OBJ_KW(py_colour_obj, 0, py_colour);
static MP_DEFINE_CONST_FUN_OBJ_KW(py_list_obj, 0, py_list);
static MP_DEFINE_CONST_FUN_OBJ_KW(py_entry_obj, 0, py_entry);
static MP_DEFINE_CONST_FUN_OBJ_KW(py_editor_obj, 0, py_editor);

/* ---- the app -------------------------------------------------------------- */

static py_screen_t *py_screen_arg(mp_obj_t o)
{
	if (!mp_obj_is_type(o, &py_screen_type))
		mp_raise_TypeError(MP_ERROR_TEXT("not a whyos screen"));
	return MP_OBJ_TO_PTR(o);
}

/* settings=[(key, label, SWITCH|CHOICE|SLIDER, default, choices or (min, max, step), sub), ...] */
static void py_schema(mp_obj_t list, wos_setting *out)
{
	size_t n, k;
	mp_obj_t *items, *t;

	memset(out, 0, sizeof(wos_setting) * (PY_SETTINGS + 1));
	if (list == mp_const_none)
		return;
	mp_obj_get_array(list, &n, &items);
	for (size_t i = 0; i < n && i < PY_SETTINGS; i++) {
		wos_setting *s = &out[i];

		mp_obj_get_array(items[i], &k, &t);
		if (k < 4)
			mp_raise_ValueError(MP_ERROR_TEXT("a setting is (key, label, kind, default, ...)"));
		s->key = py_str(t[0]);
		s->label = py_str(t[1]);
		s->kind = (int)mp_obj_get_int(t[2]);
		s->def = py_str(t[3]);
		if (k > 4 && s->kind == WOS_CHOICE)
			s->choices = py_str(t[4]);
		if (k > 4 && s->kind == WOS_SLIDER) {
			size_t m;
			mp_obj_t *r;

			mp_obj_get_array(t[4], &m, &r);
			s->min = m > 0 ? (int)mp_obj_get_int(r[0]) : 0;
			s->max = m > 1 ? (int)mp_obj_get_int(r[1]) : 100;
			s->step = m > 2 ? (int)mp_obj_get_int(r[2]) : 1;
		}
		if (k > 5)
			s->sub = py_str(t[5]);
	}
}

/* whyos.run(name, screen, about=None, keep_on=False, settings=None,
 * tick=None, tick_ms=0, quit=None): the app, until it ends */
static mp_obj_t py_run(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
	enum { A_NAME, A_SCREEN, A_ABOUT, A_KEEP_ON, A_SETTINGS, A_TICK, A_TICK_MS, A_QUIT };
	static const mp_arg_t allowed[] = {
		{ MP_QSTR_name, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
		{ MP_QSTR_screen, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
		{ MP_QSTR_about, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
		{ MP_QSTR_keep_on, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
		{ MP_QSTR_settings, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
		{ MP_QSTR_tick, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
		{ MP_QSTR_tick_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
		{ MP_QSTR_quit, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	static wos_app app;
	mp_obj_t keep[4];
	int r;

	mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
	keep[0] = a[A_NAME].u_obj;
	keep[1] = a[A_ABOUT].u_obj;
	keep[2] = a[A_SETTINGS].u_obj;
	keep[3] = mp_obj_new_tuple(2, (mp_obj_t[]){ a[A_TICK].u_obj, a[A_QUIT].u_obj });
	MP_STATE_VM(whyos_keep) = mp_obj_new_tuple(4, keep);	/* alive while the app runs */
	py_schema(a[A_SETTINGS].u_obj, py_settings);
	py_app_tick = a[A_TICK].u_obj;
	py_app_quit = a[A_QUIT].u_obj;
	app = (wos_app){
		.name = py_str(a[A_NAME].u_obj), .about = py_str(a[A_ABOUT].u_obj), .keep_on = a[A_KEEP_ON].u_bool,
		.settings = py_settings, .tick = py_app_tick != mp_const_none ? py_tick : 0,
		.tick_ms = (int)a[A_TICK_MS].u_int, .quit = py_app_quit != mp_const_none ? py_quit : 0,
	};
	r = wos_run(&app, &py_screen_arg(a[A_SCREEN].u_obj)->s);
	MP_STATE_VM(whyos_keep) = MP_OBJ_NULL;
	MP_STATE_VM(whyos_pushed) = MP_OBJ_NULL;
	return mp_obj_new_int(r);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_run_obj, 2, py_run);

/* a screen on top; the SDK keeps a C pointer to it, so the screens on the
 * stack are kept alive here (the garbage collector does not see the SDK's) */
static mp_obj_t py_push(mp_obj_t s)
{
	mp_obj_t keep = mp_obj_new_list(0, 0);
	size_t n;
	mp_obj_t *items;

	if (MP_STATE_VM(whyos_pushed) != MP_OBJ_NULL) {
		mp_obj_get_array(MP_STATE_VM(whyos_pushed), &n, &items);
		for (size_t i = 0; i < n; i++)
			for (int d = 0; d < wos.depth; d++)
				if (wos.stack[d] == &((py_screen_t *)MP_OBJ_TO_PTR(items[i]))->s)
					mp_obj_list_append(keep, items[i]);
	}
	mp_obj_list_append(keep, s);
	MP_STATE_VM(whyos_pushed) = keep;
	wos_push(&py_screen_arg(s)->s);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_push_obj, py_push);

static mp_obj_t py_pop(void)
{
	wos_pop();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_pop_obj, py_pop);

static mp_obj_t py_quit_now(void)
{
	wos_quit();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_quit_obj, py_quit_now);

/* whyos.get(key, default=None): a setting as text */
static mp_obj_t py_get(size_t n_args, const mp_obj_t *args)
{
	const char *v = wos_get(mp_obj_str_get_str(args[0]), "\x01");

	if (!strcmp(v, "\x01")) {
		const char *d = wos_kv_default ? wos_kv_default(mp_obj_str_get_str(args[0])) : 0;

		if (!d)
			return n_args > 1 ? args[1] : mp_const_none;
		v = d;
	}
	return mp_obj_new_str(v, strlen(v));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_get_obj, 1, 2, py_get);

/* whyos.set(key, value): kept at once, as str(value) */
static mp_obj_t py_set(mp_obj_t k, mp_obj_t v)
{
	char b[WOS_VAL_LEN];

	wos_set(mp_obj_str_get_str(k), mp_obj_is_str(v) ? mp_obj_str_get_str(v) : py_message(v, b, sizeof(b)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(py_set_obj, py_set);

static mp_obj_t py_log(mp_obj_t line)
{
	char b[WOS_LOG_LEN];

	wos_log(mp_obj_is_str(line) ? mp_obj_str_get_str(line) : py_message(line, b, sizeof(b)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_log_obj, py_log);

/* whyos.ask(title, body, choices, done, safe=0, danger=-1): done(i) with
 * the choice, -1 on Esc; choices as "Keep editing|Discard" or a list */
static void py_asked(int choice)
{
	mp_obj_t a = mp_obj_new_int(choice);

	py_call(MP_STATE_VM(whyos_pop_fn), 1, &a, 0, 0);
}

static mp_obj_t py_ask(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
	enum { A_TITLE, A_BODY, A_CHOICES, A_DONE, A_SAFE, A_DANGER };
	static const mp_arg_t allowed[] = {
		{ MP_QSTR_title, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
		{ MP_QSTR_body, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
		{ MP_QSTR_choices, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
		{ MP_QSTR_done, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
		{ MP_QSTR_safe, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
		{ MP_QSTR_danger, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = -1 } },
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	char choices[256] = "";
	mp_obj_t c;

	mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
	c = a[A_CHOICES].u_obj;
	if (mp_obj_is_str(c)) {
		wos_copy(choices, mp_obj_str_get_str(c), sizeof(choices));
	} else {
		size_t n;
		mp_obj_t *items;

		mp_obj_get_array(c, &n, &items);
		for (size_t i = 0; i < n; i++)
			wos_add(wos_add(choices, i ? "|" : "", sizeof(choices)), mp_obj_str_get_str(items[i]), sizeof(choices));
	}
	MP_STATE_VM(whyos_pop_fn) = a[A_DONE].u_obj;
	wos_ask(py_str(a[A_TITLE].u_obj), py_str(a[A_BODY].u_obj), choices, (int)a[A_SAFE].u_int,
		(int)a[A_DANGER].u_int, py_asked);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_ask_obj, 4, py_ask);

/* whyos.ask_text(title, explain, text, done): done(text) returns None to
 * close, or what is wrong to keep the pop-up open */
static const char *py_texted(const char *text)
{
	static char err[96];
	mp_obj_t a = mp_obj_new_str(text, strlen(text)), r;

	r = py_call(MP_STATE_VM(whyos_pop_fn), 1, &a, err, sizeof(err));
	if (r == MP_OBJ_NULL)
		return err[0] ? err : "Something went wrong";
	return r == mp_const_none ? 0 : py_message(r, err, sizeof(err));
}

static mp_obj_t py_ask_text(size_t n_args, const mp_obj_t *args)
{
	MP_STATE_VM(whyos_pop_fn) = args[3];
	wos_ask_text(py_str(args[0]), py_str(args[1]), py_str(args[2]), py_texted);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_ask_text_obj, 4, 4, py_ask_text);

static mp_obj_t py_msg(mp_obj_t text)
{
	wos_message(mp_obj_str_get_str(text));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_msg_obj, py_msg);

/* whyos.char("r"): a character as an action's key */
static mp_obj_t py_char(mp_obj_t c)
{
	const char *s = mp_obj_str_get_str(c);

	return mp_obj_new_int(WOS_CHAR((unsigned char)s[0]));
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_char_obj, py_char);

static const mp_rom_map_elem_t whyos_globals[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_whyos) },
	{ MP_ROM_QSTR(MP_QSTR_Screen), MP_ROM_PTR(&py_screen_type) },
	{ MP_ROM_QSTR(MP_QSTR_Colour), MP_ROM_PTR(&py_colour_obj) },
	{ MP_ROM_QSTR(MP_QSTR_List), MP_ROM_PTR(&py_list_obj) },
	{ MP_ROM_QSTR(MP_QSTR_Entry), MP_ROM_PTR(&py_entry_obj) },
	{ MP_ROM_QSTR(MP_QSTR_Editor), MP_ROM_PTR(&py_editor_obj) },
	{ MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&py_run_obj) },
	{ MP_ROM_QSTR(MP_QSTR_push), MP_ROM_PTR(&py_push_obj) },
	{ MP_ROM_QSTR(MP_QSTR_pop), MP_ROM_PTR(&py_pop_obj) },
	{ MP_ROM_QSTR(MP_QSTR_quit), MP_ROM_PTR(&py_quit_obj) },
	{ MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&py_get_obj) },
	{ MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&py_set_obj) },
	{ MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&py_log_obj) },
	{ MP_ROM_QSTR(MP_QSTR_char), MP_ROM_PTR(&py_char_obj) },
	{ MP_ROM_QSTR(MP_QSTR_ask), MP_ROM_PTR(&py_ask_obj) },
	{ MP_ROM_QSTR(MP_QSTR_ask_text), MP_ROM_PTR(&py_ask_text_obj) },
	{ MP_ROM_QSTR(MP_QSTR_message), MP_ROM_PTR(&py_msg_obj) },
	{ MP_ROM_QSTR(MP_QSTR_TEXT), MP_ROM_INT(WOS_TEXT) },
	/* screens */
	{ MP_ROM_QSTR(MP_QSTR_COLOUR), MP_ROM_INT(WOS_COLOUR) },
	{ MP_ROM_QSTR(MP_QSTR_ENTRY), MP_ROM_INT(WOS_ENTRY) },
	{ MP_ROM_QSTR(MP_QSTR_LIST), MP_ROM_INT(WOS_LIST) },
	/* settings */
	{ MP_ROM_QSTR(MP_QSTR_SWITCH), MP_ROM_INT(WOS_SWITCH) },
	{ MP_ROM_QSTR(MP_QSTR_CHOICE), MP_ROM_INT(WOS_CHOICE) },
	{ MP_ROM_QSTR(MP_QSTR_SLIDER), MP_ROM_INT(WOS_SLIDER) },
	/* keys */
	{ MP_ROM_QSTR(MP_QSTR_OPTIONS), MP_ROM_INT(WOS_KEY_OPTIONS) },
	{ MP_ROM_QSTR(MP_QSTR_MAIN), MP_ROM_INT(WOS_KEY_MAIN) },
	{ MP_ROM_QSTR(MP_QSTR_SECOND), MP_ROM_INT(WOS_KEY_SECOND) },
	{ MP_ROM_QSTR(MP_QSTR_ENTER), MP_ROM_INT(WOS_KEY_ENTER) },
	{ MP_ROM_QSTR(MP_QSTR_TAB), MP_ROM_INT(WOS_KEY_TAB) },
	{ MP_ROM_QSTR(MP_QSTR_BACKSPACE), MP_ROM_INT(WOS_KEY_BACKSPACE) },
	{ MP_ROM_QSTR(MP_QSTR_DELETE), MP_ROM_INT(WOS_KEY_DELETE) },
	{ MP_ROM_QSTR(MP_QSTR_SPACE), MP_ROM_INT(WOS_KEY_SPACE) },
	{ MP_ROM_QSTR(MP_QSTR_UP), MP_ROM_INT(WOS_KEY_UP) },
	{ MP_ROM_QSTR(MP_QSTR_DOWN), MP_ROM_INT(WOS_KEY_DOWN) },
	{ MP_ROM_QSTR(MP_QSTR_LEFT), MP_ROM_INT(WOS_KEY_LEFT) },
	{ MP_ROM_QSTR(MP_QSTR_RIGHT), MP_ROM_INT(WOS_KEY_RIGHT) },
	{ MP_ROM_QSTR(MP_QSTR_ARROWS), MP_ROM_INT(WOS_KEY_ARROWS) },
	{ MP_ROM_QSTR(MP_QSTR_LEFTRIGHT), MP_ROM_INT(WOS_KEY_LEFTRIGHT) },
	{ MP_ROM_QSTR(MP_QSTR_UPDOWN), MP_ROM_INT(WOS_KEY_UPDOWN) },
};
static MP_DEFINE_CONST_DICT(whyos_globals_dict, whyos_globals);

const mp_obj_module_t whyos_module = {
	.base = { &mp_type_module },
	.globals = (mp_obj_dict_t *)&whyos_globals_dict,
};

MP_REGISTER_MODULE(MP_QSTR_whyos, whyos_module);
