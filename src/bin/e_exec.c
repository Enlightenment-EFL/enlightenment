#include "e.h"

#define MAX_OUTPUT_CHARACTERS 5000

/* TODO:
 * - Clear e_exec_instances on shutdown
 * - Clear e_exec_start_pending on shutdown
 * - Create border add handler
 * - Launch .desktop in terminal if .desktop requires it
 */

typedef struct _E_Exec_Launch E_Exec_Launch;
typedef struct _E_Exec_Search E_Exec_Search;
typedef struct _E_Exec_Watch  E_Exec_Watch;
typedef struct _E_Exec_Recent E_Exec_Recent;

struct _E_Exec_Launch
{
   E_Zone     *zone;
   const char *launch_method;
};

struct _E_Exec_Search
{
   E_Exec_Instance *inst;
   Efreet_Desktop  *desktop;
   int              startup_id;
   pid_t            pid;
};

struct _E_Exec_Watch
{
   void        (*func)(void *data, E_Exec_Instance *inst, E_Exec_Watch_Type type);
   const void *data;
   Eina_Bool   delete_me E_BITFIELD;
};

struct _E_Config_Dialog_Data
{
   Efreet_Desktop       *desktop;
   char                 *exec;

   Ecore_Exe_Event_Del   event;
   Ecore_Exe_Event_Data *error;
   Ecore_Exe_Event_Data *read;

   char                 *label, *exit, *signal;
};

struct _E_Exec_Recent
{
   Eina_List *files;
};

/* local subsystem functions */
static E_Exec_Instance *_e_exec_cb_exec(void *data, Efreet_Desktop *desktop, char *exec, int remaining);
static Eina_Bool        _e_exec_cb_expire_timer(void *data);
static Eina_Bool        _e_exec_cb_exit(void *data, int type, void *event);
static Eina_Bool        _e_exec_cb_desktop_update(void *data, int type, void *event);
static void        _e_exec_cb_exec_new_free(void *data, void *event);
static void _e_exec_cb_exec_new_client_free(void *data, void *ev);
static void        _e_exec_cb_exec_del_free(void *data, void *event);
static void _e_exe_instance_watchers_call(E_Exec_Instance *inst, E_Exec_Watch_Type type);
static Eina_Bool        _e_exec_startup_id_pid_find(const Eina_Hash *hash EINA_UNUSED, const void *key EINA_UNUSED, void *value, void *data);

static void             _e_exec_error_dialog(Efreet_Desktop *desktop, const char *exec, Ecore_Exe_Event_Del *event, Ecore_Exe_Event_Data *error, Ecore_Exe_Event_Data *read);
static void             _fill_data(E_Config_Dialog_Data *cfdata);
static void            *_create_data(E_Config_Dialog *cfd);
static void             _free_data(E_Config_Dialog *cfd, E_Config_Dialog_Data *cfdata);
static Evas_Object     *_basic_create_widgets(E_Config_Dialog *cfd, Evas *evas, E_Config_Dialog_Data *cfdata);
static Evas_Object     *_advanced_create_widgets(E_Config_Dialog *cfd, Evas *evas, E_Config_Dialog_Data *cfdata);
static Evas_Object     *_dialog_scrolltext_create(Evas *evas, char *title, Ecore_Exe_Event_Data_Line *lines);
static void             _dialog_save_cb(void *data, void *data2);
static Eina_Bool        _e_exec_instance_free(E_Exec_Instance *inst);

/* local subsystem globals */
static Eina_List *e_exec_start_pending = NULL;
static Eina_Hash *e_exec_instances = NULL;
static int startup_id = 0;

static Ecore_Event_Handler *_e_exec_exit_handler = NULL;
static Ecore_Event_Handler *_e_exec_desktop_update_handler = NULL;

static E_Exec_Instance *(*_e_exec_executor_func)(void *data, E_Zone * zone, Efreet_Desktop * desktop, const char *exec, Eina_List *files, const char *launch_method) = NULL;
static void *_e_exec_executor_data = NULL;

E_API int E_EVENT_EXEC_NEW = -1;
E_API int E_EVENT_EXEC_NEW_CLIENT = -1;
E_API int E_EVENT_EXEC_DEL = -1;

static E_Exec_Recent * _e_exec_recent = NULL;
static Ecore_Idler *_e_exec_recent_idler = NULL;

static void
_e_exec_recent_exists_filter(void)
{
   Eina_List *l, *ll;
   E_Exec_Recent_File *fl;

   if ((!_e_exec_recent) || (!_e_exec_recent->files)) return;
   EINA_LIST_FOREACH_SAFE(_e_exec_recent->files, l, ll, fl)
     {
        if (!ecore_file_exists(fl->file))
          {
             _e_exec_recent->files =
               eina_list_remove_list(_e_exec_recent->files, l);
             eina_stringshare_del(fl->file);
             free(fl);
          }
     }
}

static Eina_Bool
_e_exec_cb_recent_idler(void *data EINA_UNUSED)
{
   char buf[4096];
   FILE *f;
   Eina_List *l;
   E_Exec_Recent_File *fl;

   _e_exec_recent_exists_filter();
   if ((_e_exec_recent) && (_e_exec_recent->files))
     {
        e_user_dir_snprintf(buf, sizeof(buf), "recent-files.txt");
        f = fopen(buf, "w");
        if (f)
          {
             EINA_LIST_FOREACH(_e_exec_recent->files, l, fl)
               {
                  fprintf(f, "%1.0f %s\n", fl->timestamp * 100.0, fl->file);
               }
             fclose(f);
          }
     }
   _e_exec_recent_idler = NULL;
   return EINA_FALSE;
}

