
#include "config.h"

#include <stdio.h>

#if defined (HAVE_UNISTD_H)
#  ifdef _MINIX
#    include <sys/types.h>
#  endif
#  include <unistd.h>
#endif

#include "shell.h"
#include "flags.h"
#include "builtins.h"
#include "builtins/common.h"
#include "builtins/builtext.h"

#include "y.tab.h"

#include "compiler.h"

#define NYI(...) internal_warning("NYI: compilation of "__VA_ARGS__)

#define __must_use __attribute__((warn_unused_result))

static const char bashc_header[] =
"/* This file generated by bashc */\n"
"#include <stdlib.h>\n"
"#include <stdio.h>\n"
"#include <unistd.h>\n"
"#include <sys/types.h>\n"
"#include <sys/wait.h>\n"
"\n"
"#include \"libbashc/libbashc.h\"\n"
"\n"
"int main(int argc, char** argv)\n"
"{\n"
	"\tint G_status;\n"
	"\n"
	"\t(void)argc;\n"
	"\t(void)argv;\n"
	"\tG_status = 0;\n"
	"\n"
;

static const char bashc_footer[] =
	"\treturn G_status;\n"
"}\n"
;

static int indent_level = 0;

static void indent(void)
{
	int i;
	for (i = 0; i < indent_level; i++) fputc('\t',bashc_output);
}

#define cout(...) fprintf(bashc_output,__VA_ARGS__)
#define coutn(...) do { fprintf(bashc_output,__VA_ARGS__); cout("\n"); } while (0)
#define coutsn(...) do { fprintf(bashc_output,__VA_ARGS__); cout(";\n"); } while (0)
#define icout(...) do { indent(); cout(__VA_ARGS__); } while (0)
#define icoutsn(...) do { indent(); cout(__VA_ARGS__); cout(";\n"); } while (0)

#define make_cif(...) do { \
		indent(); \
		cout("if ("); \
		cout(__VA_ARGS__); \
		cout(") {\n"); \
		++indent_level; \
	} while (0)

#define make_celseif(...) do { \
		--indent_level; \
		indent(); \
		cout("} else if ("); \
		cout(__VA_ARGS__); \
		cout(") {\n"); \
		++indent_level; \
	} while (0)

#define make_celse() do { \
		--indent_level; \
		indent(); \
		cout("} else {\n"); \
		++indent_level; \
	} while (0)

#define make_cendif() do { --indent_level; indent(); cout("}\n"); } while (0)

#define ccomment(...) do { \
		indent(); \
		cout("/* "); \
		cout(__VA_ARGS__); \
		cout(" */\n"); \
	} while (0)

#define startblock() do { icout("{\n"); ++indent_level; } while (0)
#define endblock() do { --indent_level; icout("}\n"); } while (0)

static void cencode_string(const char* str)
{
	int i;

	for (i = 0; str[i]; i++) {
		switch (str[i]) {
		case '"': cout("\\\""); break;
		case '\n': cout("\\n"); break;
		case '\r': cout("\\r"); break;
		case '\t': cout("\\t"); break;
		case '\f': cout("\\f"); break;
		case '\v': cout("\\v"); break;
		case '\a': cout("\\a"); break;
		case '\b': cout("\\b"); break;

		default:
			if (isprint(str[i]))
				cout("%c",str[i]);
			else
				cout("\\x%02hhx",str[i]);
		}
	}
}

/* Returns a pointer to a malloc()ed string of the name of a new
 * identifier, with option "base" name */
static __must_use char* new_ident(const char* base)
{
	static unsigned int idnum = 0;
	const char* baseid = base ? base : "var";
	char* str;
	asprintf(&str,"%s%u",baseid,idnum++);
	return str;
}

static void wordlist_to_cstr_array(WORD_LIST* wds, int addnullterm)
{
	WORD_LIST* wd;

	cout("{ ");

	for (wd = wds; wd; wd = wd->next) {
		cout("\"");
		cencode_string(wd->word->word);
		cout("\", ");
	}

	if (addnullterm)
		cout("NULL, ");

	cout("}");
}

