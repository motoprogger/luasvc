/*
** $Id$
** Lua service (daemon) wrapper
** See the LICENSE file for copyright notice
*/


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"


static void laction (int i);
static void lactionerr (int i);

static lua_State *globalL = NULL;

static const char *progname = "luasvc";
static const char *pidfile = NULL;

static void* const REGISTRY_KEY = (void*) &globalL;
static const char  FUNCTION_RUN[] = "run";
static const char  FUNCTION_STOP[] = "stop";
static const char* const methods[] = {FUNCTION_RUN, FUNCTION_STOP, NULL};

static int stop = 0;
static int stopping = 0;

static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  if (!lua_checkstack(L, 2))
    luaL_error(L, "Out of stack space");
  lua_pushlightuserdata(L, REGISTRY_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, FUNCTION_STOP);
  lua_remove(L, -2);
  lua_call(L, 0, 0);
  signal(SIGHUP, laction);
}

static void laction (int i) {
  if (i!=SIGHUP) {
    signal(SIGINT, SIG_DFL); /* if another SIGINT or SIGTERM happens before */
    signal(SIGTERM, SIG_DFL);/* lstop, terminate process (default action) */
  }
  signal(SIGHUP, SIG_IGN); /* if another SIGHUP happens before lstop, 
                              ignore it */
  if (i!=SIGHUP && !stop)
    stop = 1;

  if (!stopping) {
    stopping=1;
    lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
  }
}

static void lstoperr (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "interrupted!");
}

static void lactionerr (int i) {
    signal(SIGINT, SIG_DFL);
    lua_sethook(globalL, lstoperr, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}

static void print_usage (void) {
  fprintf(stderr,
  "usage: %s [options] [script [args]].\n"
  "Available options are:\n"
  "  -p pidfile  execute string " LUA_QL("stat") "\n"
  "  -l name     require library " LUA_QL("name") "\n"
  "  --       stop handling options\n"
  ,
  progname);
  fflush(stderr);
}


static void l_message (const char *pname, const char *msg) {
  if (pname) fprintf(stderr, "%s: ", pname);
  fprintf(stderr, "%s\n", msg);
  fflush(stderr);
}


static int report (lua_State *L, int status) {
  if (status && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) msg = "(error object is not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);
  }
  return status;
}


static int traceback (lua_State *L) {
  if (!lua_isstring(L, 1))  /* 'message' not a string? */
    return 1;  /* keep it intact */
  lua_getglobal(L, "debug");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_getfield(L, -1, "traceback");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushvalue(L, 1);  /* pass error message */
  lua_pushinteger(L, 2);  /* skip this function and traceback */
  lua_call(L, 2, 1);  /* call debug.traceback */
  return 1;
}

static int docall (lua_State *L, int narg, int nret) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, traceback);  /* push traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  signal(SIGINT, lactionerr);
  status = lua_pcall(L, narg, nret, base);
  signal(SIGINT, SIG_DFL);
  lua_remove(L, base);  /* remove traceback function */
  /* force a complete garbage collection in case of errors */
  if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
}

static int docallrun(lua_State *L, int narg, int nret) {
  int status;
  if (!lua_checkstack(L, 2))
    luaL_error(L, "Out of stack space");
  int base = lua_gettop(L) - narg + 1;  /* Last argument index */
  lua_pushcfunction(L, traceback);  /* push traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  lua_pushlightuserdata(L, REGISTRY_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  lua_getfield(L, -1, FUNCTION_RUN);
  lua_insert(L, base+1);
  lua_pop(L, 1);
  status = lua_pcall(L, narg, nret, base);
  lua_remove(L, base);  /* remove traceback function */
  /* force a complete garbage collection in case of errors */
  if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
}

static int getargs (lua_State *L, char **argv, int n) {
  int narg;
  int i;
  int argc = 0;
  while (argv[argc]) argc++;  /* count total number of arguments */
  narg = argc - (n + 1);  /* number of arguments to the script */
  luaL_checkstack(L, narg + 3, "too many arguments to script");
  for (i=n+1; i < argc; i++)
    lua_pushstring(L, argv[i]);
  lua_createtable(L, narg, n + 1);
  for (i=0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - n);
  }
  return narg;
}

static int dolibrary (lua_State *L, const char *name) {
  lua_getglobal(L, "require");
  lua_pushstring(L, name);
  return report(L, docall(L, 1, 0));
}


static int handle_script_ret(lua_State *L) {
  /*
   * Get the table at the top of stack
   * Check if it has the nescessary fields and if they are executable
   * Store the table in the registry
   */
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return luaL_error(L, "Script return value is not a table");
  }
  if (!lua_checkstack(L, 1))
    return luaL_error(L, "Not enough stack space");
  const char* const* method;
  for (method = methods; *method; method++) {
    lua_getfield(L, -1, *method);
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 2);
      return luaL_error(L, "Method %s is not a function or not present", *method);
    }
    lua_pop(L, 1);
  }
  lua_pushlightuserdata(L, REGISTRY_KEY);
  lua_insert(L, -2);
  lua_settable(L, LUA_REGISTRYINDEX);
  return 0;
}