static void
_e_exec_recent_load(void)
{
   FILE *f;
   char buf[4096];
   long long timi;

   if (_e_exec_recent) return;

   e_user_dir_snprintf(buf, sizeof(buf), "recent-files.txt");
   f = fopen(buf, "r");
   if (!f) return;
   while (fscanf(f, "%lli %4095[^\n]\n", &timi, buf) == 2)
     {
        E_Exec_Recent_File *fl = calloc(1, sizeof(E_Exec_Recent_File));

        if (!fl) free(fl);
        if (!_e_exec_recent) _e_exec_recent = calloc(1, sizeof(E_Exec_Recent));
        if (!_e_exec_recent)
          {
             free(fl);
             break;
          }
        fl->file = eina_stringshare_add(buf);
        fl->timestamp = (double)timi / 100.0;
        _e_exec_recent->files = eina_list_append(_e_exec_recent->files, fl);
     }
   fclose(f);
}

static void
_e_exec_recent_file_append(const char *file, double tim)
{
   Eina_List *l;
   E_Exec_Recent_File *fl = calloc(1, sizeof(E_Exec_Recent_File));
   E_Exec_Recent_File *fl2;

   if (!fl) return;
   _e_exec_recent_load();
   if (!_e_exec_recent)
     _e_exec_recent = calloc(1, sizeof(E_Exec_Recent));
   if (!_e_exec_recent)
     {
        free(fl);
        return;
     }
   fl->file = eina_stringshare_add(file);
   fl->timestamp = tim;
   EINA_LIST_FOREACH(_e_exec_recent->files, l, fl2)
     {
        if (!strcmp(fl2->file, fl->file))
          {
             _e_exec_recent->files = eina_list_remove_list(_e_exec_recent->files, l);
             eina_stringshare_del(fl2->file);
             free(fl2);
             break;
          }
     }
   _e_exec_recent->files = eina_list_prepend(_e_exec_recent->files, fl);
   if (eina_list_count(_e_exec_recent->files) > 30)
     {
        l = eina_list_last(_e_exec_recent->files);
        if (l)
          {
             fl = l->data;
             _e_exec_recent->files = eina_list_remove_list(_e_exec_recent->files, l);
             eina_stringshare_del(fl->file);
             free(fl);
          }
     }
   if (!_e_exec_recent_idler)
     _e_exec_recent_idler = ecore_idler_add(_e_exec_cb_recent_idler, NULL);
}

static void
_e_exec_recent_clean(void)
{
   E_Exec_Recent_File *fl;

   if (!_e_exec_recent) return;
   EINA_LIST_FREE(_e_exec_recent->files, fl)
     {
        eina_stringshare_del(fl->file);
        free(fl);
     }
   free(_e_exec_recent);
   _e_exec_recent = NULL;
   if (_e_exec_recent_idler) ecore_idler_del(_e_exec_recent_idler);
   _e_exec_recent_idler = NULL;
}

/* externally accessible functions */
EINTERN int
e_exec_init(void)
{
   e_exec_instances = eina_hash_string_superfast_new(NULL);

   _e_exec_exit_handler =
     ecore_event_handler_add(ECORE_EXE_EVENT_DEL, _e_exec_cb_exit, NULL);
   _e_exec_desktop_update_handler =
     ecore_event_handler_add(EFREET_EVENT_DESKTOP_CACHE_UPDATE, _e_exec_cb_desktop_update, NULL);

   E_EVENT_EXEC_NEW = ecore_event_type_new();
   E_EVENT_EXEC_NEW_CLIENT = ecore_event_type_new();
   E_EVENT_EXEC_DEL = ecore_event_type_new();
   return 1;
}

EINTERN int
e_exec_shutdown(void)
{
   _e_exec_recent_clean();
   if (_e_exec_exit_handler) ecore_event_handler_del(_e_exec_exit_handler);
   if (_e_exec_desktop_update_handler)
     ecore_event_handler_del(_e_exec_desktop_update_handler);
   eina_hash_free(e_exec_instances);
   eina_list_free(e_exec_start_pending);
   return 1;
}

E_API void
e_exec_executor_set(E_Exec_Instance *(*func)(void *data, E_Zone * zone, Efreet_Desktop * desktop, const char *exec, Eina_List *files, const char *launch_method), const void *data)
{
   _e_exec_executor_func = func;
   _e_exec_executor_data = (void *)data;
}

E_API const Eina_List *
e_exec_recent_files_get(void)
{
   _e_exec_recent_load();
   if (!_e_exec_recent) return NULL;
   _e_exec_recent_exists_filter();
   return _e_exec_recent->files;
}

E_API E_Exec_Instance *
e_exec(E_Zone *zone, Efreet_Desktop *desktop, const char *exec,
       Eina_List *files, const char *launch_method)
{
   E_Exec_Launch *launch;
   E_Exec_Instance *inst = NULL;

   if ((!desktop) && (!exec)) return NULL;

   if (files)
     {
        const char *s;
        Eina_List *l;
        char buf[4096], buf2[8192+128];
        double tim;

        if (getcwd(buf, sizeof(buf)))
          {
             tim = ecore_time_unix_get();
             EINA_LIST_FOREACH(files, l, s)
               {
                  if (s[0] == '/')
                    _e_exec_recent_file_append(s, tim);
                  else
                    {
                       snprintf(buf2, sizeof(buf2), "%s/%s", buf, s);
                       _e_exec_recent_file_append(buf2, tim);
                    }
               }
          }
     }
   if (_e_exec_executor_func)
     return _e_exec_executor_func(_e_exec_executor_data, zone,
                                  desktop, exec, files, launch_method);

   if (desktop)
     {
        const char *single;

        single = eina_hash_find(desktop->x, "X-Enlightenment-Single-Instance");
        if ((single) ||
            (e_config->exe_always_single_instance))
          {
             Eina_Bool dosingle = EINA_FALSE;

             // first take system config for always single instance if set
             if (e_config->exe_always_single_instance) dosingle = EINA_TRUE;

             // and now let desktop file override it
             if (single)
               {
                  if ((!strcasecmp(single, "true")) ||
                      (!strcasecmp(single, "yes")) ||
                      (!strcasecmp(single, "1")))
                    dosingle = EINA_TRUE;
                  else if ((!strcasecmp(single, "false")) ||
                           (!strcasecmp(single, "no")) ||
                           (!strcasecmp(single, "0")))
                    dosingle = EINA_FALSE;
               }

             if (dosingle)
               {
                  const Eina_List *l;
                  E_Client *ec;

                  EINA_LIST_FOREACH(e_comp->clients, l, ec)
                    {
                       if (ec && (ec->desktop == desktop))
                         {
                            if (!ec->focused)
                              e_client_activate(ec, EINA_TRUE);
                            else
                              evas_object_raise(ec->frame);
                            return NULL;
                         }
                    }
               }
          }
     }

   launch = E_NEW(E_Exec_Launch, 1);
   if (!launch) return NULL;
   if (zone)
     {
        launch->zone = zone;
        e_object_ref(E_OBJECT(launch->zone));
     }
   if (launch_method)
     launch->launch_method = eina_stringshare_add(launch_method);

   if (desktop)
     {
        if (exec)
          inst = _e_exec_cb_exec(launch, NULL, strdup(exec), 0);
        else
          {
             if (desktop->exec)
               inst = efreet_desktop_command_get(desktop, files,
                 (Efreet_Desktop_Command_Cb)_e_exec_cb_exec, launch);
             else
               inst = _e_exec_cb_exec(launch, desktop, NULL, 0);
          }
     }
   else
     inst = _e_exec_cb_exec(launch, NULL, strdup(exec), 0);
   if ((zone) && (inst))
     {
        inst->screen = zone->num;
        inst->desk_x = zone->desk_x_current;
        inst->desk_y = zone->desk_y_current;
     }
   return inst;
}

