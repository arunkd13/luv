#include "zmq.h"
#include "zmq_utils.h"

#include "luv_core.h"
#include "luv_cond.h"
#include "luv_object.h"
#include "luv_zmq.h"

static int luv_new_zmq(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  int nthreads = luaL_optinteger(L, 2, 1);

  luv_object_t* self = lua_newuserdata(L, sizeof(luv_object_t));
  luaL_getmetatable(L, LUV_ZMQ_CTX_T);
  lua_setmetatable(L, -2);

  luv__object_init(sched, self);

  self->data = zmq_ctx_new();
  zmq_ctx_set(self->data, ZMQ_IO_THREADS, nthreads);

  return 1;
}

inline static int luv__zmq_socket_readable(void* socket) {
  zmq_pollitem_t items[1];
  items[0].socket = socket;
  items[0].events = ZMQ_POLLIN;
  return zmq_poll(items, 1, 0);
}
inline static int luv__zmq_socket_writable(void* socket) {
  zmq_pollitem_t items[1];
  items[0].socket = socket;
  items[0].events = ZMQ_POLLOUT;
  return zmq_poll(items, 1, 0);
}

static void luv__buf_free(void* base, void* hint) {
  free(base);
}
static int luv__zmq_socket_send(luv_object_t* self, luv_state_t* state) {
  size_t    len;
  zmq_msg_t msg;

  const char* data = luaL_checklstring(state->L, 2, &len);
  if (zmq_msg_init_size(&msg, len)) {
    /* ENOMEM */
    return luaL_error(state->L, strerror(errno));
  }

  memcpy(zmq_msg_data(&msg), data, len);
  int rv = zmq_msg_send(&msg, self->data, ZMQ_DONTWAIT);
  zmq_msg_close(&msg);

  return rv;
}

static int luv__zmq_socket_recv(luv_object_t* self, luv_state_t* state) {
  zmq_msg_t msg;
  zmq_msg_init(&msg);

  int rv = zmq_msg_recv(&msg, self->data, ZMQ_DONTWAIT);
  if (rv < 0) {
    zmq_msg_close(&msg);
  }
  else {
    void* data = zmq_msg_data(&msg);
    size_t len = zmq_msg_size(&msg);
    lua_settop(state->L, 0);
    lua_pushlstring(state->L, data, len);
    zmq_msg_close(&msg);
  }
  return rv;
}

static void luv_zmq_recv_cb(uv_poll_t* handle, int status, int events) {
  luv_object_t* self = container_of(handle, luv_object_t, h);

  int readable = luv__zmq_socket_readable(self->data);
  if (readable == 0) return;

  uv_poll_stop(handle);

  ngx_queue_t* queue = ngx_queue_head(&self->rouse);
  luv_state_t* state = ngx_queue_data(queue, luv_state_t, cond);
  ngx_queue_remove(queue);

  if (readable < 0) {
    lua_settop(state->L, 0);
    lua_pushboolean(state->L, 0);
    lua_pushstring(state->L, strerror(errno));
  }
  else if (readable > 0) {
    int rv = luv__zmq_socket_recv(self, state);
    if (rv < 0) {
      if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
        lua_settop(state->L, 0);
        lua_pushboolean(state->L, 0);
        lua_pushstring(state->L, strerror(errno));
      }
    }
  }
  luv__state_resume(state);
}

static void luv_zmq_send_cb(uv_poll_t* handle, int status, int events) {
  luv_object_t* self = container_of(handle, luv_object_t, h);

  int writable = luv__zmq_socket_writable(self->data);
  if (writable == 0) return;

  uv_poll_stop(handle);

  ngx_queue_t* queue = ngx_queue_head(&self->queue);
  luv_state_t* state = ngx_queue_data(queue, luv_state_t, cond);
  ngx_queue_remove(queue);

  if (writable < 0) {
    lua_settop(state->L, 0);
    lua_pushboolean(state->L, 0);
    lua_pushstring(state->L, strerror(errno));
  }
  else if (writable > 0) {
    int rv = luv__zmq_socket_send(self, state);
    if (rv < 0) {
      lua_settop(state->L, 0);
      lua_pushboolean(state->L, 0);
      lua_pushstring(state->L, strerror(errno));
    }
  }
  luv__state_resume(state);
}