/* Compile-time I/O context.  Strings put in the fdnames array should
 * always be malloced. */
struct ctioctx {
	int numfds;
	char* fdnames[][2];
};

static __must_use struct ctioctx* new_ioc(int numfds)
{
	struct ctioctx* ioc = malloc(sizeof(struct ctioctx)
	                             + numfds*sizeof(ioc->fdnames[0]));
	ioc->numfds = numfds;
	return ioc;
}

static __must_use struct ctioctx* merge_iocs(struct ctioctx* a, struct ctioctx* b)
{
	int i,j;
	struct ctioctx* ioc;

	if (!a) return b;
	if (!b) return a;

	ioc = new_ioc(a->numfds + b->numfds);
	for (i = 0; i < a->numfds; i++) {
		ioc->fdnames[i][0] = a->fdnames[i][0];
		ioc->fdnames[i][1] = a->fdnames[i][1];
	}
	for (j = 0; j < b->numfds; j++) {
		ioc->fdnames[i+j][0] = b->fdnames[j][0];
		ioc->fdnames[i+j][1] = b->fdnames[j][1];
	}
	return ioc;
}

/*
 * Make room for 'n' more entries.  Alternately, if 'n' is negative,
 * shrink by that many entries, freeing the entries in the freed
 * slots.  Returns a possibly-relocated `ioc', or NULL on error or
 * shrinking to zero size.
 */
static __must_use struct ctioctx* ioc_grow(struct ctioctx* ioc, int n)
{
	int old,i;

	old = ioc ? ioc->numfds : 0;

	if (old + n < 0)
		fatal_error("tried to shrink ctioctx below zero size");

	if (n < 0) {
		for (i = old + n; i < old; i++) {
			free(ioc->fdnames[i][0]);
			free(ioc->fdnames[i][1]);
		}
	}

	if (old + n == 0) {
		free(ioc);
		ioc = NULL;
	} else {
		ioc = realloc(ioc,sizeof(struct ctioctx) + (old+n)*sizeof(ioc->fdnames[0]));
		if (ioc)
			ioc->numfds = old + n;
	}

	return ioc;
}

static void make_rtioctx(struct ctioctx* ioc, const char* name)
{
	int i,num;

	num = ioc ? ioc->numfds : 0;

	if (num) {
		icoutsn("struct rtioctx* %s = malloc(sizeof(struct rtioctx) + "
		        "%d*sizeof(%s->fds[0]))",name,num,name);
		icoutsn("%s->numfds = %d",name,ioc->numfds);
		for (i = 0; i < ioc->numfds; i++) {
			icoutsn("%s->fds[%d][0] = %s",name,i,ioc->fdnames[i][0]);
			icoutsn("%s->fds[%d][1] = %s",name,i,ioc->fdnames[i][1]);
		}
	} else {
		icoutsn("struct rtioctx* %s = NULL",name);
	}
}

/* Constants for flags arguments to compile_* functions */
#define CF_BACKGROUND 1

static struct ctioctx* compile_simple_command(COMMAND* cmd, int override_builtin,
                                              struct ctioctx* ioc, int flags);
static struct ctioctx* compile_command(COMMAND* cmd, struct ctioctx* ioc, int flags);

static void comment_simple_command(struct simple_com* sc)
{
	WORD_LIST* word;

	icout("/* $ ");
	for (word = sc->words; word; word = word->next)
		cout("%s%s",word->word->word,word->next ? " " : "");
	cout(" */\n");
}

static __must_use struct ctioctx* compile_builtin(sh_builtin_func_t* builtin,
                                                  COMMAND* cmd, struct ctioctx* ioc,
                                                  int flags)
{
	struct simple_com* sc = cmd->value.Simple;

	if (builtin == cd_builtin) {
		comment_simple_command(sc);
		make_cif("chdir(\"%s\")",sc->words->next->word->word);
		icoutsn("perror(\"chdir\")");
		make_cendif();
		cout("\n");
	} else if (builtin == echo_builtin
	           || builtin == false_builtin
	           || builtin == colon_builtin) {
		/* cheat and use system binaries for now */
		compile_simple_command(cmd,1,ioc,flags);
	} else {
		NYI("%s builtin",sc->words->word->word);
	}

	return ioc;
}