E_API Eina_Bool
e_exec_phony_del(E_Exec_Instance *inst)
{
   if (!inst) return EINA_TRUE;
   EINA_SAFETY_ON_TRUE_RETURN_VAL(!inst->phony, EINA_FALSE);
   inst->ref--;
   _e_exe_instance_watchers_call(inst, E_EXEC_WATCH_STOPPED);
   return _e_exec_instance_free(inst);
}

E_API E_Exec_Instance *
e_exec_phony(E_Client *ec)
{
   E_Exec_Instance *inst;
   Eina_List *l, *lnew;

   if (ec->desktop)
     {
        /* try grouping with previous phony exec */
        l = eina_hash_find(e_exec_instances, ec->desktop->orig_path ?: ec->desktop->name);
        EINA_LIST_FOREACH(l, lnew, inst)
          if (inst && inst->phony)
            {
               e_exec_instance_client_add(inst, ec);
               return inst;
            }
     }

   inst = E_NEW(E_Exec_Instance, 1);
   inst->ref = 1;
   inst->phony = 1;
   inst->desktop = ec->desktop;
   inst->startup_id = ec->netwm.startup_id;
   if (ec->desktop)
     {
        efreet_desktop_ref(ec->desktop);
        inst->key = eina_stringshare_add(ec->desktop->orig_path ?: ec->desktop->name);
     }
   else if (ec->icccm.command.argc)
     {
        Eina_Strbuf *buf;
        int x;

        buf = eina_strbuf_new();
        for (x = 0; x < ec->icccm.command.argc; x++)
          {
             eina_strbuf_append(buf, ec->icccm.command.argv[x]);
             if (x + 1 < ec->icccm.command.argc)
               eina_strbuf_append_char(buf, ' ');
          }
        inst->key = eina_stringshare_add(eina_strbuf_string_get(buf));
        eina_strbuf_free(buf);
     }
   else
     {
        free(inst);
        return NULL;
     }
   inst->used = 1;
   if (ec->zone) inst->screen = ec->zone->num;
   if (ec->desk)
     {
        inst->desk_x = ec->desk->x;
        inst->desk_y = ec->desk->y;
     }
   l = eina_hash_find(e_exec_instances, inst->key);
   lnew = eina_list_append(l, inst);
   if (l) eina_hash_modify(e_exec_instances, inst->key, lnew);
   else eina_hash_add(e_exec_instances, inst->key, lnew);
   inst->ref++;
   ecore_event_add(E_EVENT_EXEC_NEW, inst, _e_exec_cb_exec_new_free, inst);
   e_exec_instance_client_add(inst, ec);
   return inst;
}

E_API E_Exec_Instance *
e_exec_startup_id_pid_instance_find(int id, pid_t pid)
{
   E_Exec_Search search;

   search.inst = NULL;
   search.desktop = NULL;
   search.startup_id = id;
   search.pid = pid;
   eina_hash_foreach(e_exec_instances, _e_exec_startup_id_pid_find, &search);
   return search.inst;
}

E_API Efreet_Desktop *
e_exec_startup_id_pid_find(int id, pid_t pid)
{
   E_Exec_Instance *inst;

   inst = e_exec_startup_id_pid_instance_find(id, pid);
   if (!inst) return NULL;
   return inst->desktop;
}

E_API E_Exec_Instance *
e_exec_startup_desktop_instance_find(Efreet_Desktop *desktop)
{
   E_Exec_Search search;

   search.inst = NULL;
   search.desktop = desktop;
   search.startup_id = 0;
   search.pid = 0;
   eina_hash_foreach(e_exec_instances, _e_exec_startup_id_pid_find, &search);
   return search.inst;
}

static void
_e_exe_instance_watchers_call(E_Exec_Instance *inst, E_Exec_Watch_Type type)
{
   E_Exec_Watch *iw;
   Eina_List *l, *ln;

   inst->ref++;
   EINA_LIST_FOREACH(inst->watchers, l, iw)
     {
        if (iw->func && (!iw->delete_me)) iw->func((void *)(iw->data), inst, type);
     }
   inst->ref--;
   if (inst->ref == 0)
     {
        EINA_LIST_FOREACH_SAFE(inst->watchers, l, ln, iw)
          {
             if (iw->delete_me)
               {
                  inst->watchers = eina_list_remove_list(inst->watchers, l);
                  free(iw);
               }
          }
     }
}