/* socket methods */
static int luv_zmq_ctx_socket(lua_State* L) {
  luv_object_t* ctx = lua_touserdata(L, 1);
  int type = luaL_checkint(L, 2);

  luv_object_t* self = lua_newuserdata(L, sizeof(luv_object_t));
  luaL_getmetatable(L, LUV_ZMQ_SOCKET_T);
  lua_setmetatable(L, -2);

  luv__object_init(ctx->sched, self);

  self->data = zmq_socket(ctx->data, type);

  uv_os_sock_t socket;
  size_t len = sizeof(uv_os_sock_t);
  zmq_getsockopt(self->data, ZMQ_FD, &socket, &len);

  uv_poll_init_socket(self->sched->loop, &self->h.poll, socket);
  return 1;
}

static int luv_zmq_socket_bind(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  const char*   addr = luaL_checkstring(L, 2);
  /* XXX: make this async? */
  int rv = zmq_bind(self->data, addr);
  lua_pushinteger(L, rv);
  return 1;
}
static int luv_zmq_socket_connect(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  const char*   addr = luaL_checkstring(L, 2);
  /* XXX: make this async? */
  int rv = zmq_connect(self->data, addr);
  lua_pushinteger(L, rv);
  return 1;
}

static int luv_zmq_socket_send(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  luv_state_t*  curr = luv__sched_current(self->sched);
  int rv = luv__zmq_socket_send(self, curr);
  if (rv < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      uv_poll_start(&self->h.poll, UV_READABLE, luv_zmq_send_cb);
      luv__cond_wait(&self->queue, curr);
      return luv__state_yield(curr, 2);  
    }
    else {
      lua_settop(L, 0);
      lua_pushboolean(L, 0);
      lua_pushstring(L, strerror(errno));
    }
  }
  return 2;
}
static int luv_zmq_socket_recv(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  luv_state_t*  curr = luv__sched_current(self->sched);
  int rv = luv__zmq_socket_recv(self, curr);
  if (rv < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      uv_poll_start(&self->h.poll, UV_READABLE, luv_zmq_recv_cb);
      luv__cond_wait(&self->rouse, curr);
      return luv__state_yield(curr, LUA_MULTRET);  
    }
    else {
      lua_settop(L, 0);
      lua_pushboolean(L, 0);
      lua_pushstring(L, strerror(errno));
      return 2;
    }
  }
  return 1;
}

static int luv_zmq_socket_close(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  if (!(self->flags & LUV_ZMQ_SCLOSED)) {
    if (zmq_close(self->data)) {
      /* TODO: linger and error handling */
    }
    uv_poll_stop(&self->h.poll);
    self->flags |= LUV_ZMQ_SCLOSED;
  }
  return 1;
}

static const char* LUV_ZMQ_SOCKOPTS[] = {
  "",                     /* 0 */
  "",                     /* 1 */
  "",                     /* 2 */
  "",                     /* 3 */
  "AFFINITY",             /* 4 */
  "IDENTITY",             /* 5 */
  "SUBSCRIBE",            /* 6 */
  "UNSUBSCRIBE",          /* 7 */
  "RATE",                 /* 8 */
  "RECOVERY_IVL",         /* 9 */
  "",                     /* 3 */
  "SNDBUF",               /* 11 */
  "RCVBUF",               /* 12 */
  "RCVMORE",              /* 13 */
  "FD",                   /* 14 */
  "EVENTS",               /* 15 */
  "TYPE",                 /* 16 */
  "LINGER",               /* 17 */
  "RECONNECT_IVL",        /* 18 */
  "BACKLOG",              /* 19 */
  "",                     /* 20 */
  "RECONNECT_IVL_MAX",    /* 21 */
  "MAXMSGSIZE",           /* 22 */
  "SNDHWM",               /* 23 */
  "RCVHWM",               /* 24 */
  "MULTICAST_HOPS",       /* 25 */
  "",                     /* 26 */
  "RCVTIMEO",             /* 27 */
  "SNDTIMEO",             /* 28 */
  "",                     /* 29 */
  "",                     /* 30 */
  "IPV4ONLY",             /* 31 */
  "LAST_ENDPOINT",        /* 32 */
  "ROUTER_BEHAVIOR",      /* 33 */
  "TCP_KEEPALIVE",        /* 34 */
  "TCP_KEEPALIVE_CNT",    /* 35 */
  "TCP_KEEPALIVE_IDLE",   /* 36 */
  "TCP_KEEPALIVE_INTVL",  /* 37 */
  "TCP_ACCEPT_FILTER",    /* 38 */
  NULL
};