static int mainloop(lua_State *L) {
  signal(SIGINT, laction);
  signal(SIGTERM, laction);
  /* Main cycle */
  while (!stop) {
    stopping = 0;
    signal(SIGHUP, laction);
    report(L, docallrun(L, 0, 0)); /* call run() service method */
    if (!stopping) {
      /* Service terminated unexpectedly, report it */
    }
  }
  return 0;
}

static int handle_script (lua_State *L, char **argv, int n) {
  int status;
  const char *fname;
  int narg = getargs(L, argv, n);  /* collect arguments */
  lua_setglobal(L, "arg");
  fname = argv[n];
  if (strcmp(fname, "-") == 0 && strcmp(argv[n-1], "--") != 0) 
    fname = NULL;  /* stdin */
  status = luaL_loadfile(L, fname);
  lua_insert(L, -(narg+1));
  if (status == 0) {
    status = docall(L, narg, 1);
    if (status == 0)
       status = handle_script_ret(L);
  } else
    lua_pop(L, narg);      
  return report(L, status);
}

/* check that argument has no extra characters at the end */
#define notail(x)	{if ((x)[2] != '\0') return -1;}


static int collectargs (char **argv) {
  int i;
  for (i = 1; argv[i] != NULL; i++) {
    if (argv[i][0] != '-')  /* not an option? */
        return i;
    switch (argv[i][1]) {  /* option */
      case '-':
        notail(argv[i]);
        return (argv[i+1] != NULL ? i+1 : 0);
      case '\0':
        return i;
      case 'l':
        if (argv[i][2] == '\0') {
          i++;
          if (argv[i] == NULL) return -1;
        }
        break;
      case 'p':
	if (pidfile != NULL) return -1;
        if (argv[i][2] == '\0') {
          i++;
          if (argv[i] == NULL) return -1;
	  pidfile = argv[i];
        } else
          pidfile = argv[i]+2;
        break;
      default: return -1;  /* invalid option */
    }
  }
  return 0;
}


static int runargs (lua_State *L, char **argv, int n) {
  int i;
  for (i = 1; i < n; i++) {
    if (argv[i] == NULL) continue;
    lua_assert(argv[i][0] == '-');
    switch (argv[i][1]) {  /* option */
      case 'l': {
        const char *filename = argv[i] + 2;
        if (*filename == '\0') filename = argv[++i];
        lua_assert(filename != NULL);
        if (dolibrary(L, filename))
          return 1;  /* stop if file fails */
        break;
      }
      default: break;
    }
  }
  return 0;
}


struct Smain {
  int argc;
  char **argv;
  int script;
  int status;
};


static int pmain (lua_State *L) {
  struct Smain *s = (struct Smain *)lua_touserdata(L, 1);
  char **argv = s->argv;
  globalL = L;
  if (argv[0] && argv[0][0]) progname = argv[0];
  lua_gc(L, LUA_GCSTOP, 0);  /* stop collector during initialization */
  luaL_openlibs(L);  /* open libraries */
  lua_gc(L, LUA_GCRESTART, 0);
  s->status = runargs(L, argv, s->script);
  if (s->status != 0) return 0;
  s->status = handle_script(L, argv, s->script);
  if (s->status != 0) return 0;
  s->status = mainloop(L);
  return 0;
}


static int writepid(const char *filename, pid_t pid) {
  FILE* pidfile_f = fopen(filename, "w");
  if (pidfile_f==NULL) return -1;
  int res = fprintf(pidfile_f, "%i", pid);
  int res2 = fclose(pidfile_f);
  if (res<0 || res2!=0)
    return -1;
  return 0;
}


int main (int argc, char **argv) {
  int status;
  struct Smain s;
  s.argc = argc;
  s.argv = argv;
  s.script = collectargs(argv);
  if (s.script <= 0) {  /* invalid args? */
    print_usage();
    return EXIT_FAILURE;
  }
  pid_t cpid = fork();
  if (cpid>0)
    return EXIT_SUCCESS;
  if (cpid<0) {
    l_message(argv[0], "cannot create child process: not enough memory");
    return EXIT_FAILURE;
  }
  cpid = getpid();
  if (pidfile) {
    if (writepid(pidfile, cpid)<0) {
      l_message(argv[0], "cannot write pidfile");
    }
  }
  lua_State *L = luaL_newstate();  /* create state */
  if (L == NULL) {
    l_message(argv[0], "cannot create state: not enough memory");
    return EXIT_FAILURE;
  }
  lua_pushcfunction(L, &pmain);
  lua_pushlightuserdata(L, &s);
  status = lua_pcall(L, 1, 0, 0);
  report(L, status);
  lua_close(L);
  return (status || s.status) ? EXIT_FAILURE : EXIT_SUCCESS;
}