E_API void
e_exec_instance_found(E_Exec_Instance *inst)
{
   E_FREE_FUNC(inst->expire_timer, ecore_timer_del);
   _e_exe_instance_watchers_call(inst, E_EXEC_WATCH_STARTED);
}

E_API void
e_exec_instance_client_add(E_Exec_Instance *inst, E_Client *ec)
{
   inst->clients = eina_list_append(inst->clients, ec);
   REFD(ec, 2);
   e_object_ref(E_OBJECT(ec));
   ec->exe_inst = inst;
   inst->ref++;
   ecore_event_add(E_EVENT_EXEC_NEW_CLIENT, inst, _e_exec_cb_exec_new_client_free, ec);
}

E_API void
e_exec_instance_watcher_add(E_Exec_Instance *inst, void (*func)(void *data, E_Exec_Instance *inst, E_Exec_Watch_Type type), const void *data)
{
   E_Exec_Watch *iw;

   iw = E_NEW(E_Exec_Watch, 1);
   if (!iw) return;
   iw->func = func;
   iw->data = data;
   inst->watchers = eina_list_append(inst->watchers, iw);
}

E_API void
e_exec_instance_watcher_del(E_Exec_Instance *inst, void (*func)(void *data, E_Exec_Instance *inst, E_Exec_Watch_Type type), const void *data)
{
   E_Exec_Watch *iw;
   Eina_List *l, *ln;

   EINA_LIST_FOREACH_SAFE(inst->watchers, l, ln, iw)
     {
        if ((iw->func == func) && (iw->data == data))
          {
             if (inst->ref == 0)
               {
                  inst->watchers = eina_list_remove_list(inst->watchers, l);
                  free(iw);
                  return;
               }
             else
               {
                  iw->delete_me = EINA_TRUE;
                  return;
               }
          }
     }
}

/* local subsystem functions */
static E_Exec_Instance *
_e_exec_cb_exec(void *data, Efreet_Desktop *desktop, char *exec, int remaining)
{
   E_Exec_Instance *inst = NULL;
   E_Exec_Launch *launch;
   Eina_List *l, *lnew;
   Ecore_Exe *exe = NULL;
   char buf[4096];

   launch = data;
   inst = E_NEW(E_Exec_Instance, 1);
   if (!inst) return NULL;
   inst->ref = 1;

   if (startup_id == 0)
     {
        startup_id = e_exehist_startup_id_get();
        if (startup_id < 0) startup_id = 0;
     }
   if (++startup_id < 1) startup_id = 1;
   e_exehist_startup_id_set(startup_id);
   snprintf(buf, sizeof(buf), "E_START|%i", startup_id);
   e_util_env_set("DESKTOP_STARTUP_ID", buf);

   // don't set vsync for clients - maybe inherited from compositor. fixme:
   // need a way to still inherit from parent env of wm.
   e_util_env_set("__GL_SYNC_TO_VBLANK", NULL);

//// FIXME: seem to be some issues with the pipe and filling up ram - need to
//// check. for now disable.
//   exe = ecore_exe_pipe_run(exec,
//			    ECORE_EXE_PIPE_AUTO | ECORE_EXE_PIPE_READ | ECORE_EXE_PIPE_ERROR |
//			    ECORE_EXE_PIPE_READ_LINE_BUFFERED | ECORE_EXE_PIPE_ERROR_LINE_BUFFERED,
//			    inst);
   if ((desktop) && (desktop->path) && (desktop->path[0]))
     {
        if (!getcwd(buf, sizeof(buf)))
          {
             free(inst);
             e_util_dialog_show
               (_("Run Error"),
               _("Enlightenment was unable to get current directory"));
             return NULL;
          }
        if (chdir(desktop->path))
          {
             free(inst);
             e_util_dialog_show
               (_("Run Error"),
               _("Enlightenment was unable to change to directory:<ps/>"
                 "<ps/>"
                 "%s"),
               desktop->path);
             return NULL;
          }
        exe = e_util_exe_safe_run(exec, inst);
        if (chdir(buf))
          {
             e_util_dialog_show
               (_("Run Error"),
               _("Enlightenment was unable to restore to directory:<ps/>"
                 "<ps/>"
                 "%s"),
               buf);
             free(inst);
             return NULL;
          }
     }
   else if (exec)
     {
        if ((desktop) && (desktop->terminal))
          {
             Efreet_Desktop *tdesktop;

             tdesktop = e_util_terminal_desktop_get();
             if (tdesktop)
               {
                  if (tdesktop->exec)
                    {
                       Eina_Strbuf *sb;

                       sb = eina_strbuf_new();
                       if (sb)
                         {
                            eina_strbuf_append(sb, tdesktop->exec);
                            eina_strbuf_append(sb, " -e ");
                            eina_strbuf_append_escaped(sb, exec);
                            exe = e_util_exe_safe_run
                              (eina_strbuf_string_get(sb), inst);
                            eina_strbuf_free(sb);
                         }
                    }
                  else
                    exe = e_util_exe_safe_run(exec, inst);
                  efreet_desktop_free(tdesktop);
               }
             else
               exe = e_util_exe_safe_run(exec, inst);
          }
        else if (desktop && desktop->url)
          {
             char *sb;
             size_t size = 65536, len;

             sb = malloc(size);
             snprintf(sb, size, "%s/enlightenment_open ", e_prefix_bin_get());
             len = strlen(sb);
             sb = e_util_string_append_quoted(sb, &size, &len, desktop->url);
             exe = e_util_exe_safe_run(sb, inst);
             free(sb);
          }
        else
          exe = e_util_exe_safe_run(exec, inst);
     }

   if (!exe)
     {
        free(inst);
        e_util_dialog_show(_("Run Error"),
                           _("Enlightenment was unable to fork a child process:<ps/>"
                             "<ps/>"
                             "%s"),
                           exec);
        return NULL;
     }
   /* reset env vars */
   if ((launch->launch_method) && (!desktop))
     e_exehist_add(launch->launch_method, exec);
   /* 20 lines at start and end, 20x100 limit on bytes at each end. */
//// FIXME: seem to be some issues with the pipe and filling up ram - need to
//// check. for now disable.
//   ecore_exe_auto_limits_set(exe, 2000, 2000, 20, 20);
   ecore_exe_tag_set(exe, "E/exec");

   if (desktop)
     {
        inst->desktop = desktop;
        efreet_desktop_ref(desktop);
        inst->key = eina_stringshare_add(desktop->orig_path ?: desktop->name);
     }
   else
     inst->key = eina_stringshare_add(exec);
   inst->exe = exe;
   inst->startup_id = startup_id;
   inst->launch_time = ecore_time_get();
   inst->expire_timer = ecore_timer_loop_add(e_config->exec.expire_timeout,
                                        _e_exec_cb_expire_timer, inst);
   l = eina_hash_find(e_exec_instances, inst->key);
   lnew = eina_list_append(l, inst);
   if (l) eina_hash_modify(e_exec_instances, inst->key, lnew);
   else eina_hash_add(e_exec_instances, inst->key, lnew);
   if (inst->desktop && inst->desktop->exec)
     {
        e_exec_start_pending = eina_list_append(e_exec_start_pending,
                                                inst->desktop);
        e_exehist_add(launch->launch_method, inst->desktop->exec);
     }

   if (!remaining)
     {
        if (launch->launch_method) eina_stringshare_del(launch->launch_method);
        if (launch->zone) e_object_unref(E_OBJECT(launch->zone));
        free(launch);
     }
   free(exec);
   inst->ref++;
   ecore_event_add(E_EVENT_EXEC_NEW, inst, _e_exec_cb_exec_new_free, inst);
   return inst;
}