static int luv_zmq_socket_setsockopt(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  int opt = luaL_checkoption(L, 1, NULL, LUV_ZMQ_SOCKOPTS);
  int rv;
  switch (opt) {
    case ZMQ_SNDHWM:
    case ZMQ_RCVHWM:
    case ZMQ_RATE:
    case ZMQ_RECOVERY_IVL:
    case ZMQ_SNDBUF:
    case ZMQ_RCVBUF:
    case ZMQ_LINGER:
    case ZMQ_RECONNECT_IVL:
    case ZMQ_RECONNECT_IVL_MAX:
    case ZMQ_BACKLOG:
    case ZMQ_MULTICAST_HOPS:
    case ZMQ_RCVTIMEO:
    case ZMQ_SNDTIMEO:
    case ZMQ_ROUTER_BEHAVIOR:
    case ZMQ_TCP_KEEPALIVE:
    case ZMQ_TCP_KEEPALIVE_CNT:
    case ZMQ_TCP_KEEPALIVE_IDLE:
    case ZMQ_TCP_KEEPALIVE_INTVL:
    {
      int val = lua_tointeger(L, 2);
      rv = zmq_setsockopt(self->data, opt, &val, sizeof(val));
      break;
    }

    case ZMQ_AFFINITY:
    {
      uint64_t val = (uint64_t)lua_tointeger(L, 2);
      rv = zmq_setsockopt(self->data, opt, &val, sizeof(val));
      break;
    }

    case ZMQ_MAXMSGSIZE:
    {
      int64_t val = (int64_t)lua_tointeger(L, 2);
      rv = zmq_setsockopt(self->data, opt, &val, sizeof(val));
      break;
    }

    case ZMQ_IPV4ONLY:
    {
      int val = lua_toboolean(L, 2);
      rv = zmq_setsockopt(self->data, opt, &val, sizeof(val));
      break;
    }

    case ZMQ_IDENTITY:
    case ZMQ_SUBSCRIBE:
    case ZMQ_UNSUBSCRIBE:
    case ZMQ_TCP_ACCEPT_FILTER:
    {
      size_t len;
      const char* val = lua_tolstring(L, 2, &len);
      rv = zmq_setsockopt(self->data, opt, &val, len);
      break;
    }

    case ZMQ_RCVMORE:
    case ZMQ_FD:
    case ZMQ_EVENTS:
    case ZMQ_TYPE:
    case ZMQ_LAST_ENDPOINT:
      return luaL_error(L, "readonly option");
    default:
      return luaL_error(L, "invalid option");
  }
  if (rv < 0) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, zmq_strerror(zmq_errno()));
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}
static int luv_zmq_socket_getsockopt(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_SOCKET_T);
  int opt = luaL_checkoption(L, 1, NULL, LUV_ZMQ_SOCKOPTS);
  size_t len;
  switch (opt) {
    case ZMQ_TYPE:
    case ZMQ_RCVMORE:
    case ZMQ_SNDHWM:
    case ZMQ_RCVHWM:
    case ZMQ_RATE:
    case ZMQ_RECOVERY_IVL:
    case ZMQ_SNDBUF:
    case ZMQ_RCVBUF:
    case ZMQ_LINGER:
    case ZMQ_RECONNECT_IVL:
    case ZMQ_RECONNECT_IVL_MAX:
    case ZMQ_BACKLOG:
    case ZMQ_MULTICAST_HOPS:
    case ZMQ_RCVTIMEO:
    case ZMQ_SNDTIMEO:
    case ZMQ_ROUTER_BEHAVIOR:
    case ZMQ_TCP_KEEPALIVE:
    case ZMQ_TCP_KEEPALIVE_CNT:
    case ZMQ_TCP_KEEPALIVE_IDLE:
    case ZMQ_TCP_KEEPALIVE_INTVL:
    case ZMQ_EVENTS:
    {
      int val;
      len = sizeof(val);
      zmq_getsockopt(self->data, opt, &val, &len);
      lua_pushinteger(L, val);
      break;
    }

    case ZMQ_AFFINITY:
    {
      uint64_t val = (uint64_t)lua_tointeger(L, 2);
      len = sizeof(val);
      zmq_getsockopt(self->data, opt, &val, &len);
      lua_pushinteger(L, (lua_Integer)val);
      break;
    }

    case ZMQ_MAXMSGSIZE:
    {
      int64_t val = (int64_t)lua_tointeger(L, 2);
      len = sizeof(val);
      zmq_getsockopt(self->data, opt, &val, &len);
      lua_pushinteger(L, (lua_Integer)val);
      break;
    }

    case ZMQ_IPV4ONLY:
    {
      int val = lua_toboolean(L, 2);
      len = sizeof(val);
      zmq_getsockopt(self->data, opt, &val, &len);
      lua_pushboolean(L, val);
      break;
    }

    case ZMQ_IDENTITY:
    case ZMQ_LAST_ENDPOINT:
    {
      char val[1024];
      len = sizeof(val);
      zmq_getsockopt(self->data, opt, val, &len);
      lua_pushlstring(L, val, len);
      break;
    }

    case ZMQ_FD:
    {
      uv_os_sock_t socket;
      len = sizeof(uv_os_sock_t);
      zmq_getsockopt(self->data, ZMQ_FD, &socket, &len);
      /* TODO: give these a metatable */
#ifdef _WIN32
      luv_boxpointer(L, socket);
#else
      luv_boxinteger(L, socket);
#endif
    }

    case ZMQ_SUBSCRIBE:
    case ZMQ_UNSUBSCRIBE:
    case ZMQ_TCP_ACCEPT_FILTER:
      return luaL_error(L, "writeonly option");
    default:
      return luaL_error(L, "invalid option");
  }
  return 1;
}