/* return the variable identifier */
static __must_use char* build_argv(WORD_LIST* wds)
{
	char* argvname = new_ident("argv");
	icout("static char* const %s[] = ",argvname);
	wordlist_to_cstr_array(wds,1);
	cout(";\n");
	return argvname;
}

static void output_flags(int f)
{
	cout("0");
	if (f & CF_BACKGROUND) cout("|FE_BACKGROUND");
}

/* FIXME: 'override_builtin' is an ugly hack. */
static __must_use  struct ctioctx* compile_simple_command(COMMAND* cmd,
                                                          int override_builtin,
                                                          struct ctioctx* ioc, int flags)
{
	sh_builtin_func_t* builtin;
	struct simple_com* sc = cmd->value.Simple;
	char* argvname;
	char* rtiocname;

	if (sc->redirects) {
		NYI("redirects");
		return ioc;
	}

	if (!override_builtin &&
	    (builtin = find_shell_builtin(sc->words->word->word)))
		return compile_builtin(builtin,cmd,ioc,flags);

	comment_simple_command(sc);

	startblock();
	argvname = build_argv(sc->words);

	rtiocname = new_ident("rtioc");
	make_rtioctx(ioc,rtiocname);

	icout("G_status = %sforkexec_argv(%s,%s,",
	      (cmd->flags & CMD_INVERT_RETURN) ? "!" : "",argvname,rtiocname);
	output_flags(flags);
	coutsn(")");

	endblock();
	cout("\n");

	free(rtiocname);
	free(argvname);

	return ioc;
}

static __must_use struct ctioctx* compile_pipe(COMMAND* first, COMMAND* second,
                                               struct ctioctx* ioc, int flags)
{
	char* pipeends;
	char* pidname;
	int ofst;

	pipeends = new_ident("pipe");
	pidname = new_ident("pid");

	startblock();

	icoutsn("int %s[2]",pipeends);
	icoutsn("pid_t %s",pidname);

	make_cif("!pipe(%s)",pipeends);

	ioc = ioc_grow(ioc,2);
	ofst = ioc->numfds - 2;
	asprintf(&ioc->fdnames[ofst][0],"%s[1]",pipeends);
	ioc->fdnames[ofst][1] = strdup("1");
	asprintf(&ioc->fdnames[ofst+1][0],"%s[0]",pipeends);
	ioc->fdnames[ofst+1][1] = strdup("IO_CLOSE_FD");

	ioc = compile_command(first,ioc,flags);
	ioc = ioc_grow(ioc,-2);

	icoutsn("close(%s[1])",pipeends);

	ioc = ioc_grow(ioc,1);
	ofst = ioc->numfds - 1;
	asprintf(&ioc->fdnames[ofst][0],"%s[0]",pipeends);
	ioc->fdnames[ofst][1] = strdup("0");

	ioc = compile_command(second,ioc,flags);
	ioc = ioc_grow(ioc,-1);

	icoutsn("close(%s[0])",pipeends);

	make_celse();

	icoutsn("perror(\"pipe\")");

	make_cendif();

	endblock();
	cout("\n");

	free(pipeends);
	free(pidname);

	return ioc;
}

static __must_use struct ctioctx* compile_connection(COMMAND* cmd,
                                                     struct ctioctx* ioc, int flags)
{
	struct connection* conn = cmd->value.Connection;