static Eina_Bool
_e_exec_cb_expire_timer(void *data)
{
   E_Exec_Instance *inst;

   inst = data;
   if (inst->desktop)
     e_exec_start_pending = eina_list_remove(e_exec_start_pending,
                                             inst->desktop);
   inst->expire_timer = NULL;
   _e_exe_instance_watchers_call(inst, E_EXEC_WATCH_TIMEOUT);
   return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool
_e_exec_instance_free(E_Exec_Instance *inst)
{
   Eina_List *instances;

   if (inst->ref) return EINA_FALSE;
   E_FREE_LIST(inst->watchers, free);
   if (inst->key)
     {
        instances = eina_hash_find(e_exec_instances, inst->key);
        if (instances)
          {
             instances = eina_list_remove(instances, inst);
             if (instances)
               eina_hash_modify(e_exec_instances, inst->key, instances);
             else
               eina_hash_del_by_key(e_exec_instances, inst->key);
          }
        eina_stringshare_replace(&inst->key, NULL);
     }
   if (!inst->deleted)
     {
        inst->deleted = 1;
        inst->ref++;
        E_LIST_FOREACH(inst->clients, e_object_ref);
        ecore_event_add(E_EVENT_EXEC_DEL, inst, _e_exec_cb_exec_del_free, inst);
        return EINA_FALSE;
     }

   return EINA_TRUE;
}

/*
   static Eina_Bool
   _e_exec_cb_instance_finish(void *data)
   {
   _e_exec_instance_free(data);
   return ECORE_CALLBACK_CANCEL;
   }
 */

static void
_e_exec_cb_exec_new_client_free(void *data, void *ev)
{
   E_Exec_Instance *inst = ev;
   E_Client *ec = data;

   inst->ref--;
   _e_exec_instance_free(inst);
   UNREFD(ec, 1);
   e_object_unref(E_OBJECT(ec));
}

static void
_e_exec_cb_exec_new_free(void *data, void *ev EINA_UNUSED)
{
   E_Exec_Instance *inst = data;

   inst->ref--;
   _e_exec_instance_free(inst);
}

static void
_e_exec_cb_exec_del_free(void *data, void *ev EINA_UNUSED)
{
   E_Exec_Instance *inst = data;
   E_Client *ec;

   inst->ref--;

   if (inst->desktop)
     e_exec_start_pending = eina_list_remove(e_exec_start_pending,
                                             inst->desktop);
   if (inst->expire_timer) ecore_timer_del(inst->expire_timer);

   EINA_LIST_FREE(inst->clients, ec)
     {
        ec->exe_inst = NULL;
        e_object_unref(E_OBJECT(ec));
     }

   if (inst->desktop) efreet_desktop_free(inst->desktop);
   if (!inst->phony)
     {
        if (inst->exe) ecore_exe_data_set(inst->exe, NULL);
     }
   free(inst);
}

static Eina_Bool
_e_exec_cb_exit(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Exe_Event_Del *ev;
   E_Exec_Instance *inst;

   ev = event;
   if (!ev->exe)
     {
        inst = e_exec_startup_id_pid_instance_find(-1, ev->pid);
        if (!inst) return ECORE_CALLBACK_PASS_ON;
        ev->exe = inst->exe;
     }
//   if (ecore_exe_tag_get(ev->exe)) printf("  tag %s\n", ecore_exe_tag_get(ev->exe));
   if (!(ecore_exe_tag_get(ev->exe) &&
         (!strcmp(ecore_exe_tag_get(ev->exe), "E/exec"))))
     return ECORE_CALLBACK_PASS_ON;
   inst = ecore_exe_data_get(ev->exe);
   if (!inst) return ECORE_CALLBACK_PASS_ON;
   if (inst->phony) return ECORE_CALLBACK_RENEW;

   /* /bin/sh uses this if cmd not found */
   if ((ev->exited) &&
       ((ev->exit_code == 127) || (ev->exit_code == 255)))
     {
        if (e_config->exec.show_run_dialog)
          {
             E_Dialog *dia;

             dia = e_dialog_new(NULL,
                                "E", "_e_exec_run_error_dialog");
             if (dia)
               {
                  char buf[4096];

                  e_dialog_title_set(dia, _("Application run error"));
                  snprintf(buf, sizeof(buf),
                           _("Enlightenment was unable to run the application:<ps/>"
                             "<ps/>"
                             "%s<ps/>"
                             "<ps/>"
                             "The application failed to start."),
                           ecore_exe_cmd_get(ev->exe));
                  e_dialog_text_set(dia, buf);
                  e_dialog_button_add(dia, _("OK"), NULL, NULL, NULL);
                  e_dialog_button_focus_num(dia, 1);
                  elm_win_center(dia->win, 1, 1);
                  e_dialog_show(dia);
               }
          }
     }
   /* Let's hope that everything returns this properly. */
   else if (!((ev->exited) && (ev->exit_code == EXIT_SUCCESS)))
     {
        if (e_config->exec.show_exit_dialog)
          {
             /* filter out common exits via signals - int/term/quit. not really
              * worth popping up a dialog for */
             if (!((ev->signalled) &&
                   ((ev->exit_signal == SIGINT) ||
                    (ev->exit_signal == SIGQUIT) ||
                    (ev->exit_signal == SIGTERM)))
                 )
               {
                  /* Show the error dialog with details from the exe. */
                  _e_exec_error_dialog(inst->desktop, ecore_exe_cmd_get(ev->exe), ev,
                                       ecore_exe_event_data_get(ev->exe, ECORE_EXE_PIPE_ERROR),
                                       ecore_exe_event_data_get(ev->exe, ECORE_EXE_PIPE_READ));
               }
          }
     }

/* scripts that fork off children with & break child tracking... but this hack
 * also breaks apps that handle single-instance themselves */
/*
   if ((ecore_time_get() - inst->launch_time) < 2.0)
     {
        inst->exe = NULL;
        if (inst->expire_timer) ecore_timer_del(inst->expire_timer);
        inst->expire_timer = ecore_timer_loop_add(e_config->exec.expire_timeout, _e_exec_cb_instance_finish, inst);
     }
   else
 */
   inst->ref--;
   inst->exe = NULL;
   _e_exe_instance_watchers_call(inst, E_EXEC_WATCH_STOPPED);
   _e_exec_instance_free(inst);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_exec_cb_desktop_update(void *data EINA_UNUSED, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   const Eina_Hash *execs = e_exec_instances_get();
   Eina_Iterator *it;
   const Eina_List *l, *ll;
   E_Exec_Instance *exe;

   it = eina_hash_iterator_data_new(execs);
   EINA_ITERATOR_FOREACH(it, l)
     {
        EINA_LIST_FOREACH(l, ll, exe)
          {
             if (exe->desktop)
               {
                  efreet_desktop_free(exe->desktop);
                  exe->desktop = NULL;
                  if (exe->key)
                    {
                       exe->desktop = efreet_desktop_get(exe->key);
                       if (!exe->desktop)
                         exe->desktop = efreet_util_desktop_name_find(exe->key);
                    }
               }
          }
     }
   eina_iterator_free(it);
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_e_exec_startup_id_pid_find(const Eina_Hash *hash EINA_UNUSED, const void *key EINA_UNUSED, void *value, void *data)
{
   E_Exec_Search *search;
   E_Exec_Instance *inst;
   Eina_List *l;

   search = data;
   EINA_LIST_FOREACH(value, l, inst)
     {
        pid_t exe_pid;

        exe_pid = 0;
        if (inst->exe)
          {
             exe_pid = ecore_exe_pid_get(inst->exe);
             if (exe_pid <= 0) inst->exe = NULL;
          }
        if (((search->desktop) &&
             (search->desktop == inst->desktop)) ||

            ((search->startup_id > 0) &&
             (search->startup_id == inst->startup_id)) ||

            ((inst->exe) && (search->pid > 1) && (!inst->phony) &&
             (search->pid == exe_pid)))
          {
             search->inst = inst;
             return EINA_FALSE;
          }
     }
   return EINA_TRUE;
}

static void
_e_exec_error_dialog(Efreet_Desktop *desktop, const char *exec, Ecore_Exe_Event_Del *exe_event,
                     Ecore_Exe_Event_Data *exe_error, Ecore_Exe_Event_Data *exe_read)
{
   E_Config_Dialog_View *v;
   E_Config_Dialog_Data *cfdata;

   v = E_NEW(E_Config_Dialog_View, 1);
   if (!v) return;
   cfdata = E_NEW(E_Config_Dialog_Data, 1);
   if (!cfdata)
     {
        E_FREE(v);
        return;
     }
   cfdata->desktop = desktop;
   if (cfdata->desktop) efreet_desktop_ref(cfdata->desktop);
   if (exec) cfdata->exec = strdup(exec);
   cfdata->error = exe_error;
   cfdata->read = exe_read;
   cfdata->event = *exe_event;

   v->create_cfdata = _create_data;
   v->free_cfdata = _free_data;
   v->basic.create_widgets = _basic_create_widgets;
   v->advanced.create_widgets = _advanced_create_widgets;

   /* Create The Dialog */
   e_config_dialog_new(NULL, _("Application Execution Error"),
                       "E", "_e_exec_error_exit_dialog",
                       NULL, 0, v, cfdata);
}

static void
_fill_data(E_Config_Dialog_Data *cfdata)
{
   char buf[4096];

   if (!cfdata->label)
     {
        if (cfdata->desktop)
          snprintf(buf, sizeof(buf), _("%s stopped running unexpectedly."), cfdata->desktop->name);
        else
          snprintf(buf, sizeof(buf), _("%s stopped running unexpectedly."), cfdata->exec);
        cfdata->label = strdup(buf);
     }
   if ((cfdata->event.exited) && (!cfdata->exit))
     {
        snprintf(buf, sizeof(buf),
                 _("An exit code of %i was returned from %s."),
                 cfdata->event.exit_code, cfdata->exec);
        cfdata->exit = strdup(buf);
     }
   if ((cfdata->event.signalled) && (!cfdata->signal))
     {
        if (cfdata->event.exit_signal == SIGINT)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by an Interrupt Signal."),
                   cfdata->exec);
        else if (cfdata->event.exit_signal == SIGQUIT)
          snprintf(buf, sizeof(buf), _("%s was interrupted by a Quit Signal."),
                   cfdata->exec);
        else if (cfdata->event.exit_signal == SIGABRT)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by an Abort Signal."), cfdata->exec);
        else if (cfdata->event.exit_signal == SIGFPE)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by a Floating Point Error."),
                   cfdata->exec);
        else if (cfdata->event.exit_signal == SIGKILL)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by an Uninterruptable Kill Signal."),
                   cfdata->exec);
        else if (cfdata->event.exit_signal == SIGSEGV)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by a Segmentation Fault."),
                   cfdata->exec);
        else if (cfdata->event.exit_signal == SIGPIPE)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by a Broken Pipe."), cfdata->exec);
        else if (cfdata->event.exit_signal == SIGTERM)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by a Termination Signal."),
                   cfdata->exec);
        else if (cfdata->event.exit_signal == SIGBUS)
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by a Bus Error."), cfdata->exec);
        else
          snprintf(buf, sizeof(buf),
                   _("%s was interrupted by the signal number %i."),
                   cfdata->exec, cfdata->event.exit_signal);
        cfdata->signal = strdup(buf);
        /* FIXME: Add  sigchld_info stuff
         * cfdata->event.data
         *    siginfo_t
         *    {
         *       int      si_signo;     Signal number
         *       int      si_errno;     An errno value
         *       int      si_code;      Signal code
         *       pid_t    si_pid;       Sending process ID
         *       uid_t    si_uid;       Real user ID of sending process
         *       int      si_status;    Exit value or signal
         *       clock_t  si_utime;     User time consumed
         *       clock_t  si_stime;     System time consumed
         *       sigval_t si_value;     Signal value
         *       int      si_int;       POSIX.1b signal
         *       void *   si_ptr;       POSIX.1b signal
         *       void *   si_addr;      Memory location which caused fault
         *       int      si_band;      Band event
         *       int      si_fd;        File descriptor
         *    }
         */
     }
}