static int luv_zmq_socket_tostring(lua_State* L) {
  luv_object_t* self = lua_touserdata(L, 1);
  lua_pushfstring(L, "userdata<%s>: %p", LUV_ZMQ_SOCKET_T, self);
  return 1;
}
static int luv_zmq_socket_free(lua_State* L) {
  luv_object_t* self = lua_touserdata(L, 1);
  if (!(self->flags & LUV_ZMQ_SCLOSED)) {
    zmq_close(self->data);
    uv_poll_stop(&self->h.poll);
    self->flags |= LUV_ZMQ_SCLOSED;
  }
  return 1;
}

static int luv_zmq_ctx_xdup(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_CTX_T);
  lua_State*    L2   = (lua_State*)lua_touserdata(L, 2);

  luv_object_t* copy = lua_newuserdata(L2, sizeof(luv_object_t));
  luaL_getmetatable(L2, LUV_ZMQ_CTX_T);
  lua_setmetatable(L2, -2);

  lua_getfield(L2, LUA_REGISTRYINDEX, LUV_SCHED_O);
  luv_sched_t* sched = luaL_checkudata(L2, -1, LUV_SCHED_T);
  lua_pop(L2, 1); /* sched */

  luv__object_init(sched, copy);
  copy->data  = self->data;
  copy->flags = self->flags;

  return 0;
}
static int luv_zmq_ctx_tostring(lua_State* L) {
  luv_object_t* self = lua_touserdata(L, 1);
  lua_pushfstring(L, "userdata<%s>: %p", LUV_ZMQ_CTX_T, self);
  return 1;
}
static int luv_zmq_ctx_free(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_ZMQ_CTX_T);
  zmq_ctx_destroy(self->data);
  return 1;
}

static luaL_Reg luv_zmq_funcs[] = {
  {"create",    luv_new_zmq},
  {NULL,        NULL}
};

static luaL_Reg luv_zmq_ctx_meths[] = {
  {"socket",    luv_zmq_ctx_socket},
  {"__xdup",    luv_zmq_ctx_xdup},
  {"__gc",      luv_zmq_ctx_free},
  {"__tostring",luv_zmq_ctx_tostring},
  {NULL,        NULL}
};

static luaL_Reg luv_zmq_socket_meths[] = {
  {"bind",      luv_zmq_socket_bind},
  {"connect",   luv_zmq_socket_connect},
  {"send",      luv_zmq_socket_send},
  {"recv",      luv_zmq_socket_recv},
  {"close",     luv_zmq_socket_close},
  {"getsockopt",luv_zmq_socket_getsockopt},
  {"setsockopt",luv_zmq_socket_setsockopt},
  {"__gc",      luv_zmq_socket_free},
  {"__tostring",luv_zmq_socket_tostring},
  {NULL,        NULL}
};

