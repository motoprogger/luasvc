luasvc: luasvc.c
	$(CC) -o $@ -L$(LUA_LIBDIR) -I$(LUA_INCDIR) -l$(LUALIB) $<