static void *
_create_data(E_Config_Dialog *cfd)
{
   E_Config_Dialog_Data *cfdata;

   cfdata = cfd->data;
   _fill_data(cfdata);
   return cfdata;
}

static void
_free_data(E_Config_Dialog *cfd EINA_UNUSED, E_Config_Dialog_Data *cfdata)
{
   if (cfdata->error) ecore_exe_event_data_free(cfdata->error);
   if (cfdata->read) ecore_exe_event_data_free(cfdata->read);

   if (cfdata->desktop) efreet_desktop_free(cfdata->desktop);

   E_FREE(cfdata->exec);
   E_FREE(cfdata->signal);
   E_FREE(cfdata->exit);
   E_FREE(cfdata->label);
   E_FREE(cfdata);
}

static Evas_Object *
_dialog_scrolltext_create(Evas *evas, char *title, Ecore_Exe_Event_Data_Line *lines)
{
   Evas_Object *obj, *os;
   char *text;
   char *trunc_note = _("***The remaining output has been truncated. Save the output to view.***\n");
   int tlen, max_lines, i;

   os = e_widget_framelist_add(evas, _(title), 0);

   obj = e_widget_textblock_add(evas);

   tlen = 0;
   for (i = 0; lines[i].line; i++)
     {
        tlen += lines[i].size + 1;
        /* When the program output is extraordinarily long, it can cause
         * significant delays during text rendering. Limit to a fixed
         * number of characters. */
        if (tlen > MAX_OUTPUT_CHARACTERS)
          {
             tlen -= lines[i].size + 1;
             tlen += strlen(trunc_note);
             break;
          }
     }
   max_lines = i;
   text = alloca(tlen + 1);

   text[0] = 0;
   for (i = 0; i < max_lines; i++)
     {
        strcat(text, lines[i].line);
        strcat(text, "\n");
     }

   /* Append the warning about truncated output. */
   if (lines[max_lines].line) strcat(text, trunc_note);

   e_widget_textblock_plain_set(obj, text);
   e_widget_size_min_set(obj, 240, 120);

   e_widget_framelist_object_append(os, obj);

   return os;
}