static const luv_const_reg_t luv_zmq_consts[] = {
  /* ctx options */
  {"IO_THREADS",        ZMQ_IO_THREADS},
  {"MAX_SOCKETS",       ZMQ_MAX_SOCKETS},

  /* socket types */
  {"REQ",               ZMQ_REQ},
  {"REP",               ZMQ_REP},
  {"DEALER",            ZMQ_DEALER},
  {"ROUTER",            ZMQ_ROUTER},
  {"PUB",               ZMQ_PUB},
  {"SUB",               ZMQ_SUB},
  {"PUSH",              ZMQ_PUSH},
  {"PULL",              ZMQ_PULL},
  {"PAIR",              ZMQ_PAIR},

  /* socket options */
  {"SNDHWM",            ZMQ_SNDHWM},
  {"RCVHWM",            ZMQ_RCVHWM},
  {"AFFINITY",          ZMQ_AFFINITY},
  {"IDENTITY",          ZMQ_IDENTITY},
  {"SUBSCRIBE",         ZMQ_SUBSCRIBE},
  {"UNSUBSCRIBE",       ZMQ_UNSUBSCRIBE},
  {"RATE",              ZMQ_RATE},
  {"RECOVERY_IVL",      ZMQ_RECOVERY_IVL},
  {"SNDBUF",            ZMQ_SNDBUF},
  {"RCVBUF",            ZMQ_RCVBUF},
  {"RCVMORE",           ZMQ_RCVMORE},
  {"FD",                ZMQ_FD},
  {"EVENTS",            ZMQ_EVENTS},
  {"TYPE",              ZMQ_TYPE},
  {"LINGER",            ZMQ_LINGER},
  {"RECONNECT_IVL",     ZMQ_RECONNECT_IVL},
  {"BACKLOG",           ZMQ_BACKLOG},
  {"RECONNECT_IVL_MAX", ZMQ_RECONNECT_IVL_MAX},
  {"RCVTIMEO",          ZMQ_RCVTIMEO},
  {"SNDTIMEO",          ZMQ_SNDTIMEO},
  {"IPV4ONLY",          ZMQ_IPV4ONLY},
  {"ROUTER_BEHAVIOR",   ZMQ_ROUTER_BEHAVIOR},
  {"TCP_KEEPALIVE",     ZMQ_TCP_KEEPALIVE},
  {"TCP_KEEPALIVE_IDLE",ZMQ_TCP_KEEPALIVE_IDLE},
  {"TCP_KEEPALIVE_CNT", ZMQ_TCP_KEEPALIVE_CNT},
  {"TCP_KEEPALIVE_INTVL",ZMQ_TCP_KEEPALIVE_INTVL},
  {"TCP_ACCEPT_FILTER", ZMQ_TCP_ACCEPT_FILTER},

  /* msg options */
  {"MORE",              ZMQ_MORE},

  /* send/recv flags */
  {"DONTWAIT",          ZMQ_DONTWAIT},
  {"SNDMORE",           ZMQ_SNDMORE},

  /* poll events */
  {"POLLIN",            ZMQ_POLLIN},
  {"POLLOUT",           ZMQ_POLLOUT},
  {"POLLERR",           ZMQ_POLLERR},

  /* devices */
  {"STREAMER",          ZMQ_STREAMER},
  {"FORWARDER",         ZMQ_FORWARDER},
  {"QUEUE",             ZMQ_QUEUE},
  {NULL,                0}
};

LUALIB_API int luaopenL_luv_zmq(lua_State *L) {
  /* zmq ctx metatable */
  luaL_newmetatable(L, LUV_ZMQ_CTX_T);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_openlib(L, NULL, luv_zmq_ctx_meths, 0);
  lua_pop(L, 1);

  /* zmq socket metatable */
  luaL_newmetatable(L, LUV_ZMQ_SOCKET_T);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_openlib(L, NULL, luv_zmq_socket_meths, 0);
  lua_pop(L, 1);

  /* zmq */
  luv__new_namespace(L, "luv_zmq");
  lua_getfield(L, LUA_REGISTRYINDEX, LUV_SCHED_O);
  luaL_openlib(L, NULL, luv_zmq_funcs, 1);

  const luv_const_reg_t* c = luv_zmq_consts; 
  for (; c->key; c++) {
    lua_pushinteger(L, c->val);
    lua_setfield(L, -2, c->key);
  }

  /* luv.zmq */
  lua_getfield(L, LUA_REGISTRYINDEX, LUV_REG_KEY);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "zmq");
  lua_pop(L, 1);

  return 1;
}