	switch (conn->connector) {

	case ';':
		ioc = compile_command(conn->first,ioc,flags);
		ioc = compile_command(conn->second,ioc,flags);
		break;

	case '|':
		ioc = compile_pipe(conn->first,conn->second,ioc,flags);
		break;

	case '&':
		ioc = compile_command(conn->first,ioc,flags|CF_BACKGROUND);
		ioc = compile_command(conn->second,ioc,flags);
		break;

	case AND_AND:
		ioc = compile_command(conn->first,ioc,flags);
		make_cif("!G_status");
		ioc = compile_command(conn->second,ioc,flags);
		make_cendif();
		break;

	case OR_OR:
		ioc = compile_command(conn->first,ioc,flags);
		make_cif("G_status");
		ioc = compile_command(conn->second,ioc,flags);
		make_cendif();
		break;

	default:
		fatal_error("bad connector type in compile_connection");
		
	}

	return ioc;
}

static __must_use struct ctioctx* compile_if(COMMAND* cmd, struct ctioctx* ioc,
                                             int flags)
{
	struct if_com* ifc = cmd->value.If;

	compile_command(ifc->test,ioc,flags);

	make_cif("!G_status");

	compile_command(ifc->true_case,ioc,flags);

	if (ifc->false_case) {
		make_celse();
		compile_command(ifc->false_case,ioc,flags);
	}

	make_cendif();

	return ioc;
}

static __must_use struct ctioctx* compile_while(COMMAND* cmd, struct ctioctx* ioc,
                                                int flags, int invert)
{
	char* entrypt;
	char* exitpt;
	char* loopstatus;
	struct while_com* wh = cmd->value.While;

	entrypt = new_ident("whileentry");
	exitpt = new_ident("whileexit");
	loopstatus = new_ident("whilestatus");

	icoutsn("int %s = 0",loopstatus);
	coutn("%s:",entrypt);

	startblock();
	compile_command(wh->test,ioc,flags);

	make_cif("%sG_status",invert ? "!" : "");
	icoutsn("G_status = %s",loopstatus);
	icoutsn("goto %s",exitpt);
	make_cendif();

	compile_command(wh->action,ioc,flags);
	icoutsn("%s = G_status",loopstatus);
	icoutsn("goto %s",entrypt);
	endblock();

	coutn("%s:",exitpt);

	return ioc;
}

static __must_use struct ctioctx* compile_command(COMMAND* cmd, struct ctioctx* ioc,
                                                  int flags)
{

	switch (cmd->type) {

	case cm_for:
	case cm_case:
	case cm_select:
	case cm_function_def:
	case cm_group:
	case cm_arith:
	case cm_cond:
	case cm_arith_for:
	case cm_subshell:
	case cm_coproc:
		NYI("(command type %d)",cmd->type);
		break;

	case cm_until:
		ioc = compile_while(cmd,ioc,flags,1);
		break;

	case cm_while:
		ioc = compile_while(cmd,ioc,flags,0);
		break;

	case cm_if:
		ioc = compile_if(cmd,ioc,flags);
		break;

	case cm_simple:
		ioc = compile_simple_command(cmd,0,ioc,flags);
		break;

	case cm_connection:
		ioc = compile_connection(cmd,ioc,flags);
		break;

	default:
		fatal_error("bad command type in compile_command()");
	}

	return ioc;
}

static void init_compiler_output(void)
{
	fputs(bashc_header,bashc_output);
	indent_level = 1;
}

static void finish_compiler_output(void)
{
	fputs(bashc_footer,bashc_output);
	indent_level = 0;
}

int compile_input(void)
{
	int ret;
	struct ctioctx* ioc = NULL;

	if (!(bashc_output = fopen(bashc_outpath,"w"))) {
		report_error("Failed to open %s for writing",bashc_outpath);
		exit_shell(EX_NOTFOUND);
	}

	init_compiler_output();
	
	while (!EOF_Reached) {
		if (read_command()) {
			ret = 1;
			EOF_Reached = EOF;
		} else if (global_command) {
			ioc = compile_command(global_command,ioc,0);
			dispose_command(global_command);
			global_command = NULL;
		}

		if (just_one_command)
			EOF_Reached = EOF;
	}

	finish_compiler_output();
	/* FIXME: free ioc */

	if (fclose(bashc_output)) {
		report_error("failed to close %s",bashc_outpath);
		return 1;
	}

	return 0;
}