static Evas_Object *
_basic_create_widgets(E_Config_Dialog *cfd EINA_UNUSED, Evas *evas, E_Config_Dialog_Data *cfdata)
{
   char buf[4096];
   int error_length = 0;
   Evas_Object *o, *ob, *os;

   _fill_data(cfdata);

   o = e_widget_list_add(evas, 0, 0);

   ob = e_widget_label_add(evas, cfdata->label);
   e_widget_list_object_append(o, ob, 1, 1, 0.5);

   if (cfdata->error) error_length = cfdata->error->size;
   if (error_length)
     {
        os = _dialog_scrolltext_create(evas, _("Error Logs"),
                                       cfdata->error->lines);
        e_widget_list_object_append(o, os, 1, 1, 0.5);
     }
   else
     {
        ob = e_widget_label_add(evas, _("There was no error message."));
        e_widget_list_object_append(o, ob, 1, 1, 0.5);
     }

   ob = e_widget_button_add(evas, _("Save This Message"), "system-run",
                            _dialog_save_cb, NULL, cfdata);
   e_widget_list_object_append(o, ob, 0, 0, 0.5);

   if (cfdata->desktop)
     snprintf(buf, sizeof(buf), _("This error log will be saved as %s/%s.log"),
              e_user_homedir_get(), cfdata->desktop->name);
   else
     snprintf(buf, sizeof(buf), _("This error log will be saved as %s/%s.log"),
              e_user_homedir_get(), "Error");
   ob = e_widget_label_add(evas, buf);
   e_widget_list_object_append(o, ob, 1, 1, 0.5);

   return o;
}

static Evas_Object *
_advanced_create_widgets(E_Config_Dialog *cfd EINA_UNUSED, Evas *evas, E_Config_Dialog_Data *cfdata)
{
   char buf[4096];
   int read_length = 0;
   int error_length = 0;
   Evas_Object *o, *of, *ob, *ot;

   _fill_data(cfdata);

   o = e_widget_list_add(evas, 0, 0);
   ot = e_widget_table_add(e_win_evas_win_get(evas), 0);

   ob = e_widget_label_add(evas, cfdata->label);
   e_widget_list_object_append(o, ob, 1, 1, 0.5);

   if (cfdata->exit)
     {
        of = e_widget_framelist_add(evas, _("Error Information"), 0);
        ob = e_widget_label_add(evas, _(cfdata->exit));
        e_widget_framelist_object_append(of, ob);
        e_widget_list_object_append(o, of, 1, 1, 0.5);
     }

   if (cfdata->signal)
     {
        of = e_widget_framelist_add(evas, _("Error Signal Information"), 0);
        ob = e_widget_label_add(evas, _(cfdata->signal));
        e_widget_framelist_object_append(of, ob);
        e_widget_list_object_append(o, of, 1, 1, 0.5);
     }

   if (cfdata->read) read_length = cfdata->read->size;

   if (read_length)
     {
        of = _dialog_scrolltext_create(evas, _("Output Data"),
                                       cfdata->read->lines);
        /* FIXME: Add stdout "start". */
        /* FIXME: Add stdout "end". */
     }
   else
     {
        of = e_widget_framelist_add(evas, _("Output Data"), 0);
        ob = e_widget_label_add(evas, _("There was no output."));
        e_widget_framelist_object_append(of, ob);
     }
   e_widget_table_object_append(ot, of, 0, 0, 1, 1, 1, 1, 1, 1);

   if (cfdata->error) error_length = cfdata->error->size;
   if (error_length)
     {
        of = _dialog_scrolltext_create(evas, _("Error Logs"),
                                       cfdata->error->lines);
        /* FIXME: Add stderr "start". */
        /* FIXME: Add stderr "end". */
     }
   else
     {
        of = e_widget_framelist_add(evas, _("Error Logs"), 0);
        ob = e_widget_label_add(evas, _("There was no error message."));
        e_widget_framelist_object_append(of, ob);
     }
   e_widget_table_object_append(ot, of, 1, 0, 1, 1, 1, 1, 1, 1);

   e_widget_list_object_append(o, ot, 1, 1, 0.5);

   ob = e_widget_button_add(evas, _("Save This Message"), "system-run",
                            _dialog_save_cb, NULL, cfdata);
   e_widget_list_object_append(o, ob, 0, 0, 0.5);

   if (cfdata->desktop)
     snprintf(buf, sizeof(buf), _("This error log will be saved as %s/%s.log"),
              e_user_homedir_get(), cfdata->desktop->name);
   else
     snprintf(buf, sizeof(buf), _("This error log will be saved as %s/%s.log"),
              e_user_homedir_get(), "Error");
   ob = e_widget_label_add(evas, buf);
   e_widget_list_object_append(o, ob, 1, 1, 0.5);

   return o;
}

static void
_dialog_save_cb(void *data EINA_UNUSED, void *data2)
{
   E_Config_Dialog_Data *cfdata;
   FILE *f;
   char *text;
   char buf[1024];
   char buffer[4096];
   int read_length = 0;
   int i, tlen;

   cfdata = data2;

   if (cfdata->desktop)
     snprintf(buf, sizeof(buf), "%s/%s.log", e_user_homedir_get(),
              e_util_filename_escape(cfdata->desktop->name));
   else
     snprintf(buf, sizeof(buf), "%s/%s.log", e_user_homedir_get(),
              "Error");
   f = fopen(buf, "w");
   if (!f) return;

   if (cfdata->exit)
     {
        snprintf(buffer, sizeof(buffer), "Error Information:\n\t%s\n\n",
                 cfdata->exit);
        fwrite(buffer, sizeof(char), strlen(buffer), f);
     }
   if (cfdata->signal)
     {
        snprintf(buffer, sizeof(buffer), "Error Signal Information:\n\t%s\n\n",
                 cfdata->signal);
        fwrite(buffer, sizeof(char), strlen(buffer), f);
     }

   if (cfdata->read) read_length = cfdata->read->size;

   if (read_length)
     {
        tlen = 0;
        for (i = 0; cfdata->read->lines[i].line; i++)
          tlen += cfdata->read->lines[i].size + 2;
        text = alloca(tlen + 1);
        text[0] = 0;
        for (i = 0; cfdata->read->lines[i].line; i++)
          {
             strcat(text, "\t");
             strcat(text, cfdata->read->lines[i].line);
             strcat(text, "\n");
          }
        snprintf(buffer, sizeof(buffer), "Output Data:\n%s\n\n", text);
        fwrite(buffer, sizeof(char), strlen(buffer), f);
     }
   else
     {
        snprintf(buffer, sizeof(buffer), "Output Data:\n\tThere was no output\n\n");
        fwrite(buffer, sizeof(char), strlen(buffer), f);
     }

   /* Reusing this var */
   read_length = 0;
   if (cfdata->error) read_length = cfdata->error->size;

   if (read_length)
     {
        tlen = 0;
        for (i = 0; cfdata->error->lines[i].line; i++)
          tlen += cfdata->error->lines[i].size + 1;
        text = alloca(tlen + 1);
        text[0] = 0;
        for (i = 0; cfdata->error->lines[i].line; i++)
          {
             strcat(text, "\t");
             strcat(text, cfdata->error->lines[i].line);
             strcat(text, "\n");
          }
        snprintf(buffer, sizeof(buffer), "Error Logs:\n%s\n", text);
        fwrite(buffer, sizeof(char), strlen(buffer), f);
     }
   else
     {
        snprintf(buffer, sizeof(buffer), "Error Logs:\n\tThere was no error message\n");
        fwrite(buffer, sizeof(char), strlen(buffer), f);
     }

   fclose(f);
}

E_API const Eina_List *
e_exec_desktop_instances_find(const Efreet_Desktop *desktop)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(desktop, NULL);
   return eina_hash_find(e_exec_instances, desktop->orig_path ?: desktop->name);
}

E_API const Eina_Hash *
e_exec_instances_get(void)
{
   return e_exec_instances;
}
